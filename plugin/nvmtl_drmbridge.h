/*
 * b80 (10-19): the DRM render node through IOKit for sandboxed processes.
 *
 * WindowServer's and the browsers' GPU sandboxes refuse open("/dev/dri/renderD128") with EPERM, so Mesa ANV found no
 * GPU there (WindowServer log 10-19: "open(renderD128, O_RDWR) FAILED errno 1"). Those sandboxes do allow IOAccelerator
 * user clients: NMXAccel opens the render node for us behind one (NMXGPU's IOAccelNMXDrmBridge).
 *
 * The Vulkan driver (libvulkan_intel.dylib) and libdrm are not changed. After the plugin loads them, their imports of
 * open/close/ioctl/fstat/dup/fcntl are pointed at the functions below (the indirect-symbol tables of those two images
 * only - the rest of the process keeps libc). Every call goes straight to libc first; only an open() of /dev/dri that
 * libc refuses with EPERM/EACCES becomes a bridge connection, represented to Mesa by a placeholder descriptor on
 * /dev/null (readable in every sandbox). ioctls on a placeholder become IOConnectCallMethod; everything else is libc.
 * NVMTL_DRM_BRIDGE=0 turns the hook off; =force uses the bridge even where /dev/dri opens.
 */
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdio.h>

#define NMXBR_MAX_FD 4096
static io_connect_t g_br_conn[NMXBR_MAX_FD];
static char *g_br_path[NMXBR_MAX_FD];
static pthread_mutex_t g_br_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_br_mode = -1;            /* 0 off, 1 on refusal, 2 always */
static unsigned long g_br_opens, g_br_ioctls, g_br_fail;

static int nmxbr_mode(void)
{
    if (g_br_mode < 0) {
        const char *e = getenv("NVMTL_DRM_BRIDGE");
        g_br_mode = !e ? 1 : (e[0] == '0' ? 0 : (!strcmp(e, "force") ? 2 : 1));
    }
    return g_br_mode;
}

/* connection refcounts: dup()ed placeholders share one connection (one DRM file, as a dup of a real fd would) */
static io_connect_t g_br_refc_conn[64]; static int g_br_refc[64];
static void nmxbr_ref(io_connect_t c, int d)
{
    for (int i = 0; i < 64; i++) if (g_br_refc_conn[i] == c) {
        g_br_refc[i] += d;
        if (g_br_refc[i] <= 0) { g_br_refc_conn[i] = 0; g_br_refc[i] = 0; IOServiceClose(c); }
        return; }
    if (d > 0) for (int i = 0; i < 64; i++) if (!g_br_refc_conn[i]) { g_br_refc_conn[i] = c; g_br_refc[i] = d; return; }
    if (d < 0) IOServiceClose(c);
}

static io_connect_t nmxbr_conn(int fd)
{
    if (fd < 0 || fd >= NMXBR_MAX_FD) return 0;
    return g_br_conn[fd];
}

/* b251/b252: Mesa's anonymous files (os_create_anonymous_file: mkostemp in $XDG_RUNTIME_DIR, then ftruncate + mmap
   MAP_SHARED). Where the sandbox refuses every folder: a POSIX shared-memory object (b251). Where it refuses that too
   (11-02: iconservicesagent, Brave's GPU process): a placeholder descriptor on /dev/null whose ftruncate allocates memory
   and whose mmap maps that same memory again (mach_vm_remap, shared) - what Mesa needs of the file: one size, any
   number of shared mappings. fstat reports the size; dup/close follow the descriptor. */
#include <mach/mach_vm.h>
#include <mach/vm_param.h>
typedef struct { mach_vm_address_t mem; mach_vm_size_t size; int ref; } nmxbr_anon;
static nmxbr_anon *g_anon[NMXBR_MAX_FD];
static nmxbr_anon *nmxbr_anon_of(int fd) { return fd >= 0 && fd < NMXBR_MAX_FD ? g_anon[fd] : NULL; }
static int nmxbr_anon_placeholder(int cloexec, int e)
{
    const int fd = open("/dev/null", O_RDWR | (cloexec ? O_CLOEXEC : 0));
    if (fd < 0 || fd >= NMXBR_MAX_FD) { if (fd >= 0) close(fd); errno = e; return -1; }
    nmxbr_anon *a = calloc(1, sizeof *a); if (!a) { close(fd); errno = ENOMEM; return -1; }
    a->ref = 1;
    pthread_mutex_lock(&g_br_lock); g_anon[fd] = a; pthread_mutex_unlock(&g_br_lock);
    static int said; if (said++ < 3) nvlog("drm bridge: anon file in memory (placeholder fd %d) - no folder and no shared memory here", fd);
    return fd;
}
static void nmxbr_anon_unref(nmxbr_anon *a)   /* g_br_lock held */
{
    if (a && --a->ref <= 0) { if (a->mem) mach_vm_deallocate(mach_task_self(), a->mem, a->size); free(a); }
}
static int nmxbr_anon_shm(int cloexec, int e)
{
    static _Atomic unsigned seq;
    char nm[32]; snprintf(nm, sizeof nm, "/nvmtl%d.%u", (int)getpid(), atomic_fetch_add(&seq, 1));
    const int fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return nmxbr_anon_placeholder(cloexec, e);
    static int said; if (said++ < 3) nvlog("drm bridge: anon file refused here (%s) - shared memory %s", strerror(e), nm);
    shm_unlink(nm);
    if (cloexec) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
/* b253: a sandbox may let a file be created but not sized (ftruncate is checked as file-write-data): such a file is
   given up at once (closed, removed) and the fallbacks used instead */
static int nmxbr_sizable(int fd, const char *path)
{
    if (ftruncate(fd, 4096) == 0) { ftruncate(fd, 0); return 1; }
    const int e = errno; close(fd); unlink(path);
    static int said; if (said++ < 3) nvlog("drm bridge: anon file created but cannot be sized here (%s)", strerror(e));
    errno = e; return 0;
}
static int nmxbr_hook_mkostemp(char *t, int flags)
{
    const int fd = mkostemp(t, flags);
    if (fd >= 0) { if (nmxbr_sizable(fd, t)) return fd; return nmxbr_anon_shm((flags & O_CLOEXEC) != 0, errno); }
    if (errno != EPERM && errno != EACCES && errno != ENOENT && errno != EROFS) return fd;
    return nmxbr_anon_shm((flags & O_CLOEXEC) != 0, errno);
}
static int nmxbr_hook_mkstemp(char *t)
{
    const int fd = mkstemp(t);
    if (fd >= 0) { if (nmxbr_sizable(fd, t)) return fd; return nmxbr_anon_shm(0, errno); }
    if (errno != EPERM && errno != EACCES && errno != ENOENT && errno != EROFS) return fd;
    return nmxbr_anon_shm(0, errno);
}
static int nmxbr_hook_ftruncate(int fd, off_t len)
{
    pthread_mutex_lock(&g_br_lock);
    nmxbr_anon *a = nmxbr_anon_of(fd);
    if (!a) { pthread_mutex_unlock(&g_br_lock); return ftruncate(fd, len); }
    int r = 0;
    const mach_vm_size_t sz = round_page((mach_vm_size_t)(len > 0 ? len : 0));
    if (sz > a->size) {   /* grow: new zeroed memory, the old contents copied (Mesa sizes it once, before mapping) */
        mach_vm_address_t m = 0;
        if (mach_vm_allocate(mach_task_self(), &m, sz, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { errno = ENOMEM; r = -1; }
        else {
            if (a->mem) { memcpy((void *)m, (void *)a->mem, a->size); mach_vm_deallocate(mach_task_self(), a->mem, a->size); }
            a->mem = m; a->size = sz;
        }
    }
    pthread_mutex_unlock(&g_br_lock);
    return r;
}
static void *nmxbr_hook_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    pthread_mutex_lock(&g_br_lock);
    nmxbr_anon *a = nmxbr_anon_of(fd);
    if (!a) { pthread_mutex_unlock(&g_br_lock); return mmap(addr, len, prot, flags, fd, off); }
    if (off < 0 || !a->mem || (mach_vm_size_t)off + len > a->size) { pthread_mutex_unlock(&g_br_lock); errno = ENXIO; return MAP_FAILED; }
    mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)addr; vm_prot_t cur, max;
    const kern_return_t kr = mach_vm_remap(mach_task_self(), &at, round_page(len), 0,
                                           (flags & MAP_FIXED) ? (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) : VM_FLAGS_ANYWHERE,
                                           mach_task_self(), a->mem + (mach_vm_address_t)off, (flags & MAP_PRIVATE) ? TRUE : FALSE,
                                           &cur, &max, VM_INHERIT_SHARE);
    pthread_mutex_unlock(&g_br_lock);
    if (kr != KERN_SUCCESS) { errno = ENOMEM; return MAP_FAILED; }
    mach_vm_protect(mach_task_self(), at, round_page(len), FALSE,
                    ((prot & PROT_READ) ? VM_PROT_READ : 0) | ((prot & PROT_WRITE) ? VM_PROT_WRITE : 0) | ((prot & PROT_EXEC) ? VM_PROT_EXECUTE : 0));
    return (void *)(uintptr_t)at;
}

static int nmxbr_open(const char *path)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("NMXAccel"));
    if (!svc) { errno = ENODEV; return -1; }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0x4E560000u | 2u, &c);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        g_br_fail++;
        nvlog("drm bridge: IOServiceOpen(NMXAccel, bridge) -> 0x%x in %s - the new NMXGPU/NMXAccel kexts are needed", kr, getprogname());
        errno = EPERM; return -1;
    }
    uint64_t out = 0; uint32_t n = 1;
    kr = IOConnectCallScalarMethod(c, 0, NULL, 0, &out, &n);
    if (kr != KERN_SUCCESS || out) {
        nvlog("drm bridge: opening the render node over IOKit failed (kr 0x%x, errno %llu)", kr, (unsigned long long)out);
        IOServiceClose(c); errno = kr ? EIO : (int)out; return -1;
    }
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fd >= NMXBR_MAX_FD) { if (fd >= 0) close(fd); IOServiceClose(c); errno = EMFILE; return -1; }
    pthread_mutex_lock(&g_br_lock);
    g_br_conn[fd] = c; free(g_br_path[fd]); g_br_path[fd] = strdup(path);
    nmxbr_ref(c, 1);
    pthread_mutex_unlock(&g_br_lock);
    if (g_br_opens++ < 16)
        nvlog("drm bridge: %s opened through NMXAccel (IOKit) in %s - placeholder fd %d", path, getprogname(), fd);
    return fd;
}

static int nmxbr_hook_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    const int dri = path && !strncmp(path, "/dev/dri/", 9);
    if (dri && nmxbr_mode() == 2) return nmxbr_open(path);
    int fd = open(path, flags, mode);
    if (fd < 0 && dri && (errno == EPERM || errno == EACCES) && nmxbr_mode() == 1) {
        static int said; if (!said++) nvlog("drm bridge: open(%s) refused here (%s) - using the IOKit bridge", path, strerror(errno));
        return nmxbr_open(path);
    }
    return fd;
}

static int nmxbr_hook_ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    io_connect_t c = nmxbr_conn(fd);
    if (!c) return ioctl(fd, req, arg);
    const uint32_t cmd = (uint32_t)req;
    const size_t len = IOCPARM_LEN(cmd);
    uint64_t in[2] = { cmd, (uint64_t)(uintptr_t)arg }, out = 0; uint32_t nout = 1;
    size_t olen = (cmd & IOC_OUT) ? len : 0;
    kern_return_t kr = IOConnectCallMethod(c, 1, in, 2, (cmd & IOC_IN) && len ? arg : NULL, (cmd & IOC_IN) ? len : 0,
                                           &out, &nout, olen ? arg : NULL, olen ? &olen : NULL);
    g_br_ioctls++;
    if (kr != KERN_SUCCESS) {
        static int said; if (said++ < 8) nvlog("drm bridge: ioctl 0x%08x over IOKit -> kr 0x%x", cmd, kr);
        errno = EIO; return -1;
    }
    if (out) {
        /* b81: say which ioctl the kernel refused (vkCreateDevice -> -3 in WindowServer, 10-19) */
        static int said; if (said++ < 40) nvlog("drm bridge: ioctl 0x%08x (nr 0x%02x, %zu B) -> errno %d", cmd, cmd & 0xff, len, (int)out);
        errno = (int)out; return -1;
    }
    return 0;
}

/* b81: Mesa writes why a call failed to stderr, which goes nowhere in WindowServer. Around a call, stderr is a pipe and
   what arrives is logged here. Only in a process that uses the bridge. */
static int nmxbr_err_begin(int *saved, int p[2])
{
    if (!g_br_opens || pipe(p)) return 0;
    fcntl(p[0], F_SETFL, O_NONBLOCK); fcntl(p[1], F_SETFL, O_NONBLOCK);
    fflush(stderr);
    *saved = dup(2);
    if (*saved < 0) { close(p[0]); close(p[1]); return 0; }
    dup2(p[1], 2);
    return 1;
}
static void nmxbr_err_end(int saved, int p[2], const char *what)
{
    fflush(stderr);
    dup2(saved, 2); close(saved); close(p[1]);
    char buf[8192]; ssize_t n = read(p[0], buf, sizeof buf - 1);
    close(p[0]);
    if (n <= 0) { nvlog("drm bridge: %s: Mesa wrote nothing to stderr", what); return; }
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) nvlog("drm bridge: %s: mesa: %s", what, line);
}

static int nmxbr_hook_close(int fd)
{
    io_connect_t c = 0;
    pthread_mutex_lock(&g_br_lock);
    if (fd >= 0 && fd < NMXBR_MAX_FD && g_br_conn[fd]) {
        c = g_br_conn[fd]; g_br_conn[fd] = 0; free(g_br_path[fd]); g_br_path[fd] = NULL;
        nmxbr_ref(c, -1);
    }
    if (fd >= 0 && fd < NMXBR_MAX_FD && g_anon[fd]) { nmxbr_anon_unref(g_anon[fd]); g_anon[fd] = NULL; }   /* b252 */
    pthread_mutex_unlock(&g_br_lock);
    return close(fd);
}

static int nmxbr_hook_fstat(int fd, struct stat *st)
{
    const char *p = NULL;
    pthread_mutex_lock(&g_br_lock);
    if (fd >= 0 && fd < NMXBR_MAX_FD && g_br_conn[fd]) p = g_br_path[fd];
    char path[256]; if (p) strlcpy(path, p, sizeof path);
    pthread_mutex_unlock(&g_br_lock);
    if (p) return stat(path, st);      /* the node itself, as libdrm expects of a DRM fd */
    pthread_mutex_lock(&g_br_lock); nmxbr_anon *a = nmxbr_anon_of(fd); const mach_vm_size_t asz = a ? a->size : 0; pthread_mutex_unlock(&g_br_lock);
    if (a) { const int r = fstat(fd, st); if (!r) { st->st_mode = S_IFREG | 0600; st->st_size = (off_t)asz; } return r; }   /* b252 */
    return fstat(fd, st);
}

static int nmxbr_track_dup(int from, int to)
{
    if (to < 0) return to;
    pthread_mutex_lock(&g_br_lock);
    io_connect_t c = nmxbr_conn(from);
    if (c && to < NMXBR_MAX_FD) {
        g_br_conn[to] = c; free(g_br_path[to]); g_br_path[to] = g_br_path[from] ? strdup(g_br_path[from]) : NULL;
        nmxbr_ref(c, 1);
    }
    if (to < NMXBR_MAX_FD && nmxbr_anon_of(from)) { if (g_anon[to]) nmxbr_anon_unref(g_anon[to]); g_anon[to] = g_anon[from]; g_anon[to]->ref++; }   /* b252 */
    pthread_mutex_unlock(&g_br_lock);
    return to;
}
static int nmxbr_hook_dup(int fd) { return nmxbr_track_dup(fd, dup(fd)); }
static int nmxbr_hook_fcntl(int fd, int cmd, ...)
{
    va_list ap; va_start(ap, cmd); intptr_t a = va_arg(ap, intptr_t); va_end(ap);
    int r = fcntl(fd, cmd, a);
    if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && r >= 0) return nmxbr_track_dup(fd, r);
    return r;
}

/* b104 (Phase 4): app sandboxes (TextEdit 10-20: "opendir(/dev/dri) FAILED errno 1") refuse even LISTING /dev/dri, so
   libdrm's drmGetDevices2 found no node and never reached the open() above: zero Vulkan devices, the app stayed on the
   CPU. WindowServer's sandbox lists the folder, so it never hit this. stat() of the nodes is allowed in those sandboxes,
   so the listing is rebuilt: an empty file per node that stats as a character device, in a folder of the process's own
   temp space, and opendir returns that folder. libdrm reads only the names, then stats and opens the real
   /dev/dri/<name> - the open is refused and becomes a bridge connection as before. */
/* b254: the /dev/dri listing for processes that may not read /dev/dri. b104-b253 rebuilt it as empty files in a temp
   folder, but Brave's GPU sandbox may neither write nor create there (11-02: "deny file-write-data .../nvmtl-dri/card0",
   and still no GPU after removing the folder). Now nothing is written: opendir returns a real, readable directory handle
   (the driver's own folder) marked as a stand-in, and readdir on it hands out the node names that stat as character
   devices under /dev/dri (stat is allowed where open is not). closedir unmarks it. */
#define NMXBR_FAKE_DIRS 8
static struct { DIR *d; int pos, n; char names[6][16]; struct dirent ent; } g_fake[NMXBR_FAKE_DIRS];
static DIR *nmxbr_hook_opendir(const char *name)
{
    DIR *d = opendir(name);
    if (d || !name || nmxbr_mode() == 0) return d;
    const int e = errno;
    if ((strcmp(name, "/dev/dri") && strcmp(name, "/dev/dri/")) || (e != EPERM && e != EACCES)) return d;
    static const char *const nodes[] = { "card0", "card1", "renderD128", "renderD129", "renderD130", "renderD131" };
    char seen[160] = ""; int n = 0; char names[6][16];
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
        char real[64]; struct stat st;
        snprintf(real, sizeof real, "/dev/dri/%s", nodes[i]);
        if (stat(real, &st) == 0 && S_ISCHR(st.st_mode)) {
            strlcpy(names[n++], nodes[i], 16); strlcat(seen, " ", sizeof seen); strlcat(seen, nodes[i], sizeof seen);
        }
    }
    if (n) {
        d = opendir("/Library/GPUBundles/nvmtl");
        if (!d) d = opendir("/");
    }
    if (d) {
        pthread_mutex_lock(&g_br_lock);
        int slot = -1;
        for (int i = 0; i < NMXBR_FAKE_DIRS; i++) if (!g_fake[i].d) { slot = i; break; }
        if (slot < 0) { pthread_mutex_unlock(&g_br_lock); closedir(d); d = NULL; }
        else { g_fake[slot].d = d; g_fake[slot].pos = 0; g_fake[slot].n = n; memcpy(g_fake[slot].names, names, sizeof names);
               pthread_mutex_unlock(&g_br_lock); }
    }
    static int said; if (said++ < 4)
        nvlog("drm bridge: opendir(/dev/dri) refused here (%s) - stand-in listing:%s -> %s", strerror(e), n ? seen : " (no node stats)", d ? "ok" : "FAILED");
    if (!d) errno = e;
    return d;
}
static struct dirent *nmxbr_hook_readdir(DIR *d)
{
    pthread_mutex_lock(&g_br_lock);
    for (int i = 0; i < NMXBR_FAKE_DIRS; i++) if (d && g_fake[i].d == d) {
        struct dirent *r = NULL;
        if (g_fake[i].pos < g_fake[i].n) {
            struct dirent *x = &g_fake[i].ent; memset(x, 0, sizeof *x);
            x->d_ino = 1000 + (uint64_t)g_fake[i].pos; x->d_type = DT_CHR; x->d_reclen = sizeof *x;
            strlcpy(x->d_name, g_fake[i].names[g_fake[i].pos], sizeof x->d_name); x->d_namlen = (uint16_t)strlen(x->d_name);
            g_fake[i].pos++; r = x;
        }
        pthread_mutex_unlock(&g_br_lock);
        return r;
    }
    pthread_mutex_unlock(&g_br_lock);
    return readdir(d);
}
static int nmxbr_hook_closedir(DIR *d)
{
    pthread_mutex_lock(&g_br_lock);
    for (int i = 0; i < NMXBR_FAKE_DIRS; i++) if (d && g_fake[i].d == d) g_fake[i].d = NULL;
    pthread_mutex_unlock(&g_br_lock);
    return closedir(d);
}

/* point one image's imports of the names below at the hooks (its indirect symbol tables, as dyld left them) */
static const struct { const char *name; void *fn; } g_br_hooks[] = {
    { "_open", (void *)nmxbr_hook_open }, { "_ioctl", (void *)nmxbr_hook_ioctl }, { "_close", (void *)nmxbr_hook_close },
    { "_fstat$INODE64", (void *)nmxbr_hook_fstat },
    { "_dup", (void *)nmxbr_hook_dup }, { "_fcntl", (void *)nmxbr_hook_fcntl },
    { "_opendir$INODE64", (void *)nmxbr_hook_opendir }, { "_opendir", (void *)nmxbr_hook_opendir },   /* b104 */
    { "_readdir$INODE64", (void *)nmxbr_hook_readdir }, { "_readdir", (void *)nmxbr_hook_readdir },      /* b254 */
    { "_closedir", (void *)nmxbr_hook_closedir },
    { "_mkostemp", (void *)nmxbr_hook_mkostemp }, { "_mkstemp", (void *)nmxbr_hook_mkstemp },          /* b251 */
    { "_ftruncate", (void *)nmxbr_hook_ftruncate }, { "_mmap", (void *)nmxbr_hook_mmap },                  /* b252 */
};
static int nmxbr_rebind_image(const struct mach_header_64 *mh, intptr_t slide)
{
    const struct segment_command_64 *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysym = NULL;
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT)) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) symtab = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysym = (const void *)lc;
    }
    if (!linkedit || !symtab || !dysym || !dysym->nindirectsyms) return 0;
    const uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *syms = (const struct nlist_64 *)(base + symtab->symoff);
    const char *strs = (const char *)(base + symtab->stroff);
    const uint32_t *ind = (const uint32_t *)(base + dysym->indirectsymoff);
    int done = 0;
    lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *seg = (const void *)lc;
        if (strcmp(seg->segname, "__DATA") && strcmp(seg->segname, "__DATA_CONST") && strcmp(seg->segname, "__AUTH_CONST")) continue;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
            const uint32_t type = sect->flags & SECTION_TYPE;
            if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
            const uint32_t *idx = ind + sect->reserved1;
            void **ptrs = (void **)((uintptr_t)slide + sect->addr);
            const uint64_t n = sect->size / sizeof(void *);
            for (uint64_t j = 0; j < n; j++) {
                const uint32_t si = idx[j];
                if (si == INDIRECT_SYMBOL_ABS || si == INDIRECT_SYMBOL_LOCAL || si == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                if (si >= symtab->nsyms) continue;
                const char *name = strs + syms[si].n_un.n_strx;
                for (size_t k = 0; k < sizeof g_br_hooks / sizeof g_br_hooks[0]; k++) {
                    if (strcmp(name, g_br_hooks[k].name)) continue;
                    if (ptrs[j] == g_br_hooks[k].fn) break;
                    vm_address_t page = (vm_address_t)&ptrs[j] & ~(vm_address_t)(vm_page_size - 1);
                    kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
                    if (kr != KERN_SUCCESS) { nvlog("drm bridge: cannot write the import table (%s, 0x%x)", name, kr); break; }
                    ptrs[j] = g_br_hooks[k].fn; done++;
                    if (!strcmp(seg->segname, "__DATA_CONST") || !strcmp(seg->segname, "__AUTH_CONST"))
                        vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ);
                    break;
                }
            }
        }
    }
    return done;
}

/* after the plugin dlopen()ed the Vulkan driver: hook libvulkan_intel and libdrm from our own folder */
static void nmxbr_install(void)
{
    if (nmxbr_mode() == 0) { nvlog("drm bridge: off (NVMTL_DRM_BRIDGE=0)"); return; }
    int images = 0, slots = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (!n || !strstr(n, "/GPUBundles/nvmtl/")) continue;
        if (!strstr(n, "libvulkan_intel") && !strstr(n, "libdrm")) continue;
        const struct mach_header *mh = _dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64) continue;
        const int k = nmxbr_rebind_image((const struct mach_header_64 *)mh, _dyld_get_image_vmaddr_slide(i));
        nvlog("drm bridge: %d import slot(s) in %s", k, n);   /* b250: per image (0 = its imports are not where we look) */
        slots += k; images++;
    }
    nvlog("drm bridge: %d import slot(s) hooked in %d image(s) in %s (mode %s)", slots, images, getprogname(),
          nmxbr_mode() == 2 ? "always" : "when /dev/dri is refused");
}
