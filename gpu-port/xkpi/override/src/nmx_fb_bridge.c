/*
 * The display bridge, DRM side: lights the GPU's outputs with one scan-out buffer that macOS draws into.
 *
 * It is an in-kernel DRM client, the same mechanism Linux's fbdev emulation uses: probe the connectors, pick each
 * output's preferred mode, create one XRGB8888 buffer as large as the largest mode, point every lit CRTC at it and
 * commit. The buffer's CPU view is the GPU aperture (a CPU-visible, contiguous VRAM buffer read through the BAR), so
 * the kext publishes its physical range as the IOFramebuffer's VRAM and WindowServer draws straight into what the
 * display engine scans out. Smaller outputs show the top-left of the buffer (mirrored), as fbdev does.
 *
 * Any step that fails leaves no client behind and reports why; the kext then publishes no framebuffer.
 */
#include <linux/kernel.h>
void nmx_step(const char *what);
int nmx_boot_snapshot_paint(void *va, uint32_t pitch, uint32_t width, uint32_t height);
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/iosys-map.h>

#include <drm/drm_client.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_crtc.h>
#include <drm/drm_gem.h>
#include <drm/drm_file.h>
#include <uapi/drm/drm_mode.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_modeset_lock.h>

/* the DRM core's own framebuffer calls (drm_crtc_internal.h, not a public header); drm_client.c uses them the same way */
int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_rmfb(struct drm_device *dev, u32 fb_id, struct drm_file *file_priv);
/* b94: the legacy cursor call (drm_plane.c), which X11 and Wayland use for the hardware cursor; on i915 it takes the
   cursor plane's fast path (intel_legacy_cursor_update), so a move does not wait for a full atomic commit */
int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
#include <linux/mutex.h>

#include <sys/bus.h>

#include "nmx_xnu.h"
#include "nmx_port.h"

struct nmx_fb {
    struct drm_client_dev client;
    struct drm_client_buffer *buffer;
    struct iosys_map map;
    int mapped;
    struct nmx_fb_info info;
    /* 10-18 (Retina sizes): a smaller desktop shown full-screen by the display engine's panel fitter */
    uint32_t cur_w, cur_h;              /* the size macOS draws at (0 = native) */
    struct drm_framebuffer *scaled;     /* the scan-out buffer seen as cur_w x cur_h (same memory, same pitch) */
    /* b94: hardware cursor - two ARGB8888 images on the cursor plane; what macOS asked for is kept so it can be put
       back after any mode set (drm_client_modeset_commit turns every non-primary plane off) */
    struct drm_client_buffer *cur_buf[2];
    struct iosys_map cur_map[2];
    int cur_mapped[2];
    u32 cur_handle[2];
    int cur_ready;
    struct mutex cur_lock;
    int cur_img, cur_vis;               /* wanted: image (-1 none yet), visible */
    int32_t cur_x, cur_y;
    int cur_shown;                      /* image the plane holds now, -1 = off */
    int cur_hidden;                     /* b95: the plane's image is parked off screen */
    unsigned cur_errs;
    /* b125 (R2): double-buffered scan-out. buf2 is a second buffer like 'buffer'; the accelerator copies each frame into
       the buffer that is NOT on the panel and the primary plane is flipped to it at vblank (atomic commit of the primary
       plane only - the cursor plane keeps its state). front = buffer on the panel (0 = 'buffer', 1 = buf2). */
    struct drm_client_buffer *buf2;
    struct iosys_map map2;
    int mapped2;
    struct drm_framebuffer *scaled2;    /* buf2 seen as cur_w x cur_h */
    int front;
    struct mutex flip_lock;
    unsigned long flips, flip_errs;
    /* b126: the flip is NONBLOCKING; the commits still in flight are kept here and waited for only before the CPU
       writes into the buffer they are taking off the panel */
    struct drm_crtc_commit *pend[4];
    int npend;
    unsigned long flip_waits;
    /* b164: boot screen - the first mode set waits until WindowServer opens the framebuffer (nmx_fb_go). Until then the
       panel keeps scanning the firmware framebuffer, which i915 took over at boot (patch_i915_initial_fb.py) and the
       kernel's boot screen (Apple logo and progress bar) still draws into: no black gap, no second, smaller logo. */
    int deferred;
    int pending_back;                   /* b167: a frame was finished in the back buffer while the logo was held */
    int psr_on;                         /* b170: PSR switched on by the bridge */
    volatile int go_on_flip;            /* b174: hand over with the next flip (the restarted WindowServer's frame) */
    const char *go_why;
    unsigned long psr_flips;            /* b170: flips since presentation last went through the flip path */
};

/* b164: set by the kext before nmx_fb_open (boot-arg -nmxnodefer = 0: commit at once, as before) */
int nmx_fb_defer_wanted = 1;
/* b170: panel self refresh once frames flip (boot-arg -nmxnopsr = 0: never) */
int nmx_fb_psr_wanted = 1;
int nmx_i915_psr(struct drm_device *drm, int on);
#define NMX_PSR_AFTER_FLIPS 300         /* ~5 s of accelerated frames before PSR goes on */

#define NMX_CUR_DIM 256                 /* Tiger Lake cursor plane: 64, 128 or 256 square; macOS cursors fit in 256 */
static void cursor_reapply(struct nmx_fb *fb);
static int bridge_go(struct nmx_fb *fb, const char *why);
void nmx_steps_go_quiet(const char *why);   /* b213, nmx_kext.cpp */

/* one bridge per PCI function the port drives */
#define MAX_FB 8
static struct { void *dev; struct nmx_fb *fb; } fbs[MAX_FB];

/*
 * amdgpu may put scan-out buffers in system memory on APUs (scatter-gather display); the bridge needs them in
 * contiguous VRAM so one physical range describes the whole frame. Set before the driver initialises its display.
 */
extern int amdgpu_sg_display;

void nmx_fb_before_attach(void)
{
    amdgpu_sg_display = 0;
}

/* the DRM device of the driver attached to the PCI function 'dev' (amdgpu keeps it as the PCI driver data) */
static struct drm_device *drm_of(device_t dev)
{
    device_t *kids = NULL;
    int n = 0;
    struct drm_device *drm = NULL;
    if (device_get_children(dev, &kids, &n) != 0)
        return NULL;
    for (int i = 0; i < n && drm == NULL; i++) {
        if (!device_is_attached(kids[i]))
            continue;
        struct pci_dev *pdev = device_get_softc(kids[i]);
        if (pdev != NULL)
            drm = pci_get_drvdata(pdev);
    }
    free(kids, M_TEMP);
    return drm;
}

/* the DRM device behind an attached PCI function, for the kext (nmxgtawake) */
void *nmx_drm_of(void *dev) { return drm_of((device_t)dev); }

/*
 * b180 (external displays, step 1: mirroring). The laptop panel (eDP) leads: the desktop and the scan-out buffers have
 * its size, and an external output (HDMI / DisplayPort / USB-C) shows the same picture:
 *   - desktop scaled (cur_w x cur_h < panel): the external gets a mode of exactly that size if it offers one (full
 *     screen, same picture), else its own mode at most panel-sized showing the buffer 1:1 (desktop in the top-left);
 *   - desktop native: the external's preferred mode up to the panel size, buffer 1:1.
 * Only the panel uses the "same timing, smaller source" trick (i915 scales an eDP pipe with the panel fitter; an HDMI
 * mode edited that way would be a non-standard timing). If a mode set with an external fails, it is repeated with the
 * panel alone, so a monitor can never take the laptop's own screen down. -nmxnoext: externals are never lit.
 */
int nmx_fb_ext_wanted = 1;
void nmx_i915_hpd_poll(struct drm_device *drm);
/* b181: turn on polled detection of external outputs (no HPD interrupt arrives on this port) */
static void bridge_poll_outputs(struct nmx_fb *fb, const char *when)
{
    if (!nmx_fb_ext_wanted)
        return;
    nmx_i915_hpd_poll(fb->client.dev);
    printf("NullMoth display: external outputs checked every 10 s (%s; -nmxnoext: off)\n", when);
}
static int ms_internal(const struct drm_mode_set *ms)
{
    if (ms->num_connectors == 0 || ms->connectors[0] == NULL)
        return 0;
    int t = ms->connectors[0]->connector_type;
    return t == DRM_MODE_CONNECTOR_eDP || t == DRM_MODE_CONNECTOR_LVDS || t == DRM_MODE_CONNECTOR_DSI;
}
/* the view of buffer 'target' (0/1) this output scans: the cur_w x cur_h view when its mode has that size, else the
   whole buffer */
static struct drm_framebuffer *ms_view(struct nmx_fb *fb, const struct drm_mode_set *ms, int target)
{
    int sized = fb->cur_w && fb->cur_h && ms->mode != NULL && ms->mode->hdisplay == fb->cur_w && ms->mode->vdisplay == fb->cur_h;
    if (sized) {
        struct drm_framebuffer *v = target ? fb->scaled2 : fb->scaled;
        if (v != NULL)
            return v;
    }
    if (target && fb->buf2 != NULL)
        return fb->buf2->fb;
    return fb->buffer->fb;
}
/* caller holds client.modeset_mutex: this output is not lit (mode, buffer and connectors released) */
static void ms_drop(struct nmx_fb *fb, struct drm_mode_set *ms)
{
    drm_mode_destroy(fb->client.dev, ms->mode);
    ms->mode = NULL;
    ms->fb = NULL;
    for (int i = 0; i < ms->num_connectors; i++) {
        drm_connector_put(ms->connectors[i]);
        ms->connectors[i] = NULL;
    }
    ms->num_connectors = 0;
}
/* caller holds client.modeset_mutex: an external output gets the desktop size if it offers that mode */
static void ms_external_mode(struct nmx_fb *fb, struct drm_mode_set *ms)
{
    struct drm_device *drm = fb->client.dev;
    struct drm_connector *c = ms->connectors[0];
    struct drm_display_mode *m, *best = NULL;
    if (!fb->cur_w || !fb->cur_h || c == NULL || (ms->mode->hdisplay == fb->cur_w && ms->mode->vdisplay == fb->cur_h))
        return;
    mutex_lock(&drm->mode_config.mutex);
    list_for_each_entry(m, &c->modes, head)
        if (m->hdisplay == fb->cur_w && m->vdisplay == fb->cur_h && !(m->flags & DRM_MODE_FLAG_INTERLACE) &&
            (best == NULL || drm_mode_vrefresh(m) > drm_mode_vrefresh(best)))
            best = m;
    struct drm_display_mode *dup = best ? drm_mode_duplicate(drm, best) : NULL;
    mutex_unlock(&drm->mode_config.mutex);
    if (dup != NULL) {
        drm_mode_destroy(drm, ms->mode);
        ms->mode = dup;
    }
}
static void bridge_log_outputs(struct nmx_fb *fb, const char *when)
{
    struct drm_mode_set *ms;
    int n = 0;
    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client) {
        if (ms->num_connectors == 0 || ms->connectors[0] == NULL)
            continue;
        n++;
        struct drm_framebuffer *v = ms->fb;
        printf("NullMoth display: output %s (%s): %s%ux%u@%d, picture %ux%u\n", ms->connectors[0]->name, when,
               ms->mode ? "" : "OFF ", ms->mode ? ms->mode->hdisplay : 0, ms->mode ? ms->mode->vdisplay : 0,
               ms->mode ? drm_mode_vrefresh(ms->mode) : 0, v ? v->width : 0, v ? v->height : 0);
    }
    mutex_unlock(&fb->client.modeset_mutex);
    if (n == 0)
        printf("NullMoth display: no output connected (%s)\n", when);
}

/* points every lit output at the current desktop size: native, or cur_w x cur_h scaled up by the panel fitter */
static void bridge_point_outputs(struct nmx_fb *fb)
{
    struct drm_mode_set *ms;
    int scaled = fb->scaled != NULL && fb->cur_w && fb->cur_h;
    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client) {
        if (ms->mode == NULL)
            continue;
        if (!ms_internal(ms)) {
            if (!nmx_fb_ext_wanted) { ms_drop(fb, ms); continue; }
            ms_external_mode(fb, ms);
        } else if (scaled && fb->cur_w <= ms->mode->hdisplay && fb->cur_h <= ms->mode->vdisplay) {
            /* same timing, smaller source: i915 keeps the panel's mode and scales the pipe up to it */
            ms->mode->hdisplay = fb->cur_w;
            ms->mode->vdisplay = fb->cur_h;
            drm_mode_set_name(ms->mode);
        }
        ms->fb = ms_view(fb, ms, fb->front);
    }
    mutex_unlock(&fb->client.modeset_mutex);
}

/* b180: commit the outputs; if that fails while an external is lit, commit again with the panel alone */
static int bridge_commit(struct nmx_fb *fb, const char *why)
{
    int err = drm_client_modeset_commit(&fb->client);
    if (err == 0)
        return 0;
    struct drm_mode_set *ms;
    int dropped = 0;
    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client)
        if (ms->mode != NULL && !ms_internal(ms)) {
            printf("NullMoth display: %s: mode set refused (%d) - external output %s left dark, panel alone\n", why, err,
                   ms->connectors[0] ? ms->connectors[0]->name : "?");
            ms_drop(fb, ms);
            dropped++;
        }
    mutex_unlock(&fb->client.modeset_mutex);
    return dropped ? drm_client_modeset_commit(&fb->client) : err;
}

/* a monitor plugged or unplugged: probe again and light what is there now with the same buffer */
static int bridge_hotplug(struct drm_client_dev *client)
{
    struct nmx_fb *fb = container_of(client, struct nmx_fb, client);
    if (fb->buffer == NULL || fb->deferred)     /* b164: no mode set before the boot screen hand-over */
        return 0;
    if (drm_client_modeset_probe(client, fb->info.width, fb->info.height) != 0)
        return 0;
    mutex_lock(&fb->flip_lock);
    bridge_point_outputs(fb);
    int err = bridge_commit(fb, "hotplug");
    mutex_unlock(&fb->flip_lock);
    bridge_log_outputs(fb, err ? "hotplug, mode set FAILED" : "hotplug");
    cursor_reapply(fb);
    return 0;
}

/*
 * 10-18 (Retina sizes): macOS draws its desktop at w x h (at most the native size) in the top-left of the same
 * scan-out buffer, with the same pitch; the outputs keep their native timing and the display engine scales the
 * smaller picture up to the panel. w == 0 (or the native size) goes back to 1:1. The buffer and its physical range
 * never change, so the IOFramebuffer's VRAM stays where WindowServer mapped it.
 */
int nmx_fb_set_size(void *dev, uint32_t w, uint32_t h)
{
    struct nmx_fb *fb = NULL;
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev)
            fb = fbs[i].fb;
    if (fb == NULL || fb->buffer == NULL)
        return ENODEV;
    if (w > fb->info.width || h > fb->info.height || (w && w < 640) || (h && h < 400))
        return EINVAL;
    if (w == fb->info.width && h == fb->info.height)
        w = h = 0;
    struct drm_device *drm = fb->client.dev;
    /* b125: a size change goes back to buffer 0; the second buffer's scaled view is made again on the next flip */
    mutex_lock(&fb->flip_lock);
    fb->front = 0;
    if (fb->scaled2) { drm_mode_rmfb(drm, fb->scaled2->base.id, fb->client.file); fb->scaled2 = NULL; }
    mutex_unlock(&fb->flip_lock);
    struct drm_framebuffer *old = fb->scaled, *nfb = NULL;
    if (w) {
        u32 handle = 0;
        struct drm_mode_fb_cmd2 req;
        int err = -drm_gem_handle_create(fb->client.file, fb->buffer->gem, &handle);
        if (err)
            return err;
        memset(&req, 0, sizeof(req));
        req.width = w;
        req.height = h;
        req.pixel_format = DRM_FORMAT_XRGB8888;
        req.handles[0] = handle;
        req.pitches[0] = fb->info.pitch;
        err = -drm_mode_addfb2(drm, &req, fb->client.file);
        drm_gem_handle_delete(fb->client.file, handle);
        if (err) {
            printf("NullMoth display: no %ux%u view of the scan-out buffer (%d)\n", w, h, err);
            return err;
        }
        nfb = drm_framebuffer_lookup(drm, fb->client.file, req.fb_id);
        if (nfb == NULL)
            return ENOENT;
        drm_framebuffer_put(nfb);       /* the file keeps it until rmfb */
    }
    /* fresh native modes, then the new size on top of them */
    int err = -drm_client_modeset_probe(&fb->client, fb->info.width, fb->info.height);
    if (!err) {
        fb->scaled = nfb;
        fb->cur_w = w;
        fb->cur_h = h;
        bridge_point_outputs(fb);
        if (fb->deferred) {
            /* b166: WindowServer sets its desktop size right after it opens us - that must not end the boot logo hold
               (b165 handed over 2 s after the first open). Remembered; the hand-over's mode set uses it. */
            printf("NullMoth display: desktop %ux%u noted, applied at the boot screen hand-over\n", w ? w : fb->info.width,
                   h ? h : fb->info.height);
        } else {
            nmx_step(w ? "bridge: scaled desktop commit" : "bridge: native desktop commit");
            err = -bridge_commit(fb, "desktop size");
            nmx_step("bridge: desktop size commit returned");
            bridge_log_outputs(fb, "desktop size");
        }
    }
    if (err) {
        printf("NullMoth display: desktop %ux%u refused (%d), back to %ux%u\n", w, h, err, old ? fb->cur_w : fb->info.width,
               old ? fb->cur_h : fb->info.height);
        fb->scaled = old;
        if (nfb)
            drm_mode_rmfb(drm, nfb->base.id, fb->client.file);
        (void)drm_client_modeset_probe(&fb->client, fb->info.width, fb->info.height);
        bridge_point_outputs(fb);
        bridge_commit(fb, "desktop size fallback");
        cursor_reapply(fb);
        return err;
    }
    if (old && old != nfb)
        drm_mode_rmfb(drm, old->base.id, fb->client.file);
    cursor_reapply(fb);
    printf("NullMoth display: desktop now %ux%u on a %ux%u panel\n", w ? w : fb->info.width, h ? h : fb->info.height,
           fb->info.width, fb->info.height);
    return 0;
}

static void bridge_free(struct nmx_fb *fb);

/* a userland KMS client (a DRM master) closed: put our scan-out back on the outputs */
static int bridge_restore(struct drm_client_dev *client)
{
    struct nmx_fb *fb = container_of(client, struct nmx_fb, client);
    if (fb->deferred)
        return 0;
    int r = bridge_commit(fb, "restore");
    cursor_reapply(fb);
    return r;
}

/*
 * The DRM device is going away (driver detach): the DRM core has taken the client off its list and hands it back
 * here to free. Once registered, this is the only place a bridge is freed.
 */
static void bridge_unregister(struct drm_client_dev *client)
{
    struct nmx_fb *fb = container_of(client, struct nmx_fb, client);
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].fb == fb) {
            fbs[i].dev = NULL;
            fbs[i].fb = NULL;
        }
    bridge_free(fb);
}

static const struct drm_client_funcs bridge_funcs = {
    .owner = NULL,
    .unregister = bridge_unregister,
    .restore = bridge_restore,
    .hotplug = bridge_hotplug,
};

static void bridge_free(struct nmx_fb *fb)
{
    for (int i = 0; i < 2; i++) {
        if (fb->cur_mapped[i])
            drm_client_buffer_vunmap(fb->cur_buf[i]);
        if (fb->cur_handle[i])
            drm_gem_handle_delete(fb->client.file, fb->cur_handle[i]);
        if (fb->cur_buf[i] != NULL)
            drm_client_framebuffer_delete(fb->cur_buf[i]);
    }
    if (fb->mapped2)
        drm_client_buffer_vunmap(fb->buf2);
    if (fb->buf2 != NULL)
        drm_client_framebuffer_delete(fb->buf2);
    if (fb->mapped)
        drm_client_buffer_vunmap(fb->buffer);
    if (fb->buffer != NULL)
        drm_client_framebuffer_delete(fb->buffer);
    drm_client_release(&fb->client);
    kfree(fb);
}

int nmx_fb_open(void *devp, struct nmx_fb_info *out)
{
    device_t dev = devp;
    struct drm_device *drm = drm_of(dev);
    struct drm_mode_set *ms;
    const struct drm_display_mode *best = NULL;
    int slot = -1, err;

    memset(out, 0, sizeof(*out));
    if (drm == NULL)
        return ENODEV;
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == NULL) { slot = i; break; }
    if (slot < 0)
        return ENOSPC;

    struct nmx_fb *fb = kzalloc(sizeof(*fb), GFP_KERNEL);
    if (fb == NULL)
        return ENOMEM;
    mutex_init(&fb->cur_lock);
    mutex_init(&fb->flip_lock);
    fb->cur_img = -1;
    fb->cur_shown = -1;
    err = -drm_client_init(drm, &fb->client, "nmxfb", &bridge_funcs);
    if (err) {
        printf("NullMoth display: DRM client refused (%d)\n", err);
        kfree(fb);
        return err;
    }
    err = -drm_client_modeset_probe(&fb->client, 0, 0);
    if (err) {
        printf("NullMoth display: probing the outputs failed (%d)\n", err);
        goto fail;
    }
    /* the buffer is as large as the panel's mode (b180: the built-in panel leads; before, the largest output did - a 4K
       TV at boot made a 4K desktop of which the panel showed a corner); without a panel, the largest output's */
    mutex_lock(&fb->client.modeset_mutex);
    for (int pass = 0; pass < 2 && best == NULL; pass++)
        drm_client_for_each_modeset(ms, &fb->client)
            if (ms->mode != NULL && (pass == 1 || ms_internal(ms)) && (best == NULL ||
                (u64)ms->mode->hdisplay * ms->mode->vdisplay > (u64)best->hdisplay * best->vdisplay))
                best = ms->mode;
    if (best != NULL) {
        out->width = best->hdisplay;
        out->height = best->vdisplay;
        out->pclk_khz = (uint32_t)best->clock;
        out->htotal = best->htotal;
        out->vtotal = best->vtotal;
        out->hsync_start = best->hsync_start;
        out->hsync_end = best->hsync_end;
        out->vsync_start = best->vsync_start;
        out->vsync_end = best->vsync_end;
    }
    mutex_unlock(&fb->client.modeset_mutex);
    if (best == NULL) {
        printf("NullMoth display: no output is connected\n");
        err = ENXIO;
        goto fail;
    }
    /* b180: every other output picks a mode that fits this buffer */
    err = -drm_client_modeset_probe(&fb->client, out->width, out->height);
    if (err) {
        printf("NullMoth display: probing the outputs for %ux%u failed (%d)\n", out->width, out->height, err);
        goto fail;
    }

    fb->buffer = drm_client_framebuffer_create(&fb->client, out->width, out->height, DRM_FORMAT_XRGB8888);
    if (IS_ERR(fb->buffer)) {
        err = -PTR_ERR(fb->buffer);
        fb->buffer = NULL;
        printf("NullMoth display: no %ux%u scan-out buffer (%d)\n", out->width, out->height, err);
        goto fail;
    }
    out->pitch = fb->buffer->fb->pitches[0];

    printf("NullMoth display: scan-out buffer %ux%u created, pitch %u; mapping it\n", out->width, out->height, out->pitch);
    err = -drm_client_buffer_vmap(fb->buffer, &fb->map);
    if (err) {
        printf("NullMoth display: the scan-out buffer would not map (%d)\n", err);
        goto fail;
    }
    fb->mapped = 1;

    /* the published range must be one physical run: check every page of the CPU view */
    const char *va = fb->map.is_iomem ? (const char *)fb->map.vaddr_iomem : (const char *)fb->map.vaddr;
    uint64_t size = (uint64_t)out->pitch * out->height;
    uint64_t base = nmx_xnu_kva_to_phys(va);
    for (uint64_t off = PAGE_SIZE; off < size; off += PAGE_SIZE)
        if (nmx_xnu_kva_to_phys(va + off) != base + off) {
            printf("NullMoth display: the scan-out buffer is not physically contiguous (page %llu)\n",
                   (unsigned long long)(off / PAGE_SIZE));
            err = EFAULT;
            goto fail;
        }
    if (base == 0) {
        err = EFAULT;
        goto fail;
    }
    out->phys = base;
    out->size = round_page(size);
    out->kva = (uint64_t)(uintptr_t)va;
    /* the firmware's last boot screen (snapshot taken before i915 took the GPU), else black, until WindowServer draws */
    if (!nmx_boot_snapshot_paint(__DECONST(char *, va), out->pitch, out->width, out->height))
        memset(__DECONST(char *, va), 0, size);

    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client)
        if (ms->mode != NULL) {
            if (!ms_internal(ms) && !nmx_fb_ext_wanted) { ms_drop(fb, ms); continue; }
            ms->fb = fb->buffer->fb;
        }
    mutex_unlock(&fb->client.modeset_mutex);
    /* b164: is the firmware screen still lit? Then keep it until WindowServer opens us (nmx_fb_go): the boot logo and
       progress bar stay, instead of a black buffer and later a second, smaller logo. Otherwise commit now, as before. */
    {
        int lit = 0;
        struct drm_crtc *crtc;
        drm_for_each_crtc(crtc, drm)
            if (crtc->state && crtc->state->active && crtc->primary && crtc->primary->state && crtc->primary->state->fb)
                lit++;
        fb->deferred = nmx_fb_defer_wanted && lit > 0;
        printf("NullMoth display: %d output(s) still show the firmware screen; first mode set %s\n", lit,
               fb->deferred ? "DEFERRED until WindowServer opens the framebuffer (-nmxnodefer: at once)" :
               (nmx_fb_defer_wanted ? "now (nothing lit to keep)" : "now (-nmxnodefer)"));
    }
    if (fb->deferred) {
        nmx_step("bridge: buffer painted, mode set deferred (firmware screen kept)");
    } else {
        nmx_step("bridge: buffer painted, committing the mode set");
        err = -bridge_commit(fb, "first mode set");
        nmx_step(err ? "bridge: mode set FAILED" : "bridge: mode set committed");
        if (err) {
            printf("NullMoth display: the mode set failed (%d)\n", err);
            goto fail;
        }
    }
    drm_client_register(&fb->client);       /* from here on hotplug reaches bridge_hotplug */

    fb->info = *out;
    fbs[slot].dev = dev;
    fbs[slot].fb = fb;
    out->ok = 1;
    printf("NullMoth display: %ux%u, pitch %u, phys 0x%llx, %u kHz pixel clock\n", out->width, out->height,
           out->pitch, (unsigned long long)out->phys, out->pclk_khz);
    return 0;

fail:
    bridge_free(fb);
    memset(out, 0, sizeof(*out));
    return err ? err : EIO;
}

/*
 * The framebuffer is gone from macOS: blank the outputs. The client itself is freed by the DRM core when the driver
 * detaches (bridge_unregister), which the kext does right after.
 */
void nmx_fb_close(void *dev)
{
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev && fbs[i].fb != NULL)
            drm_client_modeset_dpms(&fbs[i].fb->client, DRM_MODE_DPMS_OFF);
}

/* display sleep and wake from macOS (IOFramebuffer power) */
int nmx_fb_power(void *dev, int on)
{
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev) {
            int r;
            if (fbs[i].fb->deferred) {      /* b164: before the hand-over the firmware screen stays as it is */
                printf("NullMoth display: DPMS %s before the boot screen hand-over - left to the firmware screen\n", on ? "on" : "off");
                return 0;
            }
            nmx_step(on ? "bridge: DPMS on commit" : "bridge: DPMS off commit");
            r = -drm_client_modeset_dpms(&fbs[i].fb->client, on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
            nmx_step("bridge: DPMS commit returned");
            if (on) {
                cursor_reapply(fbs[i].fb);
                bridge_poll_outputs(fbs[i].fb, "display on");   /* a resume may have switched polling off */
            }
            else {
                fbs[i].fb->cur_shown = -1;
                fbs[i].fb->cur_hidden = 0;
            }
            return r;
        }
    return ENODEV;
}

/*
 * b89 (10-19): "video memory" for NMXAccel (WindowServer's compositing buffers, nvAllocVram). The Intel GPU has no VRAM
 * and 9-16 MB of physically contiguous system memory could not be found at run time ("vram: no 9240576 contiguous
 * bytes", b88). A DRM client buffer is mapped through the GPU aperture exactly like the scan-out buffer above: one
 * contiguous physical run in the BAR, CPU-visible, and real GPU memory as far as the display engine is concerned.
 */
struct nmx_vram {
    struct drm_client_buffer *buffer;
    struct iosys_map map;
};

int nmx_fb_vram_alloc(void *dev, uint64_t size, uint64_t *phys, void **kva, void **handle)
{
    struct nmx_fb *fb = NULL;
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev)
            fb = fbs[i].fb;
    *phys = 0; *kva = NULL; *handle = NULL;
    if (fb == NULL || size == 0)
        return ENODEV;
    /* XRGB8888, 1024 pixels (4096 bytes) a row: as many rows as the size needs */
    const uint32_t w = 1024;
    const uint32_t h = (uint32_t)((size + 4095) / 4096);
    if (h > 16384)
        return EINVAL;
    struct nmx_vram *v = kzalloc(sizeof(*v), GFP_KERNEL);
    if (v == NULL)
        return ENOMEM;
    v->buffer = drm_client_framebuffer_create(&fb->client, w, h, DRM_FORMAT_XRGB8888);
    if (IS_ERR(v->buffer)) {
        int err = -PTR_ERR(v->buffer);
        kfree(v);
        return err ? err : ENOMEM;
    }
    if (v->buffer->fb->pitches[0] != 4096 || drm_client_buffer_vmap(v->buffer, &v->map) != 0) {
        drm_client_framebuffer_delete(v->buffer);
        kfree(v);
        return EFAULT;
    }
    const char *va = v->map.is_iomem ? (const char *)v->map.vaddr_iomem : (const char *)v->map.vaddr;
    const uint64_t bytes = (uint64_t)4096 * h;
    const uint64_t base = nmx_xnu_kva_to_phys(va);
    int ok = base != 0;
    for (uint64_t off = PAGE_SIZE; ok && off < bytes; off += PAGE_SIZE)
        if (nmx_xnu_kva_to_phys(va + off) != base + off)
            ok = 0;
    if (!ok) {
        drm_client_buffer_vunmap(v->buffer);
        drm_client_framebuffer_delete(v->buffer);
        kfree(v);
        return EFAULT;
    }
    memset(__DECONST(char *, va), 0, bytes);
    *phys = base; *kva = __DECONST(char *, va); *handle = v;
    return 0;
}

void nmx_fb_vram_free(void *handle)
{
    struct nmx_vram *v = handle;
    if (v == NULL)
        return;
    drm_client_buffer_vunmap(v->buffer);
    drm_client_framebuffer_delete(v->buffer);
    kfree(v);
}

/*
 * b94: hardware cursor. macOS (IOFramebuffer) draws the pointer itself when the framebuffer has no cursor of its own:
 * it saves the pixels under it, draws it into the desktop and puts the pixels back when it moves. With WindowServer on
 * the GPU that drawing landed in a buffer that was not the one on the panel - the ghost pointers and flicker of 10-19.
 * The display engine's cursor plane shows the pointer on top of the desktop without touching it, as on a real Mac.
 */
static struct nmx_fb *fb_of(void *dev)
{
    for (int i = 0; i < MAX_FB; i++)
        if (fbs[i].dev == dev && fbs[i].fb != NULL)
            return fbs[i].fb;
    return NULL;
}

/* b164: the deferred first mode set: our scan-out buffer goes on the panel now */
static int bridge_go(struct nmx_fb *fb, const char *why)
{
    if (fb == NULL || !fb->deferred)
        return 0;
    mutex_lock(&fb->flip_lock);
    fb->deferred = 0;
    if (fb->pending_back && fb->buf2)       /* b167: the newest frame is in the second buffer */
        fb->front = 1;
    /* b198 (Tahoe: the Apple logo jumps to the lower right at the hand-over, 10-27): with no newer frame in the back
       buffer, buffer 0 still holds the FIRST WindowServer's picture - its boot logo, drawn for the native 2560x1600
       layout. When the restarted WindowServer runs a scaled desktop (cur_w x cur_h smaller than the panel, Tahoe:
       1600x1000), that picture goes on the panel as its top-left cur_w x cur_h part, enlarged by the panel fitter: the
       logo lands at ~(0.8, 0.8) of the screen. Black instead - the restarted WindowServer's frames follow. Only
       written, never read (the buffer is in the GPU aperture). */
    if (fb->front == 0 && fb->mapped && fb->cur_w && fb->cur_h &&
        (fb->cur_w != fb->info.width || fb->cur_h != fb->info.height)) {
        char *va = fb->map.is_iomem ? (char *)fb->map.vaddr_iomem : (char *)fb->map.vaddr;
        if (va) {
            memset(va, 0, (size_t)fb->info.pitch * fb->info.height);
            printf("NullMoth display: hand-over: scaled desktop %ux%u on a %ux%u panel - buffer 0 cleared (no stale boot logo)\n",
                   fb->cur_w, fb->cur_h, fb->info.width, fb->info.height);
        }
    }
    bridge_point_outputs(fb);               /* b180: always - each output's mode and picture (panel / external) */
    printf("NullMoth display: boot screen hand-over: %s - committing our scan-out buffer %d\n", why, fb->front);
    nmx_step("bridge: deferred mode set committing");
    int err = -bridge_commit(fb, "hand-over");
    mutex_unlock(&fb->flip_lock);
    bridge_log_outputs(fb, "hand-over");
    bridge_poll_outputs(fb, "hand-over");
    nmx_step(err ? "bridge: deferred mode set FAILED" : "bridge: deferred mode set committed");
    if (!err)
        nmx_steps_go_quiet("boot screen handed over");
    if (err)
        printf("NullMoth display: the deferred mode set failed (%d)\n", err);
    cursor_reapply(fb);
    return err;
}

/* b169: does the newest held frame (the second buffer: while the logo is held no flip lands, so NMXAccel keeps drawing
   there) show a picture yet? 64 pixels on an 8 x 8 grid of the desktop, read through the aperture; the number that are
   not (nearly) black, -1 = no frame yet. The restarted WindowServer's first frames are black with only the pointer
   (10-24 b168: ~6 s of black after the logo) - the hand-over waits for the lock screen picture. */
int nmx_fb_back_content(void *dev)
{
    struct nmx_fb *fb = fb_of(dev);
    if (fb == NULL || !fb->pending_back || fb->buf2 == NULL || !fb->mapped2)
        return -1;
    const char *va = fb->map2.is_iomem ? (const char *)fb->map2.vaddr_iomem : (const char *)fb->map2.vaddr;
    uint32_t w = fb->cur_w ? fb->cur_w : fb->info.width, h = fb->cur_h ? fb->cur_h : fb->info.height;
    int lit = 0;
    for (uint32_t gy = 0; gy < 8; gy++)
        for (uint32_t gx = 0; gx < 8; gx++) {
            uint32_t x = (w * (2 * gx + 1)) / 16, y = (h * (2 * gy + 1)) / 16;
            uint32_t px = *(volatile const uint32_t *)(va + (uint64_t)y * fb->info.pitch + (uint64_t)x * 4);
            if (((px >> 16) & 0xff) > 0x18 || ((px >> 8) & 0xff) > 0x18 || (px & 0xff) > 0x18)
                lit++;
        }
    return lit;
}

/* b174: the auto-arm helper's signal. Buffer 0 still holds the FIRST (unaccelerated) WindowServer's lock screen, drawn
   by the CPU before the helper restarted it - b173 showed that picture for ~2 s, then the new WindowServer's black
   frames, then the lock screen again. Now the hand-over waits for the next flip, i.e. the restarted WindowServer's
   first frame (the caller falls back to nmx_fb_go after a few seconds). */
int nmx_fb_go_on_flip(void *dev, const char *why)
{
    struct nmx_fb *fb = fb_of(dev);
    if (fb == NULL || !fb->deferred)
        return ENODEV;
    fb->go_why = why;
    fb->pending_back = 0;
    fb->go_on_flip = 1;
    return 0;
}

int nmx_fb_deferred(void *dev)
{
    struct nmx_fb *fb = fb_of(dev);
    return fb != NULL && fb->deferred;
}

int nmx_fb_go(void *dev, const char *why)
{
    struct nmx_fb *fb = fb_of(dev);
    if (fb == NULL)
        return ENODEV;
    return bridge_go(fb, why);
}

/* one cursor call on every lit output; img -1 = off. Caller holds cur_lock. */
static int cursor_set_locked(struct nmx_fb *fb, int img, int32_t x, int32_t y)
{
    struct drm_device *drm = fb->client.dev;
    struct drm_mode_set *ms;
    int err = 0, outputs = 0;
    struct drm_mode_cursor2 req;

    memset(&req, 0, sizeof(req));
    if (img < 0) {
        if (fb->cur_shown < 0 || fb->cur_hidden)
            return 0;
        /* b95: hide = move the image off the screen. Turning the plane off (handle 0) and on again were full atomic
           commits (~15 ms each); a move takes i915's cursor fast path (intel_legacy_cursor_update), which also handles
           a position where no pixel is visible. The plane keeps its image, so showing it again is one more move. */
        req.flags = DRM_MODE_CURSOR_MOVE;
        req.x = -(NMX_CUR_DIM * 4);
        req.y = -(NMX_CUR_DIM * 4);
        goto call;
    } else if (img != fb->cur_shown) {
        req.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE;
        req.handle = fb->cur_handle[img];
        req.width = NMX_CUR_DIM;
        req.height = NMX_CUR_DIM;
    } else {
        req.flags = DRM_MODE_CURSOR_MOVE;
    }
    req.x = x;
    req.y = y;
call:
    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client) {
        if (ms->mode == NULL || ms->crtc == NULL)
            continue;
        req.crtc_id = ms->crtc->base.id;
        int r = -drm_mode_cursor2_ioctl(drm, &req, fb->client.file);
        outputs++;
        if (r && !err)
            err = r;
    }
    mutex_unlock(&fb->client.modeset_mutex);
    if (!outputs)
        return ENXIO;
    if (!err && img >= 0) {
        fb->cur_shown = img;
        fb->cur_hidden = 0;
    } else if (!err)
        fb->cur_hidden = 1;             /* a hide keeps the image on the plane (off screen) */
    else if (fb->cur_errs++ < 8)
        printf("NullMoth display: cursor %s refused (%d) at %d,%d\n", img < 0 ? "off" : (img != fb->cur_shown ? "image" : "move"),
               err, x, y);
    return err;
}

/* after a mode set: the plane was turned off by the commit, show what macOS asked for again */
static void cursor_reapply(struct nmx_fb *fb)
{
    if (fb == NULL || !fb->cur_ready || fb->deferred)   /* b166: no pointer over the held boot logo */
        return;
    mutex_lock(&fb->cur_lock);
    fb->cur_shown = -1;
    fb->cur_hidden = 0;
    if (fb->cur_vis && fb->cur_img >= 0)
        (void)cursor_set_locked(fb, fb->cur_img, fb->cur_x, fb->cur_y);
    mutex_unlock(&fb->cur_lock);
}

int nmx_fb_cursor_init(void *dev, uint32_t *dim, void **kva0, void **kva1)
{
    struct nmx_fb *fb = fb_of(dev);
    int err = 0;
    *dim = 0; *kva0 = NULL; *kva1 = NULL;
    if (fb == NULL)
        return ENODEV;
    if (fb->cur_ready)
        goto done;
    for (int i = 0; i < 2; i++) {
        fb->cur_buf[i] = drm_client_framebuffer_create(&fb->client, NMX_CUR_DIM, NMX_CUR_DIM, DRM_FORMAT_ARGB8888);
        if (IS_ERR(fb->cur_buf[i])) {
            err = -PTR_ERR(fb->cur_buf[i]);
            fb->cur_buf[i] = NULL;
            printf("NullMoth display: cursor: no %dx%d image buffer (%d)\n", NMX_CUR_DIM, NMX_CUR_DIM, err);
            return err ? err : ENOMEM;
        }
        if (fb->cur_buf[i]->fb->pitches[0] != NMX_CUR_DIM * 4) {
            printf("NullMoth display: cursor: image pitch %u, plane needs %d\n", fb->cur_buf[i]->fb->pitches[0], NMX_CUR_DIM * 4);
            return EINVAL;
        }
        err = -drm_client_buffer_vmap(fb->cur_buf[i], &fb->cur_map[i]);
        if (err) {
            printf("NullMoth display: cursor: image buffer would not map (%d)\n", err);
            return err;
        }
        fb->cur_mapped[i] = 1;
        err = -drm_gem_handle_create(fb->client.file, fb->cur_buf[i]->gem, &fb->cur_handle[i]);
        if (err) {
            fb->cur_handle[i] = 0;
            printf("NullMoth display: cursor: no handle (%d)\n", err);
            return err;
        }
        char *va = fb->cur_map[i].is_iomem ? (char *)fb->cur_map[i].vaddr_iomem : (char *)fb->cur_map[i].vaddr;
        memset(va, 0, (size_t)NMX_CUR_DIM * NMX_CUR_DIM * 4);   /* fully transparent */
    }
    /* try it once on the outputs (transparent image, then off): a plane that refuses leaves macOS its own cursor */
    mutex_lock(&fb->cur_lock);
    err = cursor_set_locked(fb, 0, 0, 0);
    if (!err)
        err = cursor_set_locked(fb, -1, 0, 0);
    mutex_unlock(&fb->cur_lock);
    nmx_step(err ? "bridge: hardware cursor test FAILED" : "bridge: hardware cursor ready");
    if (err) {
        printf("NullMoth display: cursor plane refused the test (%d) - software cursor stays\n", err);
        return err;
    }
    fb->cur_ready = 1;
    printf("NullMoth display: hardware cursor ready (%dx%d ARGB, 2 images)\n", NMX_CUR_DIM, NMX_CUR_DIM);
done:
    *dim = NMX_CUR_DIM;
    *kva0 = fb->cur_map[0].is_iomem ? (void *)fb->cur_map[0].vaddr_iomem : fb->cur_map[0].vaddr;
    *kva1 = fb->cur_map[1].is_iomem ? (void *)fb->cur_map[1].vaddr_iomem : fb->cur_map[1].vaddr;
    return 0;
}

int nmx_fb_cursor_apply(void *dev, int img, int32_t x, int32_t y, int visible)
{
    struct nmx_fb *fb = fb_of(dev);
    int err;
    if (fb == NULL || !fb->cur_ready)
        return ENODEV;
    if (img > 1)
        return EINVAL;
    mutex_lock(&fb->cur_lock);
    fb->cur_img = img;
    fb->cur_vis = visible;
    fb->cur_x = x;
    fb->cur_y = y;
    /* b165: no pointer over the boot logo - remembered, shown by the hand-over (bridge_go -> cursor_reapply) */
    err = fb->deferred ? 0 : cursor_set_locked(fb, (visible && img >= 0) ? img : -1, x, y);
    mutex_unlock(&fb->cur_lock);
    return err;
}


/* ---- b125 (R2): double-buffered scan-out with a page flip at vblank ------------------------------------------------ */

/* a view of 'gem' at w x h with the buffer's pitch, owned by the client file (removed with drm_mode_rmfb) */
static struct drm_framebuffer *flip_view(struct nmx_fb *fb, struct drm_gem_object *gem, uint32_t w, uint32_t h)
{
    struct drm_device *drm = fb->client.dev;
    struct drm_mode_fb_cmd2 req;
    u32 handle = 0;
    if (drm_gem_handle_create(fb->client.file, gem, &handle))
        return NULL;
    memset(&req, 0, sizeof(req));
    req.width = w; req.height = h; req.pixel_format = DRM_FORMAT_XRGB8888;
    req.handles[0] = handle; req.pitches[0] = fb->info.pitch;
    int err = drm_mode_addfb2(drm, &req, fb->client.file);
    drm_gem_handle_delete(fb->client.file, handle);
    if (err)
        return NULL;
    struct drm_framebuffer *v = drm_framebuffer_lookup(drm, fb->client.file, req.fb_id);
    if (v)
        drm_framebuffer_put(v);         /* the file keeps it until rmfb */
    return v;
}

/* caller holds flip_lock: every flip still in flight has reached the panel (at most ~2 vblanks) */
static void flip_wait_locked(struct nmx_fb *fb)
{
    for (int k = 0; k < fb->npend; k++) {
        struct drm_crtc_commit *c = fb->pend[k];
        if (!try_wait_for_completion(&c->flip_done)) {
            fb->flip_waits++;
            /* b127: at most ~2 refreshes; a flip that has not landed by then is given up on (logged), never waited for
               longer - the caller holds the accelerator's copy lock */
            if (!wait_for_completion_timeout(&c->flip_done, msecs_to_jiffies(40)) && fb->flip_errs++ < 8)
                printf("NullMoth display: flip: a flip did not complete in 40 ms\n");
        }
        drm_crtc_commit_put(c);
        fb->pend[k] = NULL;
    }
    fb->npend = 0;
}

/* caller holds flip_lock: the second buffer (and its scaled view) exist */
static int flip_ready_locked(struct nmx_fb *fb)
{
    if (fb->buf2 == NULL) {
        struct drm_client_buffer *b = drm_client_framebuffer_create(&fb->client, fb->info.width, fb->info.height, DRM_FORMAT_XRGB8888);
        if (IS_ERR(b)) { printf("NullMoth display: flip: no second %ux%u buffer (%ld)\n", fb->info.width, fb->info.height, PTR_ERR(b)); return ENOMEM; }
        if (b->fb->pitches[0] != fb->info.pitch || drm_client_buffer_vmap(b, &fb->map2) != 0) {
            printf("NullMoth display: flip: second buffer pitch %u (need %u) or no mapping\n", b->fb->pitches[0], fb->info.pitch);
            drm_client_framebuffer_delete(b); return EINVAL; }
        fb->buf2 = b; fb->mapped2 = 1;
        char *va = fb->map2.is_iomem ? (char *)fb->map2.vaddr_iomem : (char *)fb->map2.vaddr;
        memset(va, 0, (size_t)fb->info.pitch * fb->info.height);
        printf("NullMoth display: flip: second scan-out buffer %ux%u ready (double buffering)\n", fb->info.width, fb->info.height);
    }
    if (fb->cur_w && fb->cur_h && fb->scaled2 == NULL) {
        fb->scaled2 = flip_view(fb, fb->buf2->gem, fb->cur_w, fb->cur_h);
        if (fb->scaled2 == NULL) { printf("NullMoth display: flip: no %ux%u view of the second buffer\n", fb->cur_w, fb->cur_h); return EINVAL; }
    }
    return 0;
}

/* the CPU view of the buffer NOT on the panel; 0 = ok */
int nmx_fb_flip_back(void *dev, void **kva, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    struct nmx_fb *fb = fb_of(dev);
    *kva = NULL;
    if (fb == NULL || fb->buffer == NULL)
        return ENODEV;
    mutex_lock(&fb->flip_lock);
    int err = flip_ready_locked(fb);
    if (!err) {
        flip_wait_locked(fb);           /* the back buffer may still be on the panel until the last flip lands */
        struct iosys_map *m = fb->front ? &fb->map : &fb->map2;
        *kva = m->is_iomem ? (void *)m->vaddr_iomem : m->vaddr;
        *pitch = fb->info.pitch;
        *w = fb->cur_w ? fb->cur_w : fb->info.width;
        *h = fb->cur_h ? fb->cur_h : fb->info.height;
    }
    mutex_unlock(&fb->flip_lock);
    return err;
}

/* primary plane of every lit output -> 'target' buffer (0/1); blocking: returns once the panel scans it (next vblank) */
static int flip_commit_to(struct nmx_fb *fb, int target, int nonblock)
{
    struct drm_device *drm = fb->client.dev;
    struct drm_modeset_acquire_ctx ctx;
    struct drm_atomic_state *state;
    struct drm_mode_set *ms;
    int err = 0, planes = 0;
    if (target && fb->buf2 == NULL)
        return EINVAL;
    drm_modeset_acquire_init(&ctx, 0);
    state = drm_atomic_state_alloc(drm);
    if (state == NULL) { drm_modeset_acquire_fini(&ctx); return ENOMEM; }
    state->acquire_ctx = &ctx;
retry:
    planes = 0;
    mutex_lock(&fb->client.modeset_mutex);
    drm_client_for_each_modeset(ms, &fb->client) {
        if (ms->mode == NULL || ms->crtc == NULL || ms->crtc->primary == NULL)
            continue;
        struct drm_plane_state *ps = drm_atomic_get_plane_state(state, ms->crtc->primary);
        if (IS_ERR(ps)) { err = PTR_ERR(ps); break; }
        if (ps->crtc == NULL) continue;             /* plane not lit on this output */
        struct drm_framebuffer *nfb = ms_view(fb, ms, target);   /* b180: per output (panel scaled, external 1:1) */
        drm_atomic_set_fb_for_plane(ps, nfb);
        ms->fb = nfb;                               /* later mode sets (hotplug, DPMS) keep this buffer */
        planes++;
    }
    mutex_unlock(&fb->client.modeset_mutex);
    if (!err && planes) {
        err = nonblock ? drm_atomic_nonblocking_commit(state) : drm_atomic_commit(state);
        if (!err && nonblock) {             /* keep each CRTC's commit to wait for its flip later */
            mutex_lock(&fb->client.modeset_mutex);
            drm_client_for_each_modeset(ms, &fb->client) {
                if (ms->mode == NULL || ms->crtc == NULL || ms->crtc->state == NULL || ms->crtc->state->commit == NULL) continue;
                if (fb->npend < 4) fb->pend[fb->npend++] = drm_crtc_commit_get(ms->crtc->state->commit);
            }
            mutex_unlock(&fb->client.modeset_mutex);
        }
    }
    if (err == -EDEADLK) {
        drm_atomic_state_clear(state);
        drm_modeset_backoff(&ctx);
        err = 0;
        goto retry;
    }
    drm_atomic_state_put(state);
    drm_modeset_drop_locks(&ctx);
    drm_modeset_acquire_fini(&ctx);
    if (!err && !planes)
        err = -ENXIO;
    return -err;
}

/* the frame written into the back buffer goes on the panel at the next vblank; 0 = ok */
int nmx_fb_flip_commit(void *dev)
{
    struct nmx_fb *fb = fb_of(dev);
    if (fb == NULL || fb->buffer == NULL)
        return ENODEV;
    /* b167: no flip while the boot logo is held. b166 handed over at the first flip at 12 s - NMXAccel already flips
       for the FIRST (unaccelerated) WindowServer, so a flip says nothing about acceleration. The hand-over comes from
       NMXGPUFB (WindowServer's open without the auto-arm; the helper's signal or the timeout with it). */
    if (fb->deferred) {
        fb->pending_back = 1;           /* the hand-over shows this frame instead of buffer 0's older picture */
        if (fb->go_on_flip) {           /* b174: this is the restarted WindowServer's frame - show it now */
            fb->go_on_flip = 0;
            return bridge_go(fb, fb->go_why) ? EAGAIN : 0;
        }
        return EAGAIN;
    }
    mutex_lock(&fb->flip_lock);
    /* b128: BLOCKING again (as b125, which never hung) - NMXGPUFB runs it on its own thread call, so the accelerator
       does not wait. b126/b127's nonblocking commit froze the Mac within seconds (10-21/22). */
    int err = fb->buf2 ? flip_commit_to(fb, !fb->front, 0) : ENXIO;
    int psr_now = 0;
    if (!err) {
        fb->front = !fb->front;
        if (nmx_fb_psr_wanted && !fb->psr_on && ++fb->psr_flips == NMX_PSR_AFTER_FLIPS)
            psr_now = 1;
        if ((++fb->flips & 0xfff) == 1)
            printf("NullMoth display: flip: %lu frames flipped at vblank, %lu refused, %lu waited for the previous flip\n",
                   fb->flips, fb->flip_errs, fb->flip_waits);
    } else if (fb->flip_errs++ < 8)
        printf("NullMoth display: flip: commit refused (%d) - this frame stays off the panel\n", err);
    mutex_unlock(&fb->flip_lock);
    if (psr_now) {
        int r = nmx_i915_psr(fb->client.dev, 1);
        fb->psr_on = r > 0;
        printf("NullMoth display: panel self refresh (PSR1) %s after %d accelerated frames (%d; boot-arg -nmxnopsr: off)\n",
               r > 0 ? "ON" : "not available", NMX_PSR_AFTER_FLIPS, r);
    }
    return err;
}

/* back to buffer 0 (the IOFramebuffer's memory, which macOS draws into when not accelerated) */
int nmx_fb_flip_home(void *dev)
{
    struct nmx_fb *fb = fb_of(dev);
    if (fb == NULL || fb->buffer == NULL)
        return ENODEV;
    if (fb->psr_on) {   /* b170: CPU-drawn frames next - i915 cannot follow those with PSR */
        int r = nmx_i915_psr(fb->client.dev, 0);
        printf("NullMoth display: panel self refresh OFF (presentation back to buffer 0) (%d)\n", r);
        fb->psr_on = 0;
    }
    fb->psr_flips = 0;
    mutex_lock(&fb->flip_lock);
    int err = 0;
    flip_wait_locked(fb);
    if (fb->front && fb->buf2) {
        /* no copy of the picture: both buffers are read through the uncached GPU aperture (b89: ~1 s per frame) */
        err = flip_commit_to(fb, 0, 0);
        if (!err) { fb->front = 0; printf("NullMoth display: flip: back home on buffer 0\n"); }
    }
    mutex_unlock(&fb->flip_lock);
    return err;
}
