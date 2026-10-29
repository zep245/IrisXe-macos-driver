/*
 * The kext: one IOService per AMD or Intel GPU it is allowed to drive (amdgpu and i915 share one DRM core and one
 * port layer here). Compiled with Apple's headers only; everything else is reached through nmx_port.h.
 *
 * Opt-in only, per vendor. The service matches every AMD and Intel display-class device but declines in probe()
 * unless the user asked for that vendor: boot argument nmxamd=1 / nmxintel=1, or the device property
 * "nullmoth-gpu" (set through the bootloader's DeviceProperties). Without that, the device keeps whatever driver macOS
 * gives it. Taking a GPU that is lighting the boot display resets its display engine, so the opt-in is per user,
 * never a default.
 *
 * Intel only: nmxnofb=1 attaches headless (render path without the display bridge); see headless() below.
 *
 * The port (the FreeBSD/Linux layer and the AMD driver) starts with the first device and stops when the kext
 * unloads; unloading is refused while any process still has a GPU device node open.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include <mach/kmod.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include "nmx_port.h"
#include "nmx_xnu.h"

#define AMD_VENDOR 0x1002
#define INTEL_VENDOR 0x8086

/* the display bridge (nmx_fb.cpp) */
IOService *nmx_fb_publish(IOService *gpu, void *dev);
void nmx_fb_unpublish(IOService *fb, IOService *gpu, void *dev);
extern "C" void nmx_boot_snapshot(void);
extern "C" int nmx_tcache_disable;   /* b120: bsdcompat/src/nmx_kernel.c */

class NMXGPU : public IOService {
    OSDeclareDefaultStructors(NMXGPU)
    void *dev_;
    IOService *fb_;
    IOPCIDevice *pci_;
    volatile int attach_;   /* 0 = attach pending on its thread, 1 = attached, -1 = not attached */
    bool pm_;
    int awake_ = 0;       /* b79: the keep-awake level applied at attach, released before shutdown/sleep */
    void releaseAwake(const char *why);
    /* b138: GPU clock governor (nmx_i915_gpu_gov_tick) - every 200 ms; paused across sleep */
    thread_call_t gov_ = nullptr;
    volatile bool govOn_ = false;
    int govBusy_ = 800, govIdle_ = 350;
    /* b152: stepped governor - steps[0] idle .. steps[3] top; nmxgpumax caps the top (b153: default 1300) */
    int govStep_[4] = { 300, 500, 850, 1300 };
    int govLevel_ = -1, govWantLower_ = 0;
    unsigned govTicks_ = 0, govSwitches_ = 0, govBusyTicks_ = 0;
    int govLast_ = -1;
    unsigned govRc6At_ = 0;   /* b148: tick at which the boot-time keep-awake hold is released (0 = never) */
    static void govFire(thread_call_param_t p, thread_call_param_t);
    void govArm(uint32_t ms);
    void govStop();
    bool powered_ = true; /* the device is on from the moment it attaches; IOKit's first state-0 call must not suspend it */
    static void attachThread(void *arg, wait_result_t);
    void attachNow();
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override { return 1; }
    void systemWillShutdown(IOOptionBits spec) override;
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};
OSDefineMetaClassAndStructors(NMXGPU, IOService)

static IOLock *g_lock;
static bool g_started;
static int g_devices;

static bool opted_in(IOPCIDevice *pci, uint16_t vendor)
{
    int v = 0;
    /* the boot picker's "Start once without NVIDIA driver" entry (-nvoff) parks every NullMoth GPU driver for that start */
    if (PE_parse_boot_argn("-nvoff", &v, sizeof(v)))
        return false;
    if (PE_parse_boot_argn(vendor == AMD_VENDOR ? "nmxamd" : "nmxintel", &v, sizeof(v)) && v != 0)
        return true;
    OSObject *p = pci->getProperty("nullmoth-gpu");
    if (OSBoolean *b = OSDynamicCast(OSBoolean, p))
        return b->isTrue();
    if (OSData *d = OSDynamicCast(OSData, p))
        return d->getLength() > 0 && ((const uint8_t *)d->getBytesNoCopy())[0] != 0;
    return false;
}

/*
 * NMXFBHold: takes the IOFramebuffer match category on the Intel GPU when NMXGPU will drive it (nmxintel=1), so neither
 * IONDRVFramebuffer (the firmware screen) nor NMIntelFB starts beside NMXGPUFB. With two framebuffers WindowServer put the
 * login window on the firmware one, which the panel no longer shows (10-16). It does nothing else. Boot-arg nmxkeepndrv=1,
 * nmxnofb=1 or -nvoff leaves the category to the others.
 */
struct drm_device;
extern "C" void *nmx_drm_of(void *dev);
extern "C" int nmx_i915_keep_awake(struct drm_device *drm, int level);
extern "C" int nmx_i915_gpu_floor(struct drm_device *drm, int mhz, int *lo, int *hi);
extern "C" int nmx_i915_gpu_gov_tick(struct drm_device *drm, int busy_mhz, int idle_mhz, int *util, int *actual_mhz);
extern "C" int nmx_i915_gpu_util(struct drm_device *drm);
extern "C" int nmx_i915_gpu_pin(struct drm_device *drm, int mhz);
extern "C" int nmx_i915_keep_awake_release(struct drm_device *drm, int level);
extern "C" IOReturn nmx_drm_bridge_new(void *task, uint32_t type, IOUserClient **out);
static bool nmx_marker_set();
class NMXFBHold : public IOService {
    OSDeclareDefaultStructors(NMXFBHold)
public:
    IOService *probe(IOService *provider, SInt32 *score) override {
        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
        if (pci == nullptr || !opted_in(pci, 0x8086)) return nullptr;
        uint32_t v = 0;
        if (PE_parse_boot_argn("nmxkeepndrv", &v, sizeof(v)) && v != 0) return nullptr;
        v = 0;
        if (PE_parse_boot_argn("nmxnofb", &v, sizeof(v)) && v != 0) return nullptr;
        /* the boot guard is set (the last attach never returned), so NMXGPU parks and publishes no screen: leave the
           slot to IONDRV / NMIntelFB, or the Mac boots with no display at all (10-17: text console on every config) */
        if (nmx_marker_set()) {
            IOLog("NullMoth GPU: boot guard set - not holding the framebuffer slot; the firmware screen stays\n");
            return nullptr;
        }
        return IOService::probe(provider, score);
    }
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        IOLog("NullMoth GPU: holding the framebuffer slot of %s for NMXGPUFB (no IONDRV / NMIntelFB)\n", provider->getName());
        return true;
    }
};
OSDefineMetaClassAndStructors(NMXFBHold, IOService)

/*
 * Headless bring-up (boot argument nmxnofb=1): the driver attaches and the accelerator starts, but the display bridge is
 * not published, so no DRM client lights the outputs and no IOFramebuffer is added. On a laptop whose internal panel is
 * wired to this same Intel GPU, that leaves the screen exactly as the firmware (or NMIntelFB) set it up: the first test
 * of the render path cannot take the display away. Applies to Intel only; AMD behaves as before.
 */
static bool headless(uint16_t vendor)
{
    int v = 0;
    return vendor == INTEL_VENDOR && PE_parse_boot_argn("nmxnofb", &v, sizeof(v)) && v != 0;
}

IOService *NMXGPU::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    uint16_t vendor = pci ? pci->configRead16(kIOPCIConfigVendorID) : 0;
    if (vendor != AMD_VENDOR && vendor != INTEL_VENDOR)
        return nullptr;
    if (!opted_in(pci, vendor))
        return nullptr;     /* not asked for: leave the device to macOS */
    return IOService::probe(provider, score);
}

bool NMXGPU::start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr || g_lock == nullptr || !IOService::start(provider))
        return false;
    IOLockLock(g_lock);
    if (!g_started) {
        /* b151: OFF by default. 10-23 13:01 boot panic "thread_invoke: preemption_level -1" (attach -> cursor vmap ->
           shmem cond_resched); the same panic first appeared on 10-21, the day b120 added this cache, which never gave a
           measurable gain. -nmxtcache turns it back on. */
        nmx_tcache_disable = PE_parse_boot_argn("-nmxtcache", nullptr, 0) ? 0 : 1;
        IOLog("NullMoth GPU: per-CPU thread cache %s\n", nmx_tcache_disable ? "off (default since b151; -nmxtcache = on)" : "ON (-nmxtcache)");
        int err = nmx_port_start();
        if (err) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: the driver layer did not start (%d)\n", err);
            IOService::stop(provider);
            return false;
        }
        g_started = true;
    }
    g_devices++;
    IOLockUnlock(g_lock);

    /*
     * amdgpu, i915 and xe load firmware while they attach. During boot matching that read would come before the root and
     * Data volumes are mounted, and mounting waits for matching to settle: the Mac would stop at the logo (seen with the
     * NPU kext on the same layer, 10-11). The attach runs on its own thread once the firmware folder exists; the firmware
     * framebuffer keeps the screen until then.
     */
    pci_ = pci;
    dev_ = nullptr;
    fb_ = nullptr;
    pm_ = false;
    attach_ = 0;
    retain();
    thread_t t = THREAD_NULL;
    if (kernel_thread_start(&NMXGPU::attachThread, this, &t) != KERN_SUCCESS) {
        IOLog("NullMoth GPU: could not start the attach thread\n");
        attach_ = -1;
        release();
        return true;
    }
    thread_deallocate(t);
    return true;
}

void NMXGPU::attachThread(void *arg, wait_result_t)
{
    NMXGPU *self = (NMXGPU *)arg;
    self->attachNow();
    self->release();
}

/*
 * Boot guard, as in NMXNPU: the NPU kext on this layer panicked 7 s into every start until its boot argument was
 * removed from Linux (10-11). The attach, the modeset and the hand-over to macOS run between setting an NVRAM marker
 * and clearing it; a start that finds the marker still set follows an attach that never returned (a panic or a hang),
 * so this GPU stays with macOS and the driver says so. Turning the test driver off and on in the app clears it.
 */
static const char *const kAttachMarker = "nullmoth-gpu-attaching";

static IODTNVRAM *nmx_nvram()
{
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
    if (n == nullptr && e != nullptr)
        e->release();
    return n;
}

static bool nmx_marker_set()
{
    IODTNVRAM *n = nmx_nvram();
    if (n == nullptr)
        return false;
    OSObject *v = n->copyProperty(kAttachMarker);
    bool set = v != nullptr;
    if (v)
        v->release();
    n->release();
    return set;
}

static void nmx_marker_write(bool on)
{
    IODTNVRAM *n = nmx_nvram();
    if (n == nullptr) {
        IOLog("NullMoth GPU: no NVRAM; the boot guard is off\n");
        return;
    }
    const OSSymbol *k = OSSymbol::withCString(kAttachMarker);
    if (k != nullptr) {
        if (on) {
            OSData *d = OSData::withBytes("1", 1);
            if (d == nullptr || !n->setProperty(k, d))
                IOLog("NullMoth GPU: could not set the boot guard\n");
            if (d)
                d->release();
        } else {
            n->removeProperty(k);
        }
        k->release();
    }
    n->sync();
    n->release();
}

/*
 * nmx_step(): breadcrumb for hangs that lose the log. Each call logs the step and stores it in NVRAM
 * (nullmoth-last-step = "<seq> <step>"), which survives a forced power-off; read it on the next boot with
 *   nvram nullmoth-last-step
 * Called from sleepable contexts only (attach thread, the first mode set).
 */
extern "C" void nmx_step(const char *what);
/* nmx_stepn(): a breadcrumb with a number appended, e.g. the commit sequence */
extern "C" void nmx_stepn(const char *what, long n)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%s%ld", what ? what : "?", n);
    nmx_step(buf);
}

/*
 * b126: once WindowServer has opened the framebuffer (nmx_steps_quiet set by NMXGPUFB::enableController), the per-commit
 * breadcrumbs of the display engine ("commit ...", ~37 per atomic commit) are no longer logged or written to NVRAM.
 * Each NVRAM sync is a slow firmware-variable write: with b125's page flip that was ~700 ms per frame (nmx-flipcheck,
 * 10-21) - and every DPMS / wake / size-change commit paid it too. Boot-arg -nmxsteps keeps them.
 */
extern "C" volatile int nmx_steps_quiet;
volatile int nmx_steps_quiet;
/* b213: also quiet after the boot screen hand-over. Sequoia (10-29) still logged both "NullMoth commit:" lines on every
   commit (~7 a second, ret=0): there WindowServer never calls NMXGPUFB::enableController in the accelerated setup, so
   b126's switch never flipped. The hand-over is reached on every boot that shows the desktop. */
extern "C" void nmx_steps_go_quiet(const char *why)
{
    if (nmx_steps_quiet || PE_parse_boot_argn("-nmxsteps", nullptr, 0)) return;
    nmx_steps_quiet = 1;
    IOLog("NullMoth GPU: per-commit log lines off from here (%s; -nmxsteps keeps them)\n", why ? why : "?");
}
extern "C" void nmx_step(const char *what)
{
    static int seq;
    if (nmx_steps_quiet && what && !strncmp(what, "commit", 6)) { ++seq; return; }
    char buf[96];
    snprintf(buf, sizeof(buf), "%02d %s", ++seq, what ? what : "?");
    IOLog("NullMoth step: %s\n", buf);
    IODTNVRAM *n = nmx_nvram();
    if (n == nullptr)
        return;
    const OSSymbol *k = OSSymbol::withCString("nullmoth-last-step");
    OSData *d = OSData::withBytes(buf, (unsigned)strlen(buf));
    if (k && d)
        n->setProperty(k, d);
    if (d) d->release();
    if (k) k->release();
    n->sync();
    n->release();
}


/* read-only snapshot of the display engine (eDP panel power, backlight, pipe A, plane 1, DDI A, power wells) for black-screen
   diagnosis; reads only, safe at any time the device is powered */
extern "C" unsigned long __drm_debug;
/* b192: i915 controls SAGV / QGV points (PCODE requests at every bandwidth change) only with boot-arg nmxsagv=1.
   Off by default since Tahoe hung inside "commit tail: SAGV post-plane" on every boot (10-27); see patch_i915_nosagv.py */
extern "C" int nmx_sagv_on(void)
{
    static int v = -1;
    if (v < 0) {
        int a = 0;
        v = (PE_parse_boot_argn("nmxsagv", &a, sizeof(a)) && a != 0) ? 1 : 0;
        IOLog("NullMoth GPU: SAGV / QGV control by i915 %s (boot-arg nmxsagv=1 turns it on)\n", v ? "ON" : "OFF");
    }
    return v;
}
/* boot-arg nmxnovga=1: i915 leaves the legacy VGA plane alone (see patch_i915_novga.py) */
extern "C" int nmx_novga(void)
{
    int v = 0;
    return PE_parse_boot_argn("nmxnovga", &v, sizeof(v)) && v != 0;
}


static void nmx_dump_display(IOPCIDevice *pci, const char *tag)
{
    IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (m == nullptr) {
        IOLog("NullMoth DISP[%s] cannot map BAR0\n", tag);
        return;
    }
    volatile uint32_t *r = (volatile uint32_t *)m->getVirtualAddress();
    static const struct { const char *n; uint32_t o; } regs[] = {
        { "PP_STATUS", 0xc7200 }, { "PP_CONTROL", 0xc7204 },
        { "BLC_PWM_CTL1", 0xc8250 }, { "BLC_PWM_FREQ1", 0xc8254 }, { "BLC_PWM_DUTY1", 0xc8258 },
        { "PIPECONF_A", 0x70008 }, { "PIPE_SRC_A", 0x6001c }, { "TRANS_DDI_FUNC_CTL_A", 0x60400 },
        { "TRANS_CLK_SEL_A", 0x46140 }, { "DDI_BUF_CTL_A", 0x64000 }, { "DP_TP_CTL_A", 0x64040 },
        { "PLANE_CTL_1_A", 0x70180 }, { "PLANE_SURF_1_A", 0x7019c }, { "CDCLK_CTL", 0x46000 },
        { "PWR_WELL_CTL1", 0x45400 }, { "PWR_WELL_CTL2", 0x45404 }, { "DC_STATE_EN", 0x45504 },
        { "PLANE_STRIDE_1_A", 0x70188 }, { "PLANE_SIZE_1_A", 0x70190 }, { "PLANE_OFFSET_1_A", 0x701a4 },
        { "PSR1_CTL_A", 0x60800 }, { "PSR2_CTL_A", 0x60900 },
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        IOLog("NullMoth DISP[%s] %s=0x%08x\n", tag, regs[i].n, r[regs[i].o / 4]);
    const uint32_t planeSurf = r[0x7019c / 4] & ~0xfffu;     /* GGTT offset the panel scans out right now */
    /* the firmware framebuffer is scanned out through the GGTT: log a few page-table entries and the first pixels of the
       pages they point at, to tell "the screen is blank" from "the plane no longer reads the framebuffer" */
    if (m->getLength() >= 0x800000 + 8 * 4001) {
        volatile uint64_t *gsm = (volatile uint64_t *)((uintptr_t)r + 0x800000);
        static const uint32_t pages[] = { 0, 1, 100, 1000, 2000, 3999, 4000 };
        for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
            uint64_t pte = gsm[pages[i]];
            IOLog("NullMoth DISP[%s] GGTT[%u]=0x%016llx\n", tag, pages[i], pte);
        }
    }
    m->release();

    /* what the plane scans out: count non-zero words in six 64 KiB windows of the framebuffer through the GTT aperture (BAR2),
       so an emptied picture shows up as 0 and a drawn one does not */
    IOMemoryMap *ap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapInhibitCache);
    if (ap != nullptr) {
        if (ap->getLength() >= 0x1000000) {
            volatile uint32_t *fbp = (volatile uint32_t *)ap->getVirtualAddress();
            static const uint32_t win[] = { 0, 0x100000, 0x400000, 0x800000, 0xc00000, 0xf00000 };
            for (unsigned i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
                uint32_t nz = 0, nb = 0, first = 0, firstnb = 0;
                for (uint32_t j = 0; j < 16384; j++) {
                    uint32_t v = fbp[win[i] / 4 + j];
                    if (v != 0) {
                        if (nz == 0)
                            first = v;
                        nz++;
                    }
                    if ((v & 0x00ffffff) != 0) {    /* a pixel with any colour in it: text or a drawn window, not black */
                        if (nb == 0)
                            firstnb = v;
                        nb++;
                    }
                }
                IOLog("NullMoth DISP[%s] FB[0x%x] nonzero=%u/16384 coloured=%u/16384 first=0x%08x firstcoloured=0x%08x\n", tag,
                      win[i], nz, nb, first, firstnb);
            }
            /* the same count over the buffer the plane actually scans (i915's buffer, which NMXGPUFB hands to
               WindowServer): this is what is on the panel */
            static const uint32_t swin[] = { 0, 0x400000, 0x800000, 0xc00000 };
            for (unsigned i = 0; planeSurf != 0 && i < sizeof(swin) / sizeof(swin[0]); i++) {
                if ((uint64_t)planeSurf + swin[i] + 65536 > ap->getLength()) break;
                uint32_t nz = 0, nb = 0, firstnb = 0;
                for (uint32_t j = 0; j < 16384; j++) {
                    uint32_t v = fbp[(planeSurf + swin[i]) / 4 + j];
                    if (v != 0) nz++;
                    if ((v & 0x00ffffff) != 0) { if (nb == 0) firstnb = v; nb++; }
                }
                IOLog("NullMoth DISP[%s] SCANOUT[0x%x+0x%x] nonzero=%u/16384 coloured=%u/16384 firstcoloured=0x%08x\n", tag,
                      planeSurf, swin[i], nz, nb, firstnb);
            }
        }
        ap->release();
    }

    /* PCI config space of the GPU: the log of two moments shows whether anything in it changed */
    for (unsigned off = 0; off < 0x100; off += 16)
        IOLog("NullMoth DISP[%s] PCICFG[%02x] %08x %08x %08x %08x\n", tag, off, pci->configRead32(off),
              pci->configRead32(off + 4), pci->configRead32(off + 8), pci->configRead32(off + 12));

    /* which framebuffer drivers macOS has right now */
    OSDictionary *match = IOService::serviceMatching("IOFramebuffer");
    if (match != nullptr) {
        OSIterator *it = IOService::getMatchingServices(match);
        unsigned n = 0;
        if (it != nullptr) {
            while (OSObject *o = it->getNextObject()) {
                IOService *svc = OSDynamicCast(IOService, o);
                if (svc != nullptr) {
                    IOLog("NullMoth DISP[%s] IOFramebuffer: %s (%s) regID=0x%llx inactive=%d busy=%u state=0x%x\n", tag,
                          svc->getName(), svc->getMetaClass()->getClassName(), svc->getRegistryEntryID(),
                          svc->isInactive() ? 1 : 0, (unsigned)svc->getBusyState(), (unsigned)svc->getState());
                    /* the framebuffer's own registry properties (mode counts, timing and display data), in log-sized pieces */
                    if (OSSerialize *ser = OSSerialize::withCapacity(16384)) {
                        if (svc->serializeProperties(ser)) {
                            const char *txt = ser->text();
                            size_t len = txt ? strlen(txt) : 0;
                            IOLog("NullMoth DISP[%s]   props: %lu bytes\n", tag, (unsigned long)len);
                            for (size_t o = 0; o < len && o < 12000; o += 180) {
                                char piece[184];
                                size_t n = len - o < 180 ? len - o : 180;
                                memcpy(piece, txt + o, n);
                                piece[n] = 0;
                                IOLog("NullMoth DISP[%s]   P: %s\n", tag, piece);
                            }
                        }
                        ser->release();
                    }
                    /* what hangs below it: the working boot has display0 (IODisplayConnect) and AppleDisplay here */
                    if (OSIterator *ci = svc->getChildIterator(gIOServicePlane)) {
                        unsigned nc = 0;
                        while (IORegistryEntry *c = OSDynamicCast(IORegistryEntry, ci->getNextObject())) {
                            IOLog("NullMoth DISP[%s]   child: %s (%s)\n", tag, c->getName(), c->getMetaClass()->getClassName());
                            nc++;
                            if (OSIterator *gi = c->getChildIterator(gIOServicePlane)) {
                                while (IORegistryEntry *g = OSDynamicCast(IORegistryEntry, gi->getNextObject()))
                                    IOLog("NullMoth DISP[%s]     grandchild: %s (%s)\n", tag, g->getName(),
                                          g->getMetaClass()->getClassName());
                                gi->release();
                            }
                        }
                        IOLog("NullMoth DISP[%s]   children=%u\n", tag, nc);
                        ci->release();
                    }
                    /* who owns it: walk up the provider chain, so the log shows whether it still hangs off the GPU */
                    IORegistryEntry *par = svc->getParentEntry(gIOServicePlane);
                    for (int d = 0; par != nullptr && d < 6; d++) {
                        IOLog("NullMoth DISP[%s]   parent%d: %s (%s)\n", tag, d, par->getName(),
                              par->getMetaClass()->getClassName());
                        par = par->getParentEntry(gIOServicePlane);
                    }
                    n++;
                }
            }
            it->release();
        }
        IOLog("NullMoth DISP[%s] IOFramebuffer count=%u\n", tag, n);
        match->release();
    }
}

void NMXGPU::attachNow()
{
    /* experiment knob nmxdelay=<seconds>: let macOS finish bringing its desktop up on the firmware framebuffer before the
       driver touches the GPU. The black-screen boots showed WindowServer finding no display modes at the moment the attach
       ran, so this tests whether the attach racing the desktop start is the cause. */
    {
        int delay = 0;
        if (PE_parse_boot_argn("nmxdelay", &delay, sizeof(delay)) && delay > 0) {
            if (delay > 900)
                delay = 900;
            IOLog("NullMoth GPU: nmxdelay=%d: waiting before the attach so the desktop starts first\n", delay);
            for (int i = 0; i < delay; i++)
                IOSleep(1000);
            IOLog("NullMoth GPU: nmxdelay done, attaching now\n");
        }
    }
    int err = nmx_xnu_wait_firmware_root(300);
    if (err) {
        IOLog("NullMoth GPU: the firmware folder never appeared (%d); this GPU stays with macOS\n", err);
        attach_ = -1;
        return;
    }
    if (nmx_marker_set()) {
        /* b209: the hung boot's last breadcrumb is still in nullmoth-last-step here, but the next good boot overwrites
           it (10-28: a cold start hung, the evidence was gone by the time it was read). Keep a copy that only this
           branch writes: nullmoth-hang-step, and say it in the log. */
        if (IODTNVRAM *n = nmx_nvram()) {
            OSObject *lo = n->copyProperty("nullmoth-last-step");
            OSData *last = OSDynamicCast(OSData, lo);
            char buf[100] = "(none)";
            if (last && last->getLength()) {
                unsigned len = last->getLength() < sizeof(buf) - 1 ? last->getLength() : (unsigned)sizeof(buf) - 1;
                memcpy(buf, last->getBytesNoCopy(), len); buf[len] = 0;
                const OSSymbol *k = OSSymbol::withCString("nullmoth-hang-step");
                if (k) { n->setProperty(k, last); k->release(); }
                n->sync();
            }
            if (lo) lo->release();
            n->release();
            IOLog("NullMoth GPU: the hung boot stopped after step \"%s\" (kept in nvram nullmoth-hang-step)\n", buf);
        }
        IOLog("NullMoth GPU: the last attach of %04x:%04x never returned (the Mac stopped or restarted); this GPU stays "
              "with macOS. Send logs, then turn the test driver off and on to try again\n",
              pci_->configRead16(kIOPCIConfigVendorID), pci_->configRead16(kIOPCIConfigDeviceID));
        setProperty("nullmoth-gpu-state", "parked: the last attach never returned");
        attach_ = -1;
        return;
    }
    nmx_marker_write(true);
    nmx_step("attach: start");
    IOLog("NullMoth GPU: driver built " __DATE__ " " __TIME__ " (b213+)\n");   /* b213: which build is really loaded */
    {
        /* diagnostic build: DRM debug messages are opt-in (boot-arg nmxdrm=<mask>, e.g. 22 = driver+KMS+atomic) */
        uint32_t dbg = 0;
        PE_parse_boot_argn("nmxdrm", &dbg, sizeof(dbg));
        __drm_debug = dbg;
    }
    nmx_dump_display(pci_, "before-attach");
    if (!headless(pci_->configRead16(kIOPCIConfigVendorID))) nmx_boot_snapshot();     /* keep the firmware screen for the bridge (nmx_fb.cpp) */
    dev_ = nmx_port_attach(pci_);
    nmx_step(dev_ ? "attach: i915 probe done" : "attach: no driver took the device");
    if (dev_ == nullptr) {
        nmx_marker_write(false);
        IOLog("NullMoth GPU: no driver took %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
              pci_->configRead16(kIOPCIConfigDeviceID));
        attach_ = -1;
        return;
    }

    {   /* experiment (10-17 freeze hunt): nmxgtawake=1 keeps the GT from parking between frames, 2 also blocks RC6 */
        /* default 2 (10-18): with the GT allowed into RC6, a paced 60 fps load still hard-froze the Mac after ~58 s
           (build 50, RCU fixed); with RC6 blocked it ran clean. Until the RC6 path is fixed, block it by default;
           nmxgtawake=0 restores normal power saving for testing */
        /* b141: RC6 allowed by default. 10-22: ~13 min accelerated with RC6 on (nmx-rc6-try) and many sleep/wake cycles
           (every sleep already released the hold) - no freeze; the 10-18 freeze predates the RCU / thread-cache fixes.
           The old block comes back with boot-arg nmxgtawake=2, or without editing boot-args: sudo nvram nmx-rc6-block=1 */
        /* b148: 10-23 a cold boot panicked 16 s in ("NMIPI for unresponsive processor: TLB flush timeout" - a CPU stuck
           with interrupts off, the shape of an MMIO read hanging on a powered-down GT). b141 had dropped the boot-time
           hold; every RC6 run that stayed clean (sleep/wake since b131, nmx-rc6-try) released it only once the desktop
           was up. So: hold it through the boot again and release it 90 s after attach (RC6 from then on, as b141).
           nmx-rc6-block=1 keeps the hold for good; an explicit nmxgtawake=<n> boot-arg is applied as given. */
        uint32_t aw = 2;
        govRc6At_ = 450;   /* governor ticks (200 ms) = 90 s */
        {
            IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
            if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
                OSObject *o = n->copyProperty("nmx-rc6-block");
                OSData *d = OSDynamicCast(OSData, o);
                if (d && d->getLength() >= 1 && *(const char *)d->getBytesNoCopy() == '1') govRc6At_ = 0;   /* keep it */
                OSSafeReleaseNULL(o);
            }
            if (e) e->release();
        }
        if (PE_parse_boot_argn("nmxgtawake", &aw, sizeof(aw))) govRc6At_ = 0;
        IOLog("NullMoth GPU: RC6 / GT parking %s\n", !aw ? "allowed (nmxgtawake=0)" :
              govRc6At_ ? "blocked for the boot, allowed 90 s after attach (b148)" : "BLOCKED (nmxgtawake / nmx-rc6-block=1)");
        if (aw > 0) {
            void *drm = nmx_drm_of(dev_);
            int got = drm ? nmx_i915_keep_awake((struct drm_device *)drm, (int)aw) : -2;
            if (got > 0) awake_ = got;
            static const char *what[] = { "off", "GT never parks", "GT never parks, RC6 blocked",
                                          "RC6 + power gating off, parking on", "forcewake: render only",
                                          "forcewake: GT only", "forcewake: media only" };
            IOLog("NullMoth GPU: nmxgtawake=%u -> %d (%s)\n", aw, got, got >= 0 && got <= 6 ? what[got] : "FAILED");
        }
    }
    {   /* b101: the GPU clock stayed at its lowest step (100 MHz of 100..1300) - see nmx_i915_gpu_floor. Default floor
           800 MHz; nmxgpumhz=<MHz> picks another (1300 = always full speed), nmxgpumhz=0 leaves i915's own choice. */
        uint32_t mhz = 550;   /* b146: 550 (was 800) - 10-23: 350-450 felt the same, 800 ran warmer */
        PE_parse_boot_argn("nmxgpumhz", &mhz, sizeof(mhz));
        if (mhz > 0) {
            void *drm = nmx_drm_of(dev_);
            int lo = 0, hi = 0;
            int got = drm ? nmx_i915_gpu_floor((struct drm_device *)drm, (int)mhz, &lo, &hi) : -2;
            IOLog("NullMoth GPU: nmxgpumhz=%u -> GPU clock floor %d MHz (part %d..%d MHz)%s\n", mhz, got, lo, hi,
                  got > 0 ? "" : " - NOT set");
        } else IOLog("NullMoth GPU: nmxgpumhz=0 - GPU clock left to i915\n");
        /* b138: idle clock. nmxgpuidle=<MHz> (default 350); nmxgpuidle=0 = the fixed floor above, as before b138 */
        /* b152: Windows-like steps 300 / 450 / 650 / 900 MHz by load (user: max 900, "in case any problem").
           nmxgpuidle=<MHz> the idle step (0 = no governor: the fixed floor above), nmxgpumax=<MHz> the top step */
        /* b153: top step 1300 MHz, the part's own maximum, as Windows runs it (user 10-23: 900 ran fine, wants Windows-like).
           Steps 300 / 500 / 850 / 1300. nmxgpumax=900 brings back a 900 MHz cap (steps 300 / 500 / 850 / 900). */
        uint32_t idle = 300, top = 1300;
        PE_parse_boot_argn("nmxgpuidle", &idle, sizeof(idle));
        PE_parse_boot_argn("nmxgpumax", &top, sizeof(top));
        if (top < 300) top = 300;
        if (top > 1300) top = 1300;
        govStep_[0] = (int)idle; govStep_[3] = (int)top;
        for (int k = 1; k < 3; k++) if (govStep_[k] > (int)top) govStep_[k] = (int)top;
        if (govStep_[0] > govStep_[1]) govStep_[0] = govStep_[1];
        if (mhz > 0 && idle > 0) {
            govBusy_ = (int)mhz; govIdle_ = (int)idle;
            gov_ = thread_call_allocate(&NMXGPU::govFire, this);
            if (gov_) { govOn_ = true; govArm(2000); }
            IOLog("NullMoth GPU: clock governor %s - steps %d / %d / %d / %d MHz by load (nmxgpuidle, nmxgpumax; nmxgpuidle=0: fixed floor)\n",
                  gov_ ? "on" : "FAILED", govStep_[0], govStep_[1], govStep_[2], govStep_[3]);
        }
    }
    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    /* experiment knobs for the black-screen hunt: nmxnopm=1 skips the power-management registration, nmxnoclaim=1 skips the
       claim property and the re-match on the PCI device. Both off by default. */
    uint32_t knob = 0;
    bool nopm = PE_parse_boot_argn("nmxnopm", &knob, sizeof(knob)) && knob != 0;
    knob = 0;
    bool noclaim = PE_parse_boot_argn("nmxnoclaim", &knob, sizeof(knob)) && knob != 0;
    IOLog("NullMoth GPU: experiment knobs: nmxnopm=%d nmxnoclaim=%d nmxnovga=%d\n", nopm ? 1 : 0, noclaim ? 1 : 0, nmx_novga());
    if (!nopm) {
        PMinit();
        pci_->joinPMtree(this);
        registerPowerDriver(this, states, 2);
        pm_ = true;
        makeUsable(); /* hold the device at full power: a bare PM registration can leave it at state 0, which suspends the display */
    }
    registerService();
    /* NMXAccel (the Metal accelerator for this GPU) starts only on a device this driver has claimed; it declined while
       the claim was pending, so matching on the device runs again now that it is set */
    if (!noclaim) {
        pci_->setProperty("nmxgpu-claimed", kOSBooleanTrue);
        pci_->registerService();
    }
    /* light the outputs and hand them to macOS; without it the device still renders, with no screen of its own */
    if (headless(pci_->configRead16(kIOPCIConfigVendorID))) {
        IOLog("NullMoth GPU: nmxnofb=1: display bridge not published; the screen stays as the firmware left it\n");
        setProperty("nullmoth-gpu-display", "headless (nmxnofb)");
        fb_ = nullptr;
    } else {
        nmx_step("attach: publishing the display");
        fb_ = nmx_fb_publish(this, dev_);
        nmx_step(fb_ ? "attach: display published" : "attach: display NOT published");
        /* the firmware-screen driver must not sit on this GPU next to ours: NMXFBHold keeps it off at matching
           time. Terminating it here instead hung attach (10-16, build 24). */
    }
    nmx_marker_write(false);
    nmx_step("attach: done");
    IOLog("NullMoth GPU: driving %04x:%04x\n", pci_->configRead16(kIOPCIConfigVendorID),
          pci_->configRead16(kIOPCIConfigDeviceID));
    OSSynchronizeIO();
    attach_ = 1;
    nmx_dump_display(pci_, "attach+0s");
    IOSleep(3000);
    nmx_dump_display(pci_, "attach+3s");
    IOSleep(10000);
    nmx_dump_display(pci_, "attach+13s");
    IOSleep(17000);
    nmx_dump_display(pci_, "attach+30s");
    IOSleep(30000);
    nmx_dump_display(pci_, "attach+60s");
    IOSleep(60000);
    nmx_dump_display(pci_, "attach+120s");
}

void NMXGPU::govArm(uint32_t ms)
{
    if (!gov_ || !govOn_) return;
    uint64_t dl; clock_interval_to_deadline(ms, kMillisecondScale, &dl);
    thread_call_enter_delayed(gov_, dl);
}
void NMXGPU::govStop()
{
    govOn_ = false;             /* a tick already running sees this and does not re-arm; it takes a few ms */
    if (gov_) { thread_call_cancel(gov_); IOSleep(250); }
}
void NMXGPU::govFire(thread_call_param_t p, thread_call_param_t)
{
    NMXGPU *me = (NMXGPU *)p;
    if (!me->govOn_ || !me->dev_ || !me->powered_) return;
    void *drm = nmx_drm_of(me->dev_);
    int util = 0, mhz = 0;
    /* b152: stepped governor. Load -> step: >= 60 % top, >= 30 % third, >= 5 % second, else idle. Up at once; down one
       step after 5 ticks (1 s) in a row that want a lower step. First 90 s after attach (boot + login): at least the
       third step (b151 idea - the CPU is busy with macOS's start-up then). */
    util = drm ? nmx_i915_gpu_util((struct drm_device *)drm) : -1;
    int want = util >= 60 ? 3 : util >= 30 ? 2 : util >= 5 ? 1 : 0;
    if (me->govRc6At_ && want < 2) want = 2;
    int lvl = me->govLevel_;
    if (lvl < 0 || want > lvl) { lvl = want; me->govWantLower_ = 0; }
    else if (want < lvl) { if (++me->govWantLower_ >= 5) { lvl--; me->govWantLower_ = 0; } }
    else me->govWantLower_ = 0;
    if (drm && lvl != me->govLevel_) { mhz = nmx_i915_gpu_pin((struct drm_device *)drm, me->govStep_[lvl]); me->govLevel_ = lvl; }
    const int st = lvl >= 2 ? 1 : 0;
    (void)mhz;
    me->govTicks_++;
    if (me->govRc6At_ && me->govTicks_ >= me->govRc6At_) {
        me->govRc6At_ = 0;
        if (me->awake_ > 0) { IOLog("NullMoth GPU: boot settled - RC6 / GT parking allowed from now on (b148)\n"); me->releaseAwake("boot settled"); }
    }
    /* b139: one-shot RC6 test. sudo nvram nmx-rc6-try=1 -> within 5 s the keep-awake hold (nmxgtawake, RC6 blocked
       since the 10-18 freeze) is released for THIS boot: the GT may park and enter RC6 (deep idle, the big power saving).
       The variable is deleted when read, so a freeze is gone after a forced restart (the next boot blocks RC6 again). */
    if ((me->govTicks_ % 25) == 0 && me->awake_ > 0) {
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            OSObject *o = n->copyProperty("nmx-rc6-try");
            OSData *d = OSDynamicCast(OSData, o);
            const bool go = d && d->getLength() >= 1 && *(const char *)d->getBytesNoCopy() == '1';
            OSSafeReleaseNULL(o);
            if (go) {
                const OSSymbol *k = OSSymbol::withCString("nmx-rc6-try");
                if (k) { n->removeProperty(k); k->release(); }
                n->sync();
                IOLog("NullMoth GPU: nmx-rc6-try=1 - releasing keep-awake level %d for this boot: RC6 / GT parking ALLOWED\n", me->awake_);
                me->releaseAwake("rc6 test");
                nmx_step("rc6 test: keep-awake released");
            }
        }
        if (e) e->release();
    }
    if (st == 1) me->govBusyTicks_++;
    if (st != me->govLast_) { me->govSwitches_++; me->govLast_ = st; }
    if ((me->govTicks_ % 300) == 1)       /* once a minute */
        IOLog("NullMoth GPU: clock governor: step %d (%d MHz) now, %d%% busy; busy %u%% of the last %u ticks, %u switches\n",
              me->govLevel_, me->govLevel_ >= 0 ? me->govStep_[me->govLevel_] : 0, util,
              me->govTicks_ ? me->govBusyTicks_ * 100 / me->govTicks_ : 0, me->govTicks_, me->govSwitches_);
    me->govArm(200);
}

void NMXGPU::stop(IOService *provider)
{
    govStop();
    if (gov_) { thread_call_free(gov_); gov_ = nullptr; }
    /* an attach still running on its thread finishes first (it holds a reference on this object meanwhile) */
    for (int i = 0; attach_ == 0 && i < 6000; i++)
        IOSleep(100);
    provider->removeProperty("nmxgpu-claimed");
    if (fb_ != nullptr)
        nmx_fb_unpublish(fb_, this, dev_);
    fb_ = nullptr;
    if (pm_)
        PMstop();
    pm_ = false;
    nmx_port_detach(dev_);
    dev_ = nullptr;
    IOLockLock(g_lock);
    g_devices--;
    IOLockUnlock(g_lock);
    IOService::stop(provider);
}

/* state 0 = sleep, 1 = on */
void NMXGPU::releaseAwake(const char *why)
{
    if (awake_ <= 0 || dev_ == nullptr) return;
    void *drm = nmx_drm_of(dev_);
    int r = drm ? nmx_i915_keep_awake_release((struct drm_device *)drm, awake_) : -2;
    IOLog("NullMoth GPU: %s: keep-awake level %d released (%d)\n", why, awake_, r);
    awake_ = 0;
}

IOReturn NMXGPU::setPowerState(unsigned long state, IOService *device)
{
    IOLog("NullMoth GPU: power state %lu requested (device is %s)\n", state, powered_ ? "on" : "off");
    if (state == 0 && dev_ != nullptr && powered_) { govStop(); releaseAwake("sleep"); }
    if (dev_ != nullptr && (state != 0) != powered_) {
        int err = nmx_port_power(dev_, state != 0);
        powered_ = state != 0;
        if (err)
            IOLog("NullMoth GPU: %s returned %d\n", state ? "resume" : "suspend", err);
        if (state != 0 && gov_ && !govOn_) { govOn_ = true; govLast_ = -1; govLevel_ = -1; govArm(1000); }   /* b138: governor back after wake */
    }
    return kIOPMAckImplied;
}

/* b80: NMXAccel asks for a DRM bridge client (nmx_drm_bridge.cpp) for a sandboxed process that cannot open /dev/dri */
IOReturn NMXGPU::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo("nmxNewDrmClient")) {
        if (attach_ != 1 || dev_ == nullptr) return kIOReturnNotReady;
        return nmx_drm_bridge_new(p1, (uint32_t)(uintptr_t)p2, (IOUserClient **)p3);
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

void NMXGPU::systemWillShutdown(IOOptionBits spec)
{
    govStop();
    releaseAwake("shutdown");
    if (dev_ != nullptr && PE_parse_boot_argn("-nmxnoshutdown", nullptr, 0))
        IOLog("NullMoth GPU: -nmxnoshutdown: the GPU is left as it is for the restart\n");
    else if (dev_ != nullptr) {
        IOLog("NullMoth GPU: shutdown: stopping the GPU\n");
        nmx_port_shutdown(dev_);
        IOLog("NullMoth GPU: shutdown: GPU stopped\n");
    }
    IOService::systemWillShutdown(spec);
}

/* ---- module start/stop: the stop refuses while device nodes are open ------------------------------------------------- */

extern "C" {
kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data);
kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data);
}

kern_return_t nmx_kmod_start(kmod_info_t *ki, void *data)
{
    g_lock = IOLockAlloc();
    return g_lock ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t nmx_kmod_stop(kmod_info_t *ki, void *data)
{
    IOLockLock(g_lock);
    if (g_started) {
        if (g_devices > 0 || nmx_port_busy() > 0) {
            IOLockUnlock(g_lock);
            IOLog("NullMoth GPU: still in use; not unloading\n");
            return KERN_FAILURE;
        }
        nmx_port_stop();
        g_started = false;
    }
    IOLockUnlock(g_lock);
    IOLockFree(g_lock);
    g_lock = nullptr;
    return KERN_SUCCESS;
}

/* libkmod's entry points; they call _realmain/_antimain below */
extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.nullmoth.NMXGPU, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nmx_kmod_start;
__private_extern__ kmod_stop_func_t *_antimain = nmx_kmod_stop;
}
