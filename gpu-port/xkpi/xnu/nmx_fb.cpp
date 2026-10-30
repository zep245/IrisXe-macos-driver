/*
 * The display bridge, IOKit side: an IOFramebuffer whose VRAM is the scan-out buffer the DRM side lit
 * (override/src/nmx_fb_bridge.c). WindowServer draws into it and the GPU's display engine scans it out; there is no
 * copy. One mode (the largest connected output's), XRGB8888.
 *
 * The IOFramebuffer behaviour (one fixed mode, timing, power acknowledged at once, the colour answers WindowServer
 * asks, a 60 Hz timer as the VBL until the display interrupt is wired) is the same as NMIntelFB's, which met the
 * same WindowServer on studio.
 */
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOPlatformExpert.h>
#include "nmx_port.h"
extern "C" void nmx_step(const char *what);
extern "C" volatile int nmx_steps_quiet;   /* b126: nmx_kext.cpp */

#define FBLOG(fmt, ...) IOLog("NullMoth display: " fmt "\n", ##__VA_ARGS__)

static IOPMPowerState kFbPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

/* Panel brightness: NMIntelFB's handler (kexts/NMIntelFB/intelfb.cpp), unchanged but for the names. A service named
   "backlight" under the framebuffer before WindowServer opens it makes IOGraphics create an AppleBacklightDisplay, which
   owns the brightness keys and slider; each change writes the panel PWM duty (BLC_PWM_CTL/FREQ/DUTY 0xC8250..8). The
   saved level is shared with NMIntelFB (NVRAM nmintelfb-brightness). Boot-arg -nmxfbnobl leaves brightness alone. */
static const uint32_t kBlcCtl = 0xC8250, kBlcFreq = 0xC8254, kBlcDuty = 0xC8258;
static const SInt32 kMinBacklight = 5;
class NMXBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(NMXBacklight)
    volatile uint8_t *fRegs = nullptr;
    uint32_t fFreq = 0;
    thread_call_t fApply = nullptr;
    volatile SInt32 fPending = -1;
    volatile bool fPendingLinear = false;   /* b217: which parameter the pending level came from (log) */
    SInt32 fLevel = 100;
    thread_call_t fSave = nullptr;
    volatile bool fPowered = true;
    /* macOS shows the slider at 100% after a restart although the panel is at the restored level: the display's
       parameter dictionary is rewritten after setDisplay. Write our level into it again a few seconds later. */
    IODisplay *fDisp = nullptr;
    thread_call_t fSync = nullptr;
    int fSyncLeft = 0;
    /* b215 (10-29 video: after boot, or when the screen came back after dimming / display sleep, the panel was at full
       brightness while the slider still showed the low level; the next brightness key fixed it). i915 lights the panel
       at ITS OWN backlight level whenever it enables the panel (the boot hand-over mode set, DPMS on, resume) - it
       never learns ours, because this handler writes the PWM duty register directly. A watch every 2 s: when the
       panel is on and the duty register no longer holds our level, it is written again (and the first few logged). */
    thread_call_t fWatch = nullptr;
    unsigned fWatchFixes = 0;
    static void watchFire(thread_call_param_t p, thread_call_param_t) {
        NMXBacklight *me = (NMXBacklight *)p;
        if (!me->fWatch) return;
        if (me->fRegs && me->fPowered && me->fFreq && me->fPending < 0) {
            const uint32_t want = me->dutyFor(me->fLevel);
            const uint32_t freq = *(volatile uint32_t *)(me->fRegs + kBlcFreq);
            const uint32_t duty = *(volatile uint32_t *)(me->fRegs + kBlcDuty);
            if (duty != want || freq != me->fFreq) {
                *(volatile uint32_t *)(me->fRegs + kBlcFreq) = me->fFreq;
                *(volatile uint32_t *)(me->fRegs + kBlcDuty) = want;
                if (me->fWatchFixes++ < 6)
                    FBLOG("backlight: panel was at duty %u (freq %u) - set back to our %d%% (duty %u); i915 relit it at its own level",
                          duty, freq, (int)me->fLevel, want);
            }
        }
        thread_call_t w = me->fWatch;
        if (w) { uint64_t dl; clock_interval_to_deadline(2000, kMillisecondScale, &dl); thread_call_enter_delayed(w, dl); }
    }
    static void syncFire(thread_call_param_t p, thread_call_param_t) {
        NMXBacklight *me = (NMXBacklight *)p;
        IODisplay *d = me->fDisp;
        if (d == nullptr) return;
        OSObject *old = d->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        if (cur) {
            OSDictionary *params = OSDictionary::withDictionary(cur);
            if (params) {
                IODisplay::setParameter(params, gIODisplayBrightnessKey, me->fUser);
                IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, me->fUser);
                d->setProperty(gIODisplayParametersKey, params);
                params->release();
                FBLOG("backlight: slider re-synced to %d%%", (int)me->fUser);
            }
        }
        OSSafeReleaseNULL(old);
        if (--me->fSyncLeft > 0) { uint64_t dl; clock_interval_to_deadline(15000, kMillisecondScale, &dl); thread_call_enter_delayed(me->fSync, dl); }
    }
    uint32_t dutyFor(SInt32 v) const {
        SInt32 pct = v < kMinBacklight ? kMinBacklight : (v > 100 ? 100 : v);
        return (uint32_t)(((uint64_t)fFreq * (uint64_t)pct) / 100);
    }
    // brightness survives a restart: kept in NVRAM as nmintelfb-brightness (one byte, 0-100), written 2 s after the last change
    static void saveFire(thread_call_param_t p, thread_call_param_t) {
        NMXBacklight *me = (NMXBacklight *)p;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e);
        if (n == nullptr) { if (e) e->release(); return; }
        uint8_t b = (uint8_t)(me->fUser < 0 ? 0 : (me->fUser > 100 ? 100 : me->fUser));   /* b224: the user's level, not a dimmed one */
        const OSSymbol *k = OSSymbol::withCString("nmintelfb-brightness");
        OSData *d = OSData::withBytes(&b, 1);
        if (k && d) n->setProperty(k, d);
        if (d) d->release();
        if (k) k->release();
        n->sync();
        n->release();
    }
public:
    static SInt32 savedLevel() {
        SInt32 v = -1;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            if (OSData *d = OSDynamicCast(OSData, n->getProperty("nmintelfb-brightness")))
                if (d->getLength() == 1) v = *(const uint8_t *)d->getBytesNoCopy();
        }
        if (e) e->release();
        return (v >= kMinBacklight && v <= 100) ? v : -1;
    }
    // display/system sleep: backlight off; wake: the PWM set up again (sleep can reset it) at the current level
    void setPowered(bool on) {
        if (!fRegs || on == fPowered) return;
        fPowered = on;
        if (on) {
            *(volatile uint32_t *)(fRegs + kBlcFreq) = fFreq;
            *(volatile uint32_t *)(fRegs + kBlcDuty) = dutyFor(fLevel);
            uint32_t ctl = *(volatile uint32_t *)(fRegs + kBlcCtl);
            if (!(ctl & (1u << 31))) *(volatile uint32_t *)(fRegs + kBlcCtl) = ctl | (1u << 31);
            FBLOG("backlight: on at %d%%", (int)fLevel);
        } else {
            *(volatile uint32_t *)(fRegs + kBlcDuty) = 0;
            FBLOG("backlight: off (sleep)");
        }
    }
    void applyNow(SInt32 v) { if (fRegs && fPowered) *(volatile uint32_t *)(fRegs + kBlcDuty) = dutyFor(v); }
private:
    static void applyFire(thread_call_param_t p, thread_call_param_t) {
        NMXBacklight *me = (NMXBacklight *)p;
        SInt32 v;
        do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
        if (v < 0 || !me->fRegs) return;
        uint32_t duty = me->dutyFor(v);
        me->fLevel = v;
        if (me->fPowered) *(volatile uint32_t *)(me->fRegs + kBlcDuty) = duty;
        FBLOG("backlight: %d%% (duty %u of %u)%s%s", (int)v, duty, me->fFreq, me->fPowered ? "" : " (kept for wake)",
              me->fPendingLinear ? " [linear-brightness]" : "");
        if (me->fSave) { uint64_t d; clock_interval_to_deadline(2000, kMillisecondScale, &d); thread_call_enter_delayed(me->fSave, d); }
        static int applied;
        if (applied < 3) { applied++; nmx_step("backlight: level applied"); }   /* first few only: spare the NVRAM */
    }
public:
    static NMXBacklight *with(volatile uint8_t *regs, uint32_t freq, SInt32 level) {
        NMXBacklight *h = OSTypeAlloc(NMXBacklight);
        if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
        h->fRegs = regs; h->fFreq = freq; h->fLevel = level < 0 ? 0 : (level > 100 ? 100 : level);
        h->fUser = h->fLevel;   /* b224/b225 */
        h->fApply = thread_call_allocate(applyFire, h);
        h->fSave = thread_call_allocate(saveFire, h);
        h->fSync = thread_call_allocate(syncFire, h);
        if (!h->fApply) { h->release(); return nullptr; }
        if (!PE_parse_boot_argn("-nmxnoblwatch", nullptr, 0) && (h->fWatch = thread_call_allocate(watchFire, h)) != nullptr) {
            uint64_t dl; clock_interval_to_deadline(2000, kMillisecondScale, &dl); thread_call_enter_delayed(h->fWatch, dl);
        }
        return h;
    }
    void detachRegs() { fRegs = nullptr; }
    bool setDisplay(IODisplay *display) override {
        if (!display) return false;
        OSObject *old = display->copyProperty(gIODisplayParametersKey);
        OSDictionary *cur = OSDynamicCast(OSDictionary, old);
        OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
        OSSafeReleaseNULL(old);
        if (!params) return false;
        IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, fUser);
        IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fUser);
        display->setProperty(gIODisplayParametersKey, params);
        params->release();
        if (fSync && fDisp == nullptr) {
            display->retain(); fDisp = display; fSyncLeft = 2;
            uint64_t dl; clock_interval_to_deadline(5000, kMillisecondScale, &dl); thread_call_enter_delayed(fSync, dl);
        }
        if (!fShownAt) fShownAt = mach_absolute_time();   /* b219 */
        FBLOG("backlight: brightness offered on %s, panel at %d%%", display->getName(), (int)fLevel);
        nmx_step("backlight: brightness offered to macOS");
        return true;
    }
    /* b226 (Tahoe log 10-30 09:04): b225 treated "brightness" as an absolute level on the slider's scale - true on
       Sequoia by chance (slider 12-16, brightness 12). On Tahoe the slider was at 100 and after display sleep macOS
       sent brightness 11: the panel stayed at 11% with the slider at full; after a restart the same. What "brightness"
       carries reliably, on both systems: 1 = dim before display sleep, 0 = backlight off, and some other value ("12",
       "11", "100") = back to normal - its number is macOS's own stored value, not the slider. So:
         linear-brightness -> the user's level: panel and slider;
         brightness <= 3   -> dim / off: panel at that level (the user level is kept);
         brightness > 3    -> "normal again": panel back at the user's level.
       Boot-arg nmxbllinear=1: both set the panel directly (before b217). */
    SInt32 fUser = 100;          /* linear-brightness: the user's level, 0-100 (saved, shown on the slider) */
    uint64_t fShownAt = 0;
    bool doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value) override {
        if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey) return false;
        static int both = -1;
        if (both < 0) { int v = 0; both = (PE_parse_boot_argn("nmxbllinear", &v, sizeof v) && v) ? 1 : 0; }
        const bool linear = (paramName == gIODisplayLinearBrightnessKey);
        SInt32 v = value > 100 ? 100 : (SInt32)value;
        if (linear) fUser = v;
        else if (!both) {
            static unsigned said;
            if (v > 3) {
                if (said++ < 16) FBLOG("backlight: brightness %d from macOS = normal again - panel at the user's %d%%", (int)v, (int)fUser);
                /* b228: macOS now believes the level is v (100 / 11 / 12) and Control Center's slider shows that until
                   it reads the display's parameters again (10-30: full for ~1 s after opening, then the real level).
                   Write the user's level into the parameters at once, so the slider is right from the start. */
                if (v != fUser && fSync && fDisp) {
                    fSyncLeft = 1;
                    uint64_t dl; clock_interval_to_deadline(150, kMillisecondScale, &dl); thread_call_enter_delayed(fSync, dl);
                }
                v = fUser;
            } else {
                /* b227: dim like a real MacBook - half of the user's level (never below the panel minimum), not the
                   minimum macOS's "1" would give; 0 stays the backlight going off */
                const SInt32 req = v;
                if (v > 0) v = fUser / 2;
                if (said++ < 16) FBLOG("backlight: brightness %d from macOS = %s - panel %d%% (user level %d%% kept)", (int)req,
                                       v ? "dim before display sleep" : "backlight off", (int)v, (int)fUser);
            }
        }
        fPendingLinear = linear;
        fPending = v;
        thread_call_enter(fApply);
        return true;
    }
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override {
        if (fApply) { thread_call_cancel(fApply); while (!thread_call_free(fApply)) { thread_call_cancel(fApply); IOSleep(1); } fApply = nullptr; }
        if (fSave) { thread_call_cancel(fSave); while (!thread_call_free(fSave)) { thread_call_cancel(fSave); IOSleep(1); } fSave = nullptr; }
        if (fWatch) { thread_call_t w = fWatch; fWatch = nullptr; thread_call_cancel(w); while (!thread_call_free(w)) { thread_call_cancel(w); IOSleep(1); } }
        if (fSync) { thread_call_cancel(fSync); while (!thread_call_free(fSync)) { thread_call_cancel(fSync); IOSleep(1); } fSync = nullptr; }
        OSSafeReleaseNULL(fDisp);
        IODisplayParameterHandler::free();
    }
};
OSDefineMetaClassAndStructors(NMXBacklight, IODisplayParameterHandler)

class NMXGPUFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMXGPUFB)
    void *fDev = nullptr;
    nmx_fb_info fInfo = {};
    uint32_t fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; bool on = true; };
    Irq fVbl, fConnect;
    /* b102: more than one client may register for VBL (IOFramebuffer itself, the accelerator's display pipe); the single
       slot let a later registration silently replace an earlier one. Up to 4 now, each with its own on/off state. */
    enum { kVblMax = 4 };
    Irq fVblX[kVblMax];
    unsigned fIrqLogs = 0;
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    thread_call_t fPmCall = nullptr;
    /* IOFramebuffer has no controller (and its power/event code dereferences NULL) until WindowServer opens the
       framebuffer; open() calls enableController(), so that is where power registration is started (10-16) */
    volatile bool fOpened = false;
    /* b164: boot screen hand-over (nmx_fb_go) - from WindowServer's open, or after nmxdeferto seconds (default 120) */
    thread_call_t fGoCall = nullptr;
    volatile const char *fGoWhy = "timeout";
    /* b165: with the auto-arm (nmxws=1 nmxautoarm=1) the first WindowServer is restarted ~10 s later to come back
       accelerated. "Hold" keeps the boot logo through that: the hand-over waits for the accelerated WindowServer's first
       frame (the bridge's flip), for the auto-arm helper's signal (NVRAM nmx-handover=1, written when it is done or
       gives up), or for nmxdeferto seconds (default 60). -nmxnohold: hand over at the first open, as b164. */
    bool fHold = false;
    uint64_t fGoDeadline = 0;
    /* b183: the hold waits for an accelerated WindowServer - pointless without NMXAccel (Tahoe T1 test: NMXGPU alone,
       same boot-args as Sequoia -> the logo stayed for the 60 s timeout). 15 s after start with no NMXAccel: hand over. */
    uint64_t fNoAccelBy = 0;
    static bool accelPresent() {
        OSDictionary *m = IOService::serviceMatching("NMXAccel");
        if (m == nullptr) return true;           /* cannot tell: keep holding */
        OSIterator *it = IOService::getMatchingServices(m);
        m->release();
        bool yes = false;
        if (it) { yes = it->getNextObject() != nullptr; it->release(); }
        return yes;
    }
    /* b168: the helper's signal must be THIS boot's: b167 handed over at 12 s on a signal left in NVRAM by the boot
       before. NMXGPUFB publishes a per-boot token (ioreg NMXHoldToken); only nmx-handover=<that token> counts. */
    char fHoldToken[20] = {0};
    uint64_t fSignalAt = 0;     /* b169: deadline for the first picture after the helper's signal */
    uint64_t fFlipBy = 0;       /* b174: deadline for the restarted WindowServer's first frame after the signal */
    bool nvramFlagTake(const char *name) {
        bool on = false;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            OSObject *o = n->copyProperty(name);
            OSData *d = OSDynamicCast(OSData, o);
            const size_t tl = strlen(fHoldToken);
            on = d && tl && d->getLength() >= tl && !memcmp(d->getBytesNoCopy(), fHoldToken, tl);
            OSSafeReleaseNULL(o);
            if (on) {
                const OSSymbol *k = OSSymbol::withCString(name);
                if (k) { n->removeProperty(k); k->release(); }
                n->sync();
            }
        }
        if (e) e->release();
        return on;
    }
    /* the auto-arm helper's guard: after a boot that stopped inside an ARM / RESTART step it does not arm - no hold then */
    static bool autoarmWillArm() {
        int ws = 0, aa = 0;
        if (!PE_parse_boot_argn("nmxws", &ws, sizeof ws) || !ws) return false;
        if (!PE_parse_boot_argn("nmxautoarm", &aa, sizeof aa) || aa != 1) return false;
        bool tripped = false;
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            OSObject *o = n->copyProperty("nmx-autoarm-step");
            if (OSData *d = OSDynamicCast(OSData, o)) {
                char b[160]; unsigned l = d->getLength() < sizeof b - 1 ? d->getLength() : (unsigned)sizeof b - 1;
                memcpy(b, d->getBytesNoCopy(), l); b[l] = 0;
                tripped = strnstr(b, " ARM", sizeof b) != nullptr || strnstr(b, " RESTART", sizeof b) != nullptr;
                if (tripped) FBLOG("hold: the auto-arm guard is set (%s) - no hold this boot", b);
            }
            OSSafeReleaseNULL(o);
        }
        if (e) e->release();
        return !tripped;
    }
    static void goFire(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        if (me->fStopping) return;
        if (me->fHold) {
            if (!nmx_fb_deferred(me->fDev)) { me->nvramFlagTake("nmx-handover"); return; }
            uint64_t now = mach_absolute_time();
            if (!me->fSignalAt && !PE_parse_boot_argn("-nmxholdpic", nullptr, 0) && me->nvramFlagTake("nmx-handover")) {
                /* b174: hand over with the restarted WindowServer's first frame, not buffer 0's old lock screen;
                   if no frame comes within 2 s, hand over anyway. b198: 2 s, was 4 - with the accelerated WindowServer
                   the frames reach the panel by copy-forward, not by a flip, so the 4 s always ran out (Tahoe 10-28:
                   "no new frame in 4 s", 4 s of every boot) */
                if (nmx_fb_go_on_flip(me->fDev, "autoarm helper signal, first frame of the restarted WindowServer") == 0) {
                    uint64_t d; clock_interval_to_deadline(2000, kMillisecondScale, &d);
                    me->fFlipBy = d;
                    FBLOG("hold: auto-arm helper signal - handing over with the next frame (at most 2 s)");
                } else nmx_fb_go(me->fDev, "autoarm helper signal");
            }
            if (me->fFlipBy) {
                if (mach_absolute_time() >= me->fFlipBy) nmx_fb_go(me->fDev, "autoarm helper signal, no new frame in 2 s");
                else { uint64_t d; clock_interval_to_deadline(100, kMillisecondScale, &d); thread_call_enter_delayed(me->fGoCall, d); }
                return;
            }
            if (!me->fSignalAt && me->nvramFlagTake("nmx-handover")) {
                /* b169: the helper is done - now wait for the restarted WindowServer's first real picture (its first
                   frames are black with only the pointer), at most 10 s */
                uint64_t d; clock_interval_to_deadline(10000, kMillisecondScale, &d);
                me->fSignalAt = d;
                FBLOG("hold: auto-arm helper signal - waiting for the lock screen picture (at most 10 s)");
            }
            if (me->fSignalAt) {
                const int lit = nmx_fb_back_content(me->fDev);
                if (lit >= 8) nmx_fb_go(me->fDev, "autoarm helper signal, lock screen drawn");
                else if (now >= me->fSignalAt) nmx_fb_go(me->fDev, "autoarm helper signal, no picture after 10 s");
                else { uint64_t d; clock_interval_to_deadline(100, kMillisecondScale, &d); thread_call_enter_delayed(me->fGoCall, d); }
            }
            else if (now >= me->fGoDeadline) nmx_fb_go(me->fDev, "hold timeout");
            else if (now >= me->fNoAccelBy && !me->accelPresent()) nmx_fb_go(me->fDev, "no accelerator (NMXAccel) after 15 s");
            else { uint64_t d; clock_interval_to_deadline(250, kMillisecondScale, &d); thread_call_enter_delayed(me->fGoCall, d); }
            return;
        }
        nmx_fb_go(me->fDev, (const char *)me->fGoWhy);
    }
    IOMemoryMap *fMMIO = nullptr;
    NMXBacklight *fBacklight = nullptr;
    /* b175: the name and memory size System Information / About This Mac show for the GPU. macOS does not read them
       from the hardware (the PCI device only gives the IDs 8086:9a49): on a real Mac Apple's graphics driver sets them,
       so ours does. Not set when the config already injects a model (OpenCore DeviceProperties win).
       1536 MB = what Apple shows for an Intel GPU sharing 8 GB of RAM (Iris Xe has no memory of its own). */
    void publishGPUName(IOService *provider) {
        IOService *p = provider;
        IOPCIDevice *pci = nullptr;
        for (int i = 0; p && i < 3 && !pci; i++, p = p->getProvider()) pci = OSDynamicCast(IOPCIDevice, p);
        if (pci == nullptr) return;
        OSObject *m = pci->copyProperty("model");
        if (m == nullptr) {
            static const char name[] = "Intel Iris Xe Graphics";
            pci->setProperty("model", (void *)name, sizeof name);
            uint64_t vram = 1536ull << 20;
            pci->setProperty("VRAM,totalsize", &vram, sizeof vram);
            pci->setProperty("VRAM,totalMB", (unsigned long long)1536, 32);
            FBLOG("GPU name for About This Mac: %s, %u MB shared", name, 1536u);
        } else FBLOG("GPU name: the config sets \"model\" - left as it is");
        OSSafeReleaseNULL(m);
    }
    void publishBacklight(IOService *provider) {
        if (PE_parse_boot_argn("-nmxfbnobl", nullptr, 0)) { FBLOG("backlight: off by boot-arg"); return; }
        IOService *p = provider;
        IOPCIDevice *pci = nullptr;
        for (int i = 0; p && i < 3 && !pci; i++, p = p->getProvider()) pci = OSDynamicCast(IOPCIDevice, p);
        if (pci == nullptr) { nmx_step("fb: backlight: no PCI device"); return; }
        fMMIO = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
        if (fMMIO == nullptr) { nmx_step("fb: backlight: BAR0 not mapped"); return; }
        volatile uint8_t *regs = (volatile uint8_t *)fMMIO->getVirtualAddress();
        uint32_t ctl = *(volatile uint32_t *)(regs + kBlcCtl), freq = *(volatile uint32_t *)(regs + kBlcFreq),
                 duty = *(volatile uint32_t *)(regs + kBlcDuty);
        FBLOG("backlight: BLC_PWM_CTL 0x%08x FREQ 0x%08x DUTY 0x%08x", ctl, freq, duty);
        if (!(ctl & (1u << 31)) || freq == 0 || duty > freq) { nmx_step("fb: backlight: PWM not enabled, no brightness control"); return; }
        SInt32 level = (SInt32)(((uint64_t)duty * 100 + freq / 2) / freq);
        SInt32 saved = NMXBacklight::savedLevel();
        if (saved >= 0) level = saved;
        fBacklight = NMXBacklight::with(regs, freq, level);
        if (!fBacklight) return;
        if (saved >= 0) fBacklight->applyNow(saved);
        fBacklight->setName("backlight");
        if (!fBacklight->attach(this)) { OSSafeReleaseNULL(fBacklight); return; }
        fBacklight->registerService();
        nmx_step("fb: backlight handler published");
        /* b134 (slider): 10-22 corebrightnessd "Found 0 useful devices" and the panel attached as a plain AppleDisplay, so
           Control Center's slider never learnt the restored level. AppleBacklightDisplay (macOS's laptop-panel class)
           wants: a built-in connection (kConnectionFlags), a generic-LCD sense, a service named "backlight" (ours), and
           "AAPL,backlight-control" on the GPU (what WhateverGreen injects on real Intel laptops). -nmxnobldisp: skip. */
        if (!PE_parse_boot_argn("-nmxnobldisp", nullptr, 0)) {
            fBuiltIn = true;
            setProperty("built-in", kOSBooleanTrue);
            if (!pci->getProperty("AAPL,backlight-control")) {
                const uint32_t one = 1;
                OSData *d = OSData::withBytes(&one, sizeof one);
                if (d) { pci->setProperty("AAPL,backlight-control", d); setProperty("AAPL,backlight-control", d); d->release(); }
            }
            FBLOG("backlight: panel offered as BUILT-IN (AppleBacklightDisplay); -nmxnobldisp turns this off");
        }
    }
    bool fBuiltIn = false;
    bool fPmWanted = false;
    static void pmRegister(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        if (me->fStopping) return;
        nmx_step("fb: power: registering (deferred)");
        IOReturn r = me->registerPowerDriver(me, kFbPowerStates, 2);
        nmx_step(r == kIOReturnSuccess ? "fb: power: registered" : "fb: power: registration FAILED");
        /* b103: readiness signal for the Phase 3 auto-arm helper (ioreg NMXFBPower): it arms only after this */
        me->setProperty("NMXFBPower", r == kIOReturnSuccess ? "registered" : "failed");
    }

    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        if (me->fStopping) return;
        for (unsigned i = 0; i < kVblMax; i++) {
            Irq &q = me->fVblX[i];
            if (q.proc) q.proc(q.target, q.ref);   /* on/off is logged, not obeyed yet: ignoring it is what worked so far */
        }
        {   /* b98: the VBL rate macOS gets, one line every 10 s for the first 30 minutes.
               b100: with it the GPU's actual clock (GEN12_RPSTAT1 0x1381b4, CAGF bits 19:11, x 50/3 MHz) sampled each
               tick - min / average / max - and the clock range the part allows (RP_STATE_CAP 0x145998). If i915's
               frequency scaling never raises the clock (no PM interrupts in this port), every GPU frame runs at the
               lowest clock and WindowServer waits on it: the small lags of 10-20. */
            static uint64_t win; static unsigned n, lines, procs, fmin = ~0u, fmax, fn; static uint64_t fsum;
            const uint64_t t = mach_absolute_time();
            n++; for (unsigned i = 0; i < kVblMax; i++) if (me->fVblX[i].proc) { procs++; break; }
            if (me->fMMIO) {
                const volatile uint8_t *r = (const volatile uint8_t *)me->fMMIO->getVirtualAddress();
                const uint32_t st = *(const volatile uint32_t *)(r + 0x1381b4);
                const unsigned mhz = (unsigned)((((st >> 11) & 0x1ff) * 50 + 1) / 3);
                if (mhz < fmin) fmin = mhz;
                if (mhz > fmax) fmax = mhz;
                fsum += mhz; fn++;
            }
            if (!win) win = t;
            else if (t - win >= 10000000000ull) {
                if (lines < 180) {
                    lines++;
                    unsigned rp0 = 0, rp1 = 0, rpn = 0;
                    if (me->fMMIO) {
                        const uint32_t cap = *(const volatile uint32_t *)((const volatile uint8_t *)me->fMMIO->getVirtualAddress() + 0x145998);
                        rp0 = ((cap & 0xff) * 50 + 1) / 3; rp1 = (((cap >> 8) & 0xff) * 50 + 1) / 3; rpn = (((cap >> 16) & 0xff) * 50 + 1) / 3;
                    }
                    FBLOG("VBL: %u ticks in %llu ms, %u delivered to IOFramebuffer | GPU clock min %u avg %u max %u MHz (part: %u..%u, efficient %u)",
                          n, (t - win) / 1000000ull, procs, fn ? fmin : 0, fn ? (unsigned)(fsum / fn) : 0, fmax, rpn, rp0, rp1);
                }
                win = t; n = procs = 0; fmin = ~0u; fmax = 0; fsum = 0; fn = 0;
            }
        }
        uint64_t period;
        nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }

    /* b94: hardware cursor (override/src/nmx_fb_bridge.c, the display engine's cursor plane). IOFramebuffer hands the
       image and position here instead of drawing the pointer into the desktop; the DRM calls run on a thread call so the
       HID/WindowServer path that moves the pointer never waits on the display engine. -nmxnohwcursor turns it off. */
    bool fHwCursor = false;
    uint32_t fCurDim = 0;
    volatile uint32_t *fCurKva[2] = { nullptr, nullptr };
    UInt32 *fCurStage = nullptr;
    size_t fCurStageBytes = 0;
    IOLock *fCurLock = nullptr;
    thread_call_t fCurCall = nullptr;
    int fCurWant = -1, fCurOnPlane = -1;
    SInt32 fCurX = 0, fCurY = 0;
    bool fCurVis = false;
    unsigned fCurImgLogs = 0, fCurErrLogs = 0;
    volatile SInt32 fCurApplies = 0;
    void cursorSetup() {
        if (PE_parse_boot_argn("-nmxnohwcursor", nullptr, 0)) { FBLOG("cursor: hardware cursor off (-nmxnohwcursor)"); return; }
        void *k0 = nullptr, *k1 = nullptr; uint32_t dim = 0;
        int err = nmx_fb_cursor_init(fDev, &dim, &k0, &k1);
        if (err || !dim || !k0 || !k1) { FBLOG("cursor: no hardware cursor (%d) - macOS draws the pointer", err); return; }
        fCurStageBytes = (size_t)dim * dim * 4;
        fCurStage = (UInt32 *)IOMalloc(fCurStageBytes);
        fCurLock = IOLockAlloc();
        fCurCall = thread_call_allocate(curFire, this);
        if (!fCurStage || !fCurLock || !fCurCall) { FBLOG("cursor: out of memory - macOS draws the pointer"); cursorTeardown(); return; }
        fCurDim = dim; fCurKva[0] = (volatile uint32_t *)k0; fCurKva[1] = (volatile uint32_t *)k1;
        fHwCursor = true;
        FBLOG("cursor: hardware cursor on (%ux%u plane)", dim, dim);
        nmx_step("fb: hardware cursor on");
    }
    void cursorTeardown() {
        fHwCursor = false;
        if (fCurCall) { thread_call_cancel(fCurCall); while (!thread_call_free(fCurCall)) { thread_call_cancel(fCurCall); IOSleep(1); } fCurCall = nullptr; }
        if (fCurStage) { IOFree(fCurStage, fCurStageBytes); fCurStage = nullptr; }
        if (fCurLock) { IOLockFree(fCurLock); fCurLock = nullptr; }
    }
    static void curFire(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        if (me->fStopping || !me->fHwCursor) return;
        IOLockLock(me->fCurLock);
        const int img = me->fCurWant; const SInt32 x = me->fCurX, y = me->fCurY; const bool vis = me->fCurVis;
        me->fCurOnPlane = img;           /* from here new images go to the other buffer */
        IOLockUnlock(me->fCurLock);
        int err = nmx_fb_cursor_apply(me->fDev, img, x, y, vis ? 1 : 0);
        const unsigned n = (unsigned)OSIncrementAtomic(&me->fCurApplies);
        if (err && me->fCurErrLogs < 8) { me->fCurErrLogs++; FBLOG("cursor: apply %u refused (%d) img %d at %d,%d vis %d", n, err, img, (int)x, (int)y, (int)vis); }
        else if (n < 3 || (n % 5000) == 0) FBLOG("cursor: apply %u img %d at %d,%d vis %d", n, img, (int)x, (int)y, (int)vis);
    }

public:
    /* 10-18 (Retina sizes): desktop sizes macOS can pick in Displays. WindowServer cannot scale HiDPI modes without a
       GPU of its own, so smaller sizes are real modes: macOS draws at that size and the display engine's panel fitter
       scales it to the panel (larger UI, fewer pixels for the software desktop to draw). Mode 1 is the panel's own. */
    enum { kMaxModes = 8 };
    uint32_t fModeW[kMaxModes], fModeH[kMaxModes]; uint32_t fModeCount = 1; IODisplayModeID fCurMode = 1;
    void buildModes() {
        static const uint32_t widths[] = { 1920, 1680, 1600, 1440, 1280 };
        fModeW[0] = fInfo.width; fModeH[0] = fInfo.height; fModeCount = 1;
        if (PE_parse_boot_argn("-nmxfbonemode", nullptr, 0)) return;
        for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]) && fModeCount < kMaxModes; i++) {
            uint32_t w = widths[i];
            if (w >= fInfo.width || ((uint64_t)fInfo.height * w) % fInfo.width) continue;   /* same shape only */
            uint32_t h = (uint32_t)((uint64_t)fInfo.height * w / fInfo.width);
            if (h & 1) continue;
            fModeW[fModeCount] = w; fModeH[fModeCount] = h; fModeCount++;
        }
    }
    bool modeOK(IODisplayModeID m) const { return m >= 1 && (uint32_t)m <= fModeCount; }

    bool initWith(void *dev, const nmx_fb_info &info) {
        if (!init()) return false;
        fDev = dev;
        fInfo = info;
        buildModes();
        if (info.pclk_khz && info.htotal && info.vtotal) {
            uint64_t mhz = (uint64_t)info.pclk_khz * 1000000ull / ((uint64_t)info.htotal * info.vtotal);   /* milli-Hz */
            if (mhz >= 20000 && mhz <= 500000) {
                fRefresh16 = (uint32_t)((mhz << 16) / 1000);
                fFrameNs = 1000000000000ull / mhz;
            }
        }
        return true;
    }

    bool start(IOService *provider) override {
        publishBacklight(provider);   /* before IOFramebuffer::start: open() looks for "backlight" */
        publishGPUName(provider);
        nmx_step("fb: IOFramebuffer::start");
        if (!IOFramebuffer::start(provider)) { nmx_step("fb: IOFramebuffer::start FAILED"); return false; }
        nmx_step("fb: IOFramebuffer::start returned");
        /* b85b (10-19): set AFTER IOFramebuffer::start, as NMIntelFB did when it joined the NVIDIA group (b85 set it
           before start and NMXGPU did not come up). Head 0 of the NMXAccel display group, as NVRMFB heads are. Without IOFBDependentIndex the AGDC
           slot table left this framebuffer out ("fbmap n=0 (left out: noidx 1)") and CoreDisplay logged "Unable to find
           display pipe for IOFB service" - WindowServer drew on the GPU but never sent a frame (iop_ok 0). Only with
           nmxws=1 (the WindowServer-on-GPU config); a normal boot keeps the standalone panel. */
        {
            int ws = 0;
            if (PE_parse_boot_argn("nmxws", &ws, sizeof ws) && ws) {
                setProperty("IOFBDependentID", 0x4E4D584700000001ull, 64);
                setProperty("IOFBDependentIndex", 0ull, 32);
                FBLOG("display group: IOFBDependentIndex 0 (nmxws=1)");
                /* b86: the panel timing in NVRMFB's "NVRMTiming" layout, so NVRMAGDC (debug.nvrmfb_agdc_k5=1) can answer
                   WindowServer's link-configuration query (AGDC 0x921, refused on 10-19) as it does for NVIDIA heads */
                const nmx_fb_info &t = fInfo;
                if (t.pclk_khz && t.htotal > t.width && t.vtotal > t.height) {
                    uint32_t mhz = (uint32_t)((uint64_t)t.pclk_khz * 1000000ull / ((uint64_t)t.htotal * t.vtotal));
                    UInt32 k5[13] = { 0x4b350001u, t.pclk_khz * 1000u, t.width, t.hsync_start, t.hsync_end, t.htotal,
                                      t.height, t.vsync_start, t.vsync_end, t.vtotal, mhz, 1u, 1u };
                    if (OSData *k5d = OSData::withBytes(k5, sizeof k5)) { setProperty("NVRMTiming", k5d); k5d->release(); }
                    FBLOG("NVRMTiming %ux%u pclk %u kHz h %u/%u/%u v %u/%u/%u", t.width, t.height, t.pclk_khz,
                          t.hsync_start, t.hsync_end, t.htotal, t.vsync_start, t.vsync_end, t.vtotal);
                } else FBLOG("NVRMTiming NOT published: no panel timing");
            }
        }
        cursorSetup();
        if ((fGoCall = thread_call_allocate(goFire, this)) != nullptr) {
            fHold = nmx_fb_deferred(fDev) && !PE_parse_boot_argn("-nmxnohold", nullptr, 0) && autoarmWillArm();
            int to = fHold ? 60 : 120;
            if (PE_parse_boot_argn("nmxdeferto", &to, sizeof to) && (to < 5 || to > 900)) to = fHold ? 60 : 120;
            uint64_t gd; clock_interval_to_deadline((uint32_t)to * 1000u, kMillisecondScale, &gd);
            fGoDeadline = gd;
            clock_interval_to_deadline(15000, kMillisecondScale, &fNoAccelBy);
            if (fHold) {
                snprintf(fHoldToken, sizeof fHoldToken, "%llx", (unsigned long long)(mach_absolute_time() ^ 0x5a5a17ull));
                setProperty("NMXHoldToken", fHoldToken);
                FBLOG("hold: the boot logo stays until the accelerated WindowServer draws (auto-arm), at most %d s (-nmxnohold: off)", to);
                uint64_t d; clock_interval_to_deadline(250, kMillisecondScale, &d);
                thread_call_enter_delayed(fGoCall, d);
            } else thread_call_enter_delayed(fGoCall, gd);
        }
        fVblTimer = thread_call_allocate(vblFire, this);
        if (fVblTimer) {
            uint64_t d;
            clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d);
            fVblNext = d;
            thread_call_enter_delayed(fVblTimer, d);
        }
        /* registerPowerDriver() inside start() never returned for NMIntelFB on the Tiger Lake laptop (10-15); register
           from a thread call 2 s after start returns. Boot-arg -nmxfbnopm skips the registration. */
        if (PE_parse_boot_argn("-nmxfbnopm", nullptr, 0)) {
            nmx_step("fb: power registration skipped (-nmxfbnopm)");
            setProperty("NMXFBPower", "skipped");
        } else if ((fPmCall = thread_call_allocate(pmRegister, this)) != nullptr) {
            fPmWanted = true;
            nmx_step("fb: power registration waits for open");
        }
        FBLOG("framebuffer %ux%u at %u.%02u Hz", fInfo.width, fInfo.height, fRefresh16 >> 16,
              ((fRefresh16 & 0xFFFF) * 100) >> 16);
        return true;
    }

    void stop(IOService *provider) override {
        fStopping = true;
        /* thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out */
        if (fPmCall) { thread_call_cancel(fPmCall); while (!thread_call_free(fPmCall)) { thread_call_cancel(fPmCall); IOSleep(5); } fPmCall = nullptr; }
        if (fGoCall) { thread_call_cancel(fGoCall); while (!thread_call_free(fGoCall)) { thread_call_cancel(fGoCall); IOSleep(5); } fGoCall = nullptr; }
        if (fVblTimer) {
            thread_call_cancel(fVblTimer);
            while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); }
            fVblTimer = nullptr;
        }
        cursorTeardown();
        if (fBacklight) { fBacklight->detachRegs(); fBacklight->detach(this); OSSafeReleaseNULL(fBacklight); }
        OSSafeReleaseNULL(fMMIO);
        { IOMemoryMap *m = fScanMap; fScanMap = nullptr; if (m) m->release(); }
        if (fFlipCall) { while (!thread_call_free(fFlipCall)) { thread_call_cancel(fFlipCall); IOSleep(5); } fFlipCall = nullptr; }
        if (fFlipLock) { IOLockFree(fFlipLock); fFlipLock = nullptr; }
        IOFramebuffer::stop(provider);
    }
    IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended,
                           UInt32 *displayType) override {
        if (connectIndex != 0 || !fBacklight) return kIOReturnUnsupported;
        if (senseType) *senseType = 0;
        if (primary) *primary = 0;
        if (extended) *extended = 0;
        if (displayType) *displayType = kGenericLCD;
        return kIOReturnSuccess;
    }

    IOReturn enableController() override {
        fOpened = true;
        nmx_step("fb: opened (enableController)");
        /* b164: WindowServer has opened us - our scan-out buffer replaces the firmware boot screen now (on a thread call:
           the mode set takes a few frames and must not run inside WindowServer's open) */
        if (fGoCall && !fHold) { fGoWhy = "WindowServer opened the framebuffer"; thread_call_cancel(fGoCall); thread_call_enter(fGoCall); }
        /* b103: readiness signal for the Phase 3 auto-arm helper - WindowServer has opened this framebuffer */
        setProperty("NMXFBOpened", kOSBooleanTrue);
        /* b126: boot mode sets are done - per-commit breadcrumbs off (they cost an NVRAM write each) */
        { if (!PE_parse_boot_argn("-nmxsteps", nullptr, 0)) nmx_steps_quiet = 1; }
        if (fPmWanted && fPmCall) {
            fPmWanted = false;
            uint64_t pd; clock_interval_to_deadline(2000, kMillisecondScale, &pd);
            thread_call_enter_delayed(fPmCall, pd);
            nmx_step("fb: power registration in 2 s");
        }
        return kIOReturnSuccess;
    }
    bool isConsoleDevice() override { return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fInfo.phys, (IOPhysicalLength)fInfo.pitch * fInfo.height);   /* every mode: same buffer, same pitch */
    }
    IODeviceMemory *getVRAMRange() override { return IODeviceMemory::withRange(fInfo.phys, fInfo.size); }

    /* b75 (WindowServer acceleration, boot-arg nmxws=1 on NMXAccel): NMXAccel's display pipe asks its head for the
       scan-out (NVRM_VRAM_FN_SCANOUT, "nvScanoutInfo", ABI version 2) and copies each composited frame into it - the
       copy path NMIntelFB answers the same way. phys 0: this is not NVIDIA VRAM, so the accelerator never maps it as
       such; the frame reaches the panel only through kva. Width/height are the current desktop size (panel-fitter modes
       share the buffer and its pitch). */
    struct NmxScanoutReq {   /* = struct NVRMVramRequest, kexts/NVRM/nvrm_vram_abi.h */
        unsigned int version, flags; unsigned long long size, actualSize, phys; void *kva, *handle;
        unsigned long long mappedTotal; unsigned int width, height, pitch; unsigned long long allocationCookie;
    };
    IOMemoryMap *volatile fScanMap = nullptr;
    /* b128: asynchronous page flip */
    thread_call_t fFlipCall = nullptr;
    IOLock *fFlipLock = nullptr;
    volatile bool fFlipBusy = false;
    volatile SInt32 fFlipBusyMiss = 0, fFlipErrs = 0;
    bool flipSetup() {
        if (fFlipCall) return true;
        if (!fFlipLock) fFlipLock = IOLockAlloc();
        if (!fFlipLock) return false;
        thread_call_t c = thread_call_allocate(flipFire, this);
        if (!c) return false;
        if (!OSCompareAndSwapPtr(nullptr, c, (void *volatile *)&fFlipCall)) thread_call_free(c);
        return true;
    }
    bool flipIdle(uint32_t ms) {      /* true = no flip in flight */
        uint64_t dl; clock_interval_to_deadline(ms, kMillisecondScale, &dl);
        IOLockLock(fFlipLock);
        while (fFlipBusy) {
            if (IOLockSleepDeadline(fFlipLock, (void *)&fFlipBusy, *(AbsoluteTime *)&dl, 0 /* THREAD_UNINT */) == 1 /* THREAD_TIMED_OUT */) break;
        }
        const bool idle = !fFlipBusy;
        IOLockUnlock(fFlipLock);
        return idle;
    }
    static void flipFire(thread_call_param_t p0, thread_call_param_t) {
        NMXGPUFB *me = (NMXGPUFB *)p0;
        const int err = me->fStopping ? 0 : nmx_fb_flip_commit(me->fDev);
        if (err && OSIncrementAtomic(&me->fFlipErrs) < 8) FBLOG("flip: commit refused (%d)", err);
        IOLockLock(me->fFlipLock); me->fFlipBusy = false; IOLockWakeup(me->fFlipLock, (void *)&me->fFlipBusy, false); IOLockUnlock(me->fFlipLock);
    }
    unsigned fScanLogs = 0;
    volatile SInt64 fVramBytes = 0;
    unsigned fVramLogs = 0;
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        /* b125 (R2): double-buffered scan-out for NMXAccel's display pipe. op 0: the buffer not on the panel (kva,
           pitch, desktop w x h); op 1: flip to it at the next vblank (blocks until then); op 2: back to buffer 0. */
        if (fn && fn->isEqualTo("nmxFlip")) {
            struct NmxFlipReq { uint32_t version, op; void *kva; uint32_t pitch, w, h; int32_t err; };
            NmxFlipReq *r = (NmxFlipReq *)p1;
            if (!r || r->version != 1u || !fDev) return kIOReturnBadArgument;
            /* b128: the flip runs on fFlipCall (blocking commit = known good, b125); op 0 waits up to 40 ms for the
               previous flip to land, op 1 only starts it, op 2 waits up to 100 ms then goes home synchronously */
            if (!flipSetup()) return kIOReturnNoMemory;
            if (r->op == 0) {
                if (!flipIdle(40)) { r->err = 16 /* EBUSY */; OSIncrementAtomic(&fFlipBusyMiss); return kIOReturnBusy; }
                r->err = nmx_fb_flip_back(fDev, &r->kva, &r->pitch, &r->w, &r->h);
            } else if (r->op == 1) {
                IOLockLock(fFlipLock); fFlipBusy = true; IOLockUnlock(fFlipLock);
                thread_call_enter(fFlipCall);
                r->err = 0;
            } else if (r->op == 2) {
                (void)flipIdle(100);
                r->err = nmx_fb_flip_home(fDev);
            } else return kIOReturnBadArgument;
            return r->err ? kIOReturnError : kIOReturnSuccess;
        }
        if (fn && fn->isEqualTo("nvScanoutInfo")) {
            NmxScanoutReq *r = (NmxScanoutReq *)p1;
            if (!r || r->version != 2u) return kIOReturnBadArgument;
            if (!fScanMap) {
                IODeviceMemory *m = IODeviceMemory::withRange(fInfo.phys, (IOPhysicalLength)fInfo.pitch * fInfo.height);
                IOMemoryMap *mm = m ? m->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
                if (m) m->release();
                if (!mm) { FBLOG("scanout: the framebuffer would not map for the accelerator's copy"); return kIOReturnNoMemory; }
                if (!OSCompareAndSwapPtr(nullptr, mm, (void *volatile *)&fScanMap)) mm->release();
                else FBLOG("scanout: framebuffer mapped for the accelerator (%ux%u pitch %u, %llu bytes)", fInfo.width, fInfo.height,
                           fInfo.pitch, (unsigned long long)fInfo.pitch * fInfo.height);
            }
            const unsigned i = modeOK(fCurMode) ? (unsigned)fCurMode - 1 : 0;
            r->kva = (void *)fScanMap->getVirtualAddress();
            /* b87: the scan-out's real physical address (the GPU aperture, contiguous) and size. With phys 0 (copied from
               NMIntelFB) NMXAccel's display pipe could never back the framebuffer resource WindowServer asks for
               (initFramebufferResource -> NULL even with the scan-out gate open), and WindowServer never sent a frame
               (10-19: iop transaction hooks OPEN, zero transactions in three armed runs). */
            {
                const unsigned long long need = (unsigned long long)fInfo.pitch * fInfo.height;
                r->phys = fInfo.phys;
                r->actualSize = (fInfo.size >= need) ? fInfo.size : ((need + 0xfffull) & ~0xfffull);
            }
            r->width = fModeW[i]; r->height = fModeH[i]; r->pitch = fInfo.pitch;
            if (fScanLogs < 4) { fScanLogs++; FBLOG("scanout: accelerator asked - %ux%u pitch %u (mode %d) phys 0x%llx %llu bytes", r->width, r->height, r->pitch, (int)fCurMode, r->phys, r->actualSize); }
            return kIOReturnSuccess;
        }
        /* b88: memory for NMXAccel's video-memory requests ("nvAllocVram"/"nvFreeVram", the NVRMFB ABI). WindowServer's
           compositor asks for 16 MB buffers (2560x1600x4) through it; NMXGPUFB refused them (0xe00002c7), the resources
           came back with no memory and WindowServer wrote its first frame to address 0 and crashed every 10 s (10-19,
           EXC_BAD_ACCESS at 0 in CompositorSW layer_blit_byte, row 10240 bytes). An Intel GPU has no VRAM: the
           buffers are ordinary system memory (b90, see below). */
        if (fn && (fn->isEqualTo("nvAllocVram") || fn->isEqualTo("nvFreeVram"))) {
            NmxScanoutReq *r = (NmxScanoutReq *)p1;
            if (!r || r->version != 2u) return kIOReturnBadArgument;
            if (fn->isEqualTo("nvFreeVram")) {
                IOBufferMemoryDescriptor *b = (IOBufferMemoryDescriptor *)r->handle;
                if (b) { OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes); b->complete(); b->release(); }
                return kIOReturnSuccess;
            }
            r->phys = 0; r->kva = nullptr; r->handle = nullptr; r->actualSize = 0; r->mappedTotal = 0; r->flags = 0;
            const unsigned long long want = (r->size + 0xFFFFull) & ~0xFFFFull;
            if (!want || want > (256ull << 20)) return kIOReturnBadArgument;
            if (fVramBytes + (SInt64)want > (SInt64)(1024ull << 20)) {
                if (fVramLogs < 20) { fVramLogs++; FBLOG("vram: %llu bytes REFUSED - 1 GB budget spent (%lld in use)", want, (long long)fVramBytes); }
                return kIOReturnNoMemory;
            }
            /* b90: ordinary cached system memory, any number of pieces (NVRM_VRAM_FLAG_MD). b89 used the GPU aperture:
               every frame WindowServer composited there was then READ back through that uncached window by the copy to
               the screen - 1.26 s per 1920x1200 frame (10-19 log: swap at :48.159, "COPIED" at :49.418) - and the GPU
               reading the aperture pages as an imported user pointer ended in VK_ERROR_DEVICE_LOST (queue submit -4). */
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, want, 4096);
            if (!b || b->prepare() != kIOReturnSuccess) {
                if (b) b->release();
                if (fVramLogs < 20) { fVramLogs++; FBLOG("vram: no %llu bytes of system memory", want); }
                return kIOReturnNoMemory;
            }
            bzero(b->getBytesNoCopy(), (size_t)want);
            IOByteCount seg = 0;
            const addr64_t ph = b->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
            if (!ph) { b->complete(); b->release(); return kIOReturnNoMemory; }
            OSAddAtomic64((SInt64)want, &fVramBytes);
            r->handle = b; r->kva = b->getBytesNoCopy(); r->phys = ph; r->actualSize = want; r->flags = 1u /* NVRM_VRAM_FLAG_MD */;
            r->mappedTotal = (unsigned long long)fVramBytes; r->allocationCookie = 0;
            if (fVramLogs < 20) { fVramLogs++; FBLOG("vram: %llu bytes of system memory (first page 0x%llx, %lld in use)", want, (unsigned long long)ph, (long long)fVramBytes); }
            return kIOReturnSuccess;
        }
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
    /* b94: the pointer image, converted by IOFramebuffer to straight-alpha ARGB rows of ci.cursorWidth pixels, stored
       premultiplied (the cursor plane blends premultiplied ARGB8888, as NVRMFB's did) into the image not on the plane */
    IOReturn setCursorImage(void *cursorImage) override {
        if (!fHwCursor) return kIOReturnUnsupported;
        IOHardwareCursorDescriptor d; bzero(&d, sizeof d);
        d.majorVersion = kHardwareCursorDescriptorMajorVersion;
        d.minorVersion = kHardwareCursorDescriptorMinorVersion;
        d.width = fCurDim; d.height = fCurDim;
        d.bitDepth = 32;
        d.supportedSpecialEncodings = kTransparentEncodedPixel;
        IOHardwareCursorInfo ci; bzero(&ci, sizeof ci);
        ci.majorVersion = kHardwareCursorInfoMajorVersion;
        ci.minorVersion = kHardwareCursorInfoMinorVersion;
        IOLockLock(fCurLock);
        bzero(fCurStage, fCurStageBytes);
        ci.hardwareCursorData = (UInt8 *)fCurStage;
        bool ok = convertCursorImage(cursorImage, &d, &ci);
        const unsigned w = ci.cursorWidth, h = ci.cursorHeight;
        if (!ok || !w || !h || w > fCurDim || h > fCurDim) {
            IOLockUnlock(fCurLock);
            if (fCurImgLogs < 6) { fCurImgLogs++; FBLOG("cursor: image not converted (%d, %ux%u) - macOS draws this one", (int)ok, w, h); }
            return kIOReturnUnsupported;
        }
        /* b95: always image 0, rewritten in place. b94 alternated two images, and every switch was a new cursor
           framebuffer = a full atomic commit on i915 (~15 ms, "commit tail" in the log) - the small lag when the pointer
           changed shape or came back, and the busy (animated) pointer committed every frame. The display engine reads the
           image memory each frame, so writing it in place changes the shape with no commit at all. */
        const int slot = 0;
        volatile uint32_t *dst = fCurKva[slot];
        for (unsigned y = 0; y < fCurDim; y++) {
            volatile uint32_t *row = dst + (size_t)y * fCurDim;
            for (unsigned x = 0; x < fCurDim; x++) {
                uint32_t v = 0;
                if (y < h && x < w) {
                    const uint32_t p = fCurStage[y * w + x], a = p >> 24;
                    if (a) {
                        const uint32_t r = (((p >> 16) & 255) * a + 127) / 255, g = (((p >> 8) & 255) * a + 127) / 255,
                                       b = ((p & 255) * a + 127) / 255;
                        v = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
                row[x] = v;
            }
        }
        __asm__ __volatile__("sfence" ::: "memory");
        fCurWant = slot;
        IOLockUnlock(fCurLock);
        if (fCurImgLogs < 6) { fCurImgLogs++; FBLOG("cursor: image %ux%u hot %u,%u -> buffer %d", w, h, (unsigned)ci.cursorHotSpotX, (unsigned)ci.cursorHotSpotY, slot); }
        thread_call_enter(fCurCall);
        return kIOReturnSuccess;
    }
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override {
        if (!fHwCursor) return kIOReturnUnsupported;
        IOLockLock(fCurLock);
        fCurX = x; fCurY = y; fCurVis = visible;
        IOLockUnlock(fCurLock);
        thread_call_enter(fCurCall);
        return kIOReturnSuccess;
    }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return fModeCount; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override {
        for (uint32_t i = 0; i < fModeCount; i++) modes[i] = (IODisplayModeID)(i + 1);
        return kIOReturnSuccess;
    }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (!modeOK(mode) || !info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->nominalWidth = fModeW[mode - 1];
        info->nominalHeight = fModeH[mode - 1];
        info->refreshRate = fRefresh16;
        info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
        if (mode == 1) info->flags |= kDisplayModeDefaultFlag | kDisplayModeNativeFlag;
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *pi) override {
        if (!modeOK(mode) || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fInfo.pitch;
        pi->bitsPerPixel = 32;
        pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3;
        pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000;
        pi->componentMasks[1] = 0x0000FF00;
        pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fModeW[mode - 1];
        pi->activeHeight = fModeH[mode - 1];
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
        *mode = fCurMode; *depth = 0; return kIOReturnSuccess;
    }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    /* without a timing, WindowServer shows the display at 0.00 Hz (NMIntelFB on studio, 10-10) */
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (!modeOK(mode) || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        if (mode != 1) {   /* a scaled size: the panel's refresh, with the smaller active area */
            info->flags = kIODetailedTimingValid;
            IODetailedTimingInformationV2 &s = info->detailedInfo.v2;
            s.pixelClock = s.minPixelClock = s.maxPixelClock = (uint64_t)fInfo.pclk_khz * 1000;
            s.horizontalActive = fModeW[mode - 1]; s.verticalActive = fModeH[mode - 1];
            s.horizontalBlanking = fInfo.htotal > fInfo.width ? fInfo.htotal - fModeW[mode - 1] : 0;
            s.verticalBlanking = fInfo.vtotal > fInfo.height ? fInfo.vtotal - fModeH[mode - 1] : 0;
            s.horizontalScaled = fInfo.width; s.verticalScaled = fInfo.height;
            return kIOReturnSuccess;
        }
        const nmx_fb_info &t = fInfo;
        if (!t.pclk_khz || t.htotal <= t.width || t.vtotal <= t.height) return kIOReturnSuccess;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = (uint64_t)t.pclk_khz * 1000;
        d.horizontalActive = t.width;
        d.horizontalBlanking = t.htotal - t.width;
        d.horizontalSyncOffset = t.hsync_start > t.width ? t.hsync_start - t.width : 0;
        d.horizontalSyncPulseWidth = t.hsync_end > t.hsync_start ? t.hsync_end - t.hsync_start : 0;
        d.verticalActive = t.height;
        d.verticalBlanking = t.vtotal - t.height;
        d.verticalSyncOffset = t.vsync_start > t.height ? t.vsync_start - t.height : 0;
        d.verticalSyncPulseWidth = t.vsync_end > t.vsync_start ? t.vsync_end - t.vsync_start : 0;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
        if (!modeOK(mode) || depth != 0) return kIOReturnUnsupported;
        if (mode == fCurMode) return kIOReturnSuccess;
        int err = nmx_fb_set_size(fDev, mode == 1 ? 0 : fModeW[mode - 1], mode == 1 ? 0 : fModeH[mode - 1]);
        FBLOG("display mode %d (%ux%u) -> %d", (int)mode, fModeW[mode - 1], fModeH[mode - 1], err);
        if (err) return kIOReturnUnsupportedMode;
        fCurMode = mode;
        return kIOReturnSuccess;
    }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref,
                                      void **iref) override {
        const char t[5] = { (char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, 0 };
        if (type == kIOFBVBLInterruptType) {
            for (unsigned i = 0; i < kVblMax; i++)
                if (!fVblX[i].proc) {
                    fVblX[i] = { proc, target, ref, true };
                    if (iref) *iref = &fVblX[i];
                    publishVblCount();
                    if (fIrqLogs < 24) { fIrqLogs++; FBLOG("interrupt: '%s' VBL client %u registered (proc %p, target %s)", t, i, proc,
                                                           target ? target->getMetaClass()->getClassName() : "-"); }
                    return kIOReturnSuccess;
                }
            FBLOG("interrupt: '%s' VBL - all %u slots taken, REFUSED", t, (unsigned)kVblMax);
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref, true }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        if (fIrqLogs < 24) { fIrqLogs++; FBLOG("interrupt: '%s' REFUSED (unsupported type)", t); }
        return kIOReturnUnsupported;
    }
    /* b103: ioreg NMXFBVBLClients - how many VBL clients are registered (1 = IOFramebuffer for WindowServer, 2 = also the
       accelerator's display pipe once armed); the Phase 3 auto-arm helper waits for at least 1 */
    void publishVblCount() {
        unsigned n = 0;
        for (unsigned i = 0; i < kVblMax; i++) if (fVblX[i].proc) n++;
        setProperty("NMXFBVBLClients", (unsigned long long)n, 32);
    }
    IOReturn unregisterInterrupt(void *iref) override {
        for (unsigned i = 0; i < kVblMax; i++)
            if (iref == &fVblX[i]) {
                if (fIrqLogs < 24) { fIrqLogs++; FBLOG("interrupt: VBL client %u unregistered", i); }
                fVblX[i] = Irq(); fVblX[i].proc = nullptr; publishVblCount(); return kIOReturnSuccess;
            }
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *iref, UInt32 state) override {
        for (unsigned i = 0; i < kVblMax; i++)
            if (iref == &fVblX[i]) {
                if (fVblX[i].on != (state != 0) && fIrqLogs < 24) { fIrqLogs++; FBLOG("interrupt: VBL client %u %s", i, state ? "on" : "off"); }
                fVblX[i].on = state != 0; return kIOReturnSuccess;
            }
        return kIOReturnSuccess;
    }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    /* power acknowledged at once; left to IOFramebuffer it waits on a shared thread (NVRMFB, studio 10-10) */
    /* b131 (sleep): with NVRAM nmx-fbpm=1 the IOFramebuffer base class runs its own power protocol once WindowServer has
       opened us - it tells WindowServer the display is going to sleep and waits for its acknowledgement. Without it
       (10-22) WindowServer logs "Display sleep not yet completed, defer ack of IOPM sleep", the system sleep waits 30 s
       for WindowServer and any touch in that time turns the sleep into a DarkWake. Read at every power change, so
       sudo nvram nmx-fbpm=1 / sudo nvram -d nmx-fbpm switch it without a restart. */
    static bool fbPmOn() {
        bool on = true;     /* b133: ON by default (10-22: sleep + wake work every time); NVRAM nmx-fbpm=0 turns it off */
        IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
        if (IODTNVRAM *n = OSDynamicCast(IODTNVRAM, e)) {
            OSObject *o = n->copyProperty("nmx-fbpm");
            if (OSData *d = OSDynamicCast(OSData, o)) on = !(d->getLength() >= 1 && *(const char *)d->getBytesNoCopy() == '0');
            OSSafeReleaseNULL(o);
        }
        if (e) e->release();
        return on;
    }
    IOReturn setPowerState(unsigned long ordinal, IOService *dev) override {
        if (!fOpened) { nmx_step("fb: setPowerState before open, acked"); return kIOPMAckImplied; }
        if (fbPmOn()) {
            FBLOG("power: setPowerState %lu -> IOFramebuffer (nmx-fbpm=1)", ordinal);
            IOReturn r = IOFramebuffer::setPowerState(ordinal, dev);
            FBLOG("power: setPowerState %lu returned 0x%x", ordinal, (unsigned)r);
            return r;
        }
        nmx_step(ordinal ? "fb: setPowerState on, notifying" : "fb: setPowerState off, notifying");
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        nmx_step("fb: setPowerState done");
        return kIOPMAckImplied;
    }
    /* IOFramebuffer's versions take its controller lock, which is NULL until the framebuffer is opened: the option-1 boot
       (10-15) page-faulted at NULL+0x120 in IOFramebuffer::powerStateWillChangeTo right after the deferred registration.
       Our own setPowerState/setAttribute already handle power, so acknowledge here without touching the base class. */
    IOReturn powerStateWillChangeTo(IOPMPowerFlags f, unsigned long ordinal, IOService *dev) override {
        if (fOpened && fbPmOn()) {
            FBLOG("power: will change to %lu -> IOFramebuffer (nmx-fbpm=1)", ordinal);
            IOReturn r = IOFramebuffer::powerStateWillChangeTo(f, ordinal, dev);
            FBLOG("power: will change to %lu returned 0x%x", ordinal, (unsigned)r);
            return r;
        }
        nmx_step(ordinal ? "fb: power will change (on), acked" : "fb: power will change (off), acked");
        return kIOPMAckImplied;
    }
    IOReturn powerStateDidChangeTo(IOPMPowerFlags f, unsigned long ordinal, IOService *dev) override {
        if (fOpened && fbPmOn()) {
            FBLOG("power: did change to %lu -> IOFramebuffer (nmx-fbpm=1)", ordinal);
            IOReturn r = IOFramebuffer::powerStateDidChangeTo(f, ordinal, dev);
            FBLOG("power: did change to %lu returned 0x%x", ordinal, (unsigned)r);
            return r;
        }
        nmx_step(ordinal ? "fb: power did change (on), acked" : "fb: power did change (off), acked");
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            /* display sleep turns the outputs off; wake lights them again with the same buffer */
            nmx_step(value ? "fb: power attribute on, DPMS on" : "fb: power attribute off, DPMS off");
            if (!value && fBacklight) fBacklight->setPowered(false);
            if (fDev) nmx_fb_power(fDev, value != 0);
            nmx_step("fb: DPMS returned");
            if (value && fBacklight) fBacklight->setPowered(true);   /* i915 relights at its own level: ours again */
            if (!fOpened) return kIOReturnSuccess;
            if (value) {
                IOReturn r = IOFramebuffer::setAttribute(attribute, value);
                nmx_step("fb: base power attribute returned, notifying");
                handleEvent(kIOFBNotifyDidPowerOn);
                nmx_step("fb: power-on notified");
                return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
            }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag | kDisplayModeNativeFlag;
        return kIOReturnSuccess;
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        if (attribute == kIOHardwareCursorAttribute) { if (value) *value = fHwCursor ? 1 : 0; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0 || !value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionFlags: *value = fBuiltIn ? kIOConnectionBuiltIn : 0; return kIOReturnSuccess;   /* b134 */
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
};
OSDefineMetaClassAndStructors(NMXGPUFB, IOFramebuffer)

/*
 * b117: CoreDisplay decides the GPU vendor from the framebuffer's CLASS NAME (GetSystemGPUVendors: IOObjectCopyClass on
 * every IOFramebuffer, case-insensitive search for a vendor word - Apple's own is AppleIntelFramebuffer). "NMXGPUFB"
 * matches no vendor, so gpuVendors = 0, CoreDisplay::useIOP is cleared and WindowServer never uses the IOPresentment /
 * Metal compositor path (nmx-cd2..6 traces, 10-21). With boot-arg nmxintelfb=1 the framebuffer is created as this
 * subclass: same code, a class name that contains the Intel vendor word. The registry NAME stays "NMXGPUFB" and the
 * class still conforms to NMXGPUFB, so ioreg -c NMXGPUFB, serviceMatching and metaCast keep working.
 */
class NMXAppleIntelFramebuffer : public NMXGPUFB {
    OSDeclareDefaultStructors(NMXAppleIntelFramebuffer)
};
OSDefineMetaClassAndStructors(NMXAppleIntelFramebuffer, NMXGPUFB)

static bool nmxIntelFBName()
{
    uint32_t v = 0;
    return PE_parse_boot_argn("nmxintelfb", &v, sizeof v) && v != 0;
}

/*
 * Boot-screen snapshot (10-18 black-screen fix). Until WindowServer opens us the panel shows our scan-out buffer, which
 * the bridge used to clear to black, while the kernel console kept drawing into the firmware's framebuffer that nobody
 * scans out any more. Now the firmware screen (the verbose text) is copied once before i915 takes the GPU, and the
 * bridge paints that copy into the scan-out buffer instead of black, so the last boot text stays on screen.
 * Only reads: getConsoleInfo() (no setConsoleInfo - that hung build 53). -nmxnosnap turns it off.
 */
static uint8_t *gSnap;
static uint32_t gSnapW, gSnapH, gSnapPitch;
static vm_size_t gSnapSize;

/* b150: b149 read the firmware screen from the GPU's stolen memory with the CPU: a MACHINE CHECK at boot (10-23 12:08 and
   12:13 panics, IA32_MC11_ADDR = 0x4c800000 = the stolen base). Tiger Lake blocks CPU access to stolen memory. Removed;
   back to b148 (snapshot off unless -nmxsnap, aperture copy). Do not read stolen memory with the CPU. */
extern "C" void nmx_boot_snapshot(void)
{
    /* b78: OFF by default. The copy reads 16 MB through the GPU aperture with uncached 8-byte loads: 9-13 s on most
       boots of the Tiger Lake laptop (10-19 log), and the 08:26 boot hung inside it (the Mac stopped at the text
       console with the last step "snapshot: firmware console"). Without it the screen is black for a few seconds
       between the takeover and the login window. Boot-arg -nmxsnap turns it back on. */
    if (!PE_parse_boot_argn("-nmxsnap", nullptr, 0) || PE_parse_boot_argn("-nmxnosnap", nullptr, 0)) {
        FBLOG("snapshot: off (default since b78 - it slowed or hung the boot; -nmxsnap turns it on)"); return; }
    PE_Video v;
    bzero(&v, sizeof v);
    IOPlatformExpert *pe = IOService::getPlatform();
    if (pe == nullptr) return;
    pe->getConsoleInfo(&v);
    FBLOG("snapshot: firmware console %lux%lu rowbytes %lu depth %lu base 0x%lx", v.v_width, v.v_height,
          v.v_rowBytes, v.v_depth, v.v_baseAddr);
    if (v.v_baseAddr == 0 || v.v_depth != 32 || v.v_width == 0 || v.v_height == 0 || v.v_width > 8192 ||
        v.v_height > 8192 || v.v_rowBytes < v.v_width * 4) { FBLOG("snapshot: unusable console, skipped"); return; }
    vm_size_t size = (vm_size_t)v.v_rowBytes * v.v_height;
    /* 10-18: the base came back as 0x4000000001 - the low bit is a flag, not part of the address; reading from the odd
       address shifted every pixel by one byte (colours rotated). Pixels are 4-byte aligned, so drop the low bits. */
    IOPhysicalAddress base = (IOPhysicalAddress)(v.v_baseAddr & ~(unsigned long)3);
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(base, size, kIODirectionIn);
    if (md == nullptr) return;
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache);
    if (map == nullptr) { md->release(); FBLOG("snapshot: firmware framebuffer would not map"); return; }
    uint8_t *buf = (uint8_t *)IOMalloc(size);
    if (buf != nullptr) {
        const uint64_t *src = (const uint64_t *)map->getVirtualAddress();
        uint64_t *dst = (uint64_t *)buf;
        for (vm_size_t i = 0; i < size / 8; i++) dst[i] = src[i];
        gSnap = buf; gSnapSize = size;
        gSnapW = (uint32_t)v.v_width; gSnapH = (uint32_t)v.v_height; gSnapPitch = (uint32_t)v.v_rowBytes;
    }
    map->release();
    md->release();
    nmx_step(gSnap ? "snapshot: firmware screen copied" : "snapshot: no memory");
}

/* called by the bridge in place of clearing: paints the snapshot centred on black; 0 when there is none */
extern "C" int nmx_boot_snapshot_paint(void *va, uint32_t pitch, uint32_t width, uint32_t height)
{
    uint8_t *d = (uint8_t *)va;
    if (gSnap == nullptr) return 0;
    uint32_t w = gSnapW < width ? gSnapW : width, h = gSnapH < height ? gSnapH : height;
    uint32_t ox = (width - w) / 2, oy = (height - h) / 2, sx = (gSnapW - w) / 2, sy = (gSnapH - h) / 2;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(d + (uint64_t)y * pitch);
        if (y < oy || y >= oy + h) { bzero(row, (size_t)width * 4); continue; }
        if (ox) { bzero(row, (size_t)ox * 4); bzero(row + ox + w, (size_t)(width - ox - w) * 4); }
        memcpy(row + ox, gSnap + (uint64_t)(sy + y - oy) * gSnapPitch + (uint64_t)sx * 4, (size_t)w * 4);
    }
    IOFree(gSnap, gSnapSize);
    gSnap = nullptr;
    nmx_step("snapshot: painted into the scan-out buffer");
    return 1;
}

/* lights the device's outputs and publishes them under 'gpu'; nullptr when there is nothing to show (reason logged) */
IOService *nmx_fb_publish(IOService *gpu, void *dev)
{
    nmx_fb_info info;
    /* b164: keep the firmware boot screen until WindowServer opens us; -nmxnodefer = the old immediate mode set */
    nmx_fb_defer_wanted = PE_parse_boot_argn("-nmxnodefer", nullptr, 0) ? 0 : 1;
    /* b170: panel self refresh once accelerated frames flip; -nmxnopsr keeps it off */
    nmx_fb_psr_wanted = PE_parse_boot_argn("-nmxnopsr", nullptr, 0) ? 0 : 1;
    /* b180: external displays (HDMI / USB-C) mirror the panel; -nmxnoext keeps them dark */
    nmx_fb_ext_wanted = PE_parse_boot_argn("-nmxnoext", nullptr, 0) ? 0 : 1;
    int err = nmx_fb_open(dev, &info);
    nmx_step(err || !info.ok ? "publish: framebuffer open FAILED" : "publish: framebuffer open ok");
    if (err || !info.ok) {
        FBLOG("no framebuffer (%d)", err);
        return nullptr;
    }
    const bool intelName = nmxIntelFBName();
    NMXGPUFB *fb = intelName ? (NMXGPUFB *)OSTypeAlloc(NMXAppleIntelFramebuffer) : OSTypeAlloc(NMXGPUFB);
    if (fb == nullptr || !fb->initWith(dev, info)) {
        OSSafeReleaseNULL(fb);
        nmx_fb_close(dev);
        return nullptr;
    }
    if (intelName) {   /* after init: the name lives in the property table init() creates */
        fb->setName("NMXGPUFB");
        FBLOG("b117: framebuffer class NMXAppleIntelFramebuffer (nmxintelfb=1) - CoreDisplay sees an Intel GPU");
    }
    if (!fb->attach(gpu)) {
        OSSafeReleaseNULL(fb);
        nmx_fb_close(dev);
        return nullptr;
    }
    nmx_step("publish: starting NMXGPUFB");
    if (!fb->start(gpu)) {
        fb->detach(gpu);
        fb->release();
        nmx_fb_close(dev);
        return nullptr;
    }
    nmx_step("publish: NMXGPUFB started, registering");
    fb->registerService();
    nmx_step("publish: NMXGPUFB registered");
    /* 10-18: the kernel console (verbose text, then the Apple logo and progress bar) still pointed at the firmware's
       framebuffer, which the panel no longer scans out once i915 has set its own mode - so the screen stayed black
       from the mode set until WindowServer opened us (~18 s). Point the console at our scan-out buffer instead, as
       IOFramebuffer does for the framebuffer that holds the boot console. -nmxnoconsole keeps the old behaviour. */
    /* 10-18: setConsoleInfo(kPEBaseAddressChange) hung the boot right after this point (last step "NMXGPUFB
       registered", then a restart) - opt-in only (-nmxconsole) until a safe way is found */
    if (PE_parse_boot_argn("-nmxconsole", nullptr, 0)) {
        PE_Video v;
        bzero(&v, sizeof v);
        IOPlatformExpert *pe = IOService::getPlatform();
        if (pe) pe->getConsoleInfo(&v);      /* a virtual call: PE_current_console() is not exported to kexts */
        FBLOG("console: was %lux%lu rowbytes %lu depth %lu base 0x%lx display %lu", v.v_width, v.v_height, v.v_rowBytes,
              v.v_depth, v.v_baseAddr, (unsigned long)v.v_display);
        v.v_baseAddr = (unsigned long)info.phys;
        v.v_rowBytes = info.pitch;
        v.v_width = info.width;
        v.v_height = info.height;
        v.v_depth = 32;
        if (pe) {
            int r = pe->setConsoleInfo(&v, kPEBaseAddressChange);
            FBLOG("console: moved to our scan-out buffer 0x%llx %ux%u (%d)", info.phys, info.width, info.height, r);
            nmx_step("publish: boot console moved to the new framebuffer");
        }
    }
    return fb;
}

void nmx_fb_unpublish(IOService *fbsvc, IOService *gpu, void *dev)
{
    if (fbsvc == nullptr)
        return;
    fbsvc->terminate(kIOServiceSynchronous);
    fbsvc->release();
    nmx_fb_close(dev);
}
