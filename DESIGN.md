# Iris Xe full acceleration - target design (2026-10-21)

Goal: WindowServer composites on the GPU (SkyLight CompositorMetal + CoreAnimation CA::OGL::MetalContext), frames reach the
panel by page flip (no CPU copy), apps render with Metal on the Iris Xe. NVIDIA code is a reference only, not the model.

## Why the current design stalls
- NMXAccel presents itself as a LEGACY accelerator (IOAccelLegacySurface swaps, IOAccelLegacyDisplayPipe/Machine - see the
  b112 panic trace). That is the old CGL-era path: WindowServer keeps CompositorSW + CA::OGL::SWContext, swaps legacy
  surfaces, and never starts a display-pipe transaction (iop_ok 0, tx 0/0 in every report).
- The memory model is NVIDIA's (VRAM requests "nvAllocVram", a scan-out to copy into). The Iris Xe has unified memory:
  the GPU, the display engine and the CPU can all use the same pages. Every copy in today's path is unnecessary.
- So armed mode pays GPU-mode costs (Metal translation, surface locks, buffer swaps, copy to panel) with CPU-mode drawing.

## Target architecture
1. Display - NMXGPU (i915 port): KEEP. Add a flip service for the accelerator:
   - import any accelerator buffer (its pages) as an i915 GEM object, pinned in the GGTT, with a drm_framebuffer
   - nonblocking atomic page flip of the primary plane at vblank, completion callback with the vblank timestamp
   - hardware cursor (done, b94/b95), panel fitter sizes (done)
2. Memory - one allocator for the accelerator, IOSurfaces and Vulkan:
   - accelerator resources = system memory the GPU maps (GEM/userptr through the DRM bridge), the same pages Mesa ANV
     imports (zero-copy host-pointer import already exists in the plugin) and the display engine flips to
   - cache coherency handled where it matters (display reads are not snooped: flush/clflushopt only for CPU-written frames)
3. Accelerator - NMXAccel: REDESIGN the display side on Apple's own Intel drivers, not NVIDIA's:
   - reference = AppleIntelICLGraphics.kext + AppleIntelFramebufferIceLake.kext + AppleIntelICLGraphicsMTLDriver.bundle,
     which still ship in Sequoia (same vendor, same unified memory, same display model). Their class hierarchy, ioreg
     properties and display-pipe classes are what WindowServer's Metal path expects on an Intel Mac.
   - modern (non-legacy) display pipe: IOPresentment transactions -> NMXGPU flip -> completion/vblank back to WindowServer
   - keep what works: user clients, command submission plumbing, stamps, the DRM bridge for ANV
4. Metal - NVMTLDriver (Metal -> Vulkan over Mesa ANV): KEEP (Apple never shipped Metal for Gen12; translation is the only
   way). Make the device answer like Apple's Intel Metal device (families, feature sets, limits, properties QuartzCore and
   SkyLight check before choosing their Metal renderers), and keep fixing correctness (framebuffer fetch b99/b114, blur).
5. WindowServer path when done: CA MetalContext renders -> CompositorMetal composites into display-pipe buffers ->
   IOPresentment transaction -> page flip at vblank. Zero CPU copies, CPU nearly idle while typing.

## Roadmap (each step has one success signal)
| Step | Work | Done when |
|---|---|---|
| R1 | Read Apple's Intel ICL kexts + Metal bundle (class names, properties, user-client/display-pipe classes) | a property/class map in this file |
| R2 | Flip service in NMXGPU (GEM import, GGTT pin, nonblocking flip, vblank completion) | a test buffer flips on the panel |
| R3 | Unified allocator: accelerator buffers are flippable GEM pages | WindowServer frames shown by flip, copy count 0 |
| R4 | Modern display pipe in NMXAccel (Intel-style, from R1) | debug.nvaccel_iop_ok grows |
| R5 | Metal device parity (from R1) | a WindowServer sample shows CompositorMetal / CA::OGL::MetalContext in render_update |
| R6 | Effects + polish: blur, moving wallpapers, lock screen, smoothness, upload batching for app icons | armed beats unarmed |

Until R4/R5 work, unarmed mode is faster for daily use; arm only to test.

## R1 findings (10-21, from the user's BootKernelExtensions.kc: AppleIntelICLGraphics, AppleIntelICLLPGraphicsFramebuffer,
## IOAcceleratorFamily2 - symbols present; extractor: scratch tools/kcx.py, fileset entries rebuilt as standalone Mach-O)
- Apple's Intel accelerator uses the MODERN family classes, NMXAccel uses the LEGACY ones (NVIDIA style):
  | Apple Intel (ICL)                      | superclass (from the metaclass ctors) | NMXAccel today             |
  |---|---|---|
  | IntelAccelerator                       | IOGraphicsAccelerator2                | NMXAccel (same base)       |
  | IGAccelSurface (0x1338 B)              | IOAccelSurface                        | NVSurface : IOAccelLegacySurface |
  | IGAccelDisplayMachine (0x198 B)        | IOAccelDisplayMachine                 | IOAccelLegacyDisplayMachine |
  | IGAccelDisplayPipe (0x64c8 B)          | IOAccelDisplayPipe                    | NVDisplayPipe : IOAccelLegacyDisplayPipe |
  The family checks this itself ("Expecting IOAccelLegacyDisplayMachine but got IOAccelDisplayMachine").
- The Legacy pipe is the old scanout/full-screen flip model (setupScanout, submitScanoutFlipBuffer, getScanoutSurface...).
  The modern Intel pipe is TRANSACTION based and flips the planes itself: IGAccelDisplayPipe overrides beginTransaction,
  validateTransaction, performTransaction, submitTransaction, isTransactionComplete, signalTransactionComplete,
  DecodeTransaction, generateFlip(event, plane, res, res), getScanoutOffset, planeSurfOffset(Live), initFramebufferResource,
  destroyFramebufferResource, displayModeWill/DidChange, probeDisplayPipe, allocate/deallocateScanoutMemory, scaled modes,
  gamma/PQ transactions. IntelAccelerator::newDisplayMachine/newDisplayPipe create them.
- IGAccelSurface still implements submitSwapCopy/copyFromBuffer/copyToBuffer/copyBufferDMA/shapeSurface/setBuffer/setData.
- WindowServer submits transactions through IOAccelDisplayPipeUserClient2 (transaction_begin/end/wait, gamma tables,
  request_notify); canUseDisplayPipe() is an entitlement check (WindowServer has it).
- Accelerator Info.plist: IOAccelDisplayPipeCapabilities {DisplayPipeSupported, TransactionsSupported} static in the
  personality, plus IOGLBundleName, IOCFPlugInTypes (IOAccelerator2D.plugin), IOVARendererID, GPURawCounter* names.
Consequence for R4: rebase NMXAccel's display side on IOAccelDisplayMachine / IOAccelDisplayPipe / IOAccelSurface and
implement the transaction + generateFlip set the way IGAccelDisplayPipe does, with the flip done by NMXGPU (i915).

## R4 status (10-21, b116 + nmxmodern=1, trace nmx-ws-props.sh v3)
- With the modern classes WindowServer now OPENS the display pipe: 3 x IOAccelDisplayPipeUserClient2 (accelerator user
  client type 4), setPipeIndex (0 twice, once 0xe00002c5 exclusive - a second client on the same pipe), getCapabilitiesData
  (0), requestNotify (0), 2 x clientClose. It never calls transaction_begin, never enables VBL interrupts (iop_ok 0).
  Before b116 (legacy classes) type 4 was never opened.
- Capabilities come from the accelerator property IOAccelDisplayPipeCapabilities (IOAccelDisplayPipe::copyCapabilities is
  accel->copyProperty), same keys as Apple's Intel personality.
- The family offers IOAccelDisplayPipe::signalVBLInterrupt / signalTransactionInterrupt for the vendor to call (Intel
  registers a VBL handler with its framebuffer: fSymFBCLASSRegisterVBLHandler). NMXDisplayPipeM calls neither yet.
- Likely remaining blocker: WindowServer still composites with CompositorSW (the CPU compositor) and that path presents
  with surface swaps, not transactions. Next: v4 trace of which SkyLight/QuartzCore/CoreDisplay compositor functions run.
- User report: armed with nmxmodern=1, typing / UI / System Settings icons are as smooth as unarmed (legacy-surface costs
  gone), occasional small black flashes (no copy-forward on modern surfaces).

## R5 blocker found (10-21, nmx-cd2.sh)
- CoreDisplay::UseIOPresentment() = useIOP && (vendorBits(useIOP_onIG<<1 | onAMD<<2 | onNVIDIA<<3 | onAPV<<4) & gpuVendors)
  == gpuVendors && gpuVendors != 0; otherwise it clears useIOP for good.
- Armed: useIOP_onIG/onAMD/onNVIDIA/onAPV all 1, but GetSystemGPUVendors::gpuVendors = 0 -> CoreDisplay sees NO GPU,
  useIOP cleared, WindowServer stays on CompositorSW. Next: read GetSystemGPUVendors (nmx-cd3.sh code dump) and make
  NMXGPU/NMXAccel publish what it looks for, so gpuVendors = 2 (Intel).
- 10-21 nmx-cd4..6: GetSystemGPUVendors = for each IOFramebuffer: IOObjectCopyClass(), CFStringFind (case-insensitive)
  for 4 vendor words -> 2 IG / 4 AMD / 16 APV / 8 NV. "NMXGPUFB" matches none. b117: boot-arg nmxintelfb=1 creates the
  framebuffer as subclass NMXAppleIntelFramebuffer (registry name kept "NMXGPUFB"). Success = useIOP stays 1,
  UseIOPresentment() returns 1, WindowServer compositor = CompositorMetal.
- 10-21 b117 + nmxintelfb=1 CONFIRMED: gpuVendors = 0x2 (Intel), useIOP = 1 -> CoreDisplay::UseIOPresentment() can say yes.
  Next signal: compositor chosen (CompositorMetal?), display-pipe transactions, iop_ok (nmx-gpucheck.sh).
- 10-21 b117 armed, nmx-gpucheck: UseIOPresentment YES, CA::OGL::MetalContext draws, CompositorMetal, display-pipe
  transactions complete (iop_ok 2047 -> 2051 in 10 s idle), WindowServer 4.7 % CPU. R4 + R5 success signals MET.
  User: no typing delay, no black boxes, System Settings icons fast. Open: 24 IOPresentmentErrorSetErrorCode / 10 s,
  WSGetCompositorForAccelerator still NONE, frames still reach the panel by copy (R2/R3 flip not done).
- 10-21 nmx-ioperr: the "IOPresentment errors" are IOPresentmentErrorSetErrorCode(0) = clearing the error slot on every
  call (success). No kernel display-pipe errors either (submitTransaction is void; getPlaneIOSurface returns pointers).
  Real remaining cost: nvpipe_performTransaction copies the whole IOSurface (2560x1600x4, 16 MB, volatile 32-bit loop)
  into the i915 scan-out on every transaction; nvAccelIopFlip is NVIDIA-only. -> R2: Intel flip (IOSurface pages ->
  i915 GEM/GGTT -> drm framebuffer -> plane flip at vblank, completion via signalTransactionInterrupt).
- b118 (R2 prep): frame copy = memcpy per row (was a volatile word loop), timed (debug.nvaccel_iop_copy_ns/_max_ns/
  _copies; nmxslowcopy=1 = old loop). nmx-iopstat.sh measures frames/s and copy cost during animation + K6 source log,
  which decides the R2 import route (IOSurface pages vs our nvAllocVram resources -> i915 GEM -> plane flip).
- 10-21 b118 nmx-iopstat (animating): 28 frames/s to the panel, copy 1.7 ms avg (6.5 ms max), 4 % CPU copying.
  Desktop 1920x1200 (panel fitter to 2560x1600). Frames = IOSurfaces 0x2/0x3 (double buffer), each backed by an
  NMXResource whose memory is OUR nvAllocVram allocation (9240576 B, IOBufferMemoryDescriptor, kva). -> R2 route:
  allocate those from i915 GEM and flip them. BUT WindowServer CPU 58 % while animating: the copy is not the
  bottleneck (28 fps). Profile WindowServer first (nmx-wsprof.sh).
- 10-21 nmx-wsprof (animating, b118): WindowServer user 7147 / kernel 2572 samples in 15 s (~65 % of a core).
  libvulkan_intel 2265 (anv_AllocateDescriptorSets 583 + its bzero, anv_descriptor_set_destroy 241, NIR pipeline
  compiles), libnvmtl_translate 1964 (runtime AIR->SPIR-V translation - first-use shaders?), malloc/free 961,
  kernel: i915 execbuf (eb_lookup_vmas, active tracking). Draws allocate 2 descriptor sets each and free them at retire.
- b119: descriptor-set recycling in NVMTLDriver (retired sets kept on a stack and reused; off with
  /Library/GPUBundles/nvmtl-norecycle). Next: confirm translation is first-use only (2nd Mission Control should hit
  the disk caches); then execbuf BO-list cost; then R2 flip.
- 10-21 b119 measured (animating, caches warm): 44 frames/s (was 28), WindowServer user samples 2193 (was 7147, -69 %),
  descriptor alloc/free + translation gone from the profile. Kernel now dominates: 2603 samples - i915 execbuf
  (eb_lookup_vmas 99, i915_active_add_request 108, userptr_submit_init, dma_resv), linuxkpi locking
  (ml_set_interrupts_enabled 317, IOLock 170, IORWLock 85, nmx_thread_get 59). Next: execbuf BO count/latency
  (nmx-execbuf.sh) - every host-pointer (userptr) BO with a device address is likely listed on every submit.
- 10-21 nmx-execbuf (b119, animating): WindowServer 2521 submissions / 15 s (168/s, ~4 per frame), 592 buffers listed in
  EVERY submission (ANV i915 lists all device memory objects - buffer device addresses), 0.5-2 ms kernel each,
  2.48 s of 15 s (16.5 % of a core) inside execbuf. Per buffer: handle lookup under RCU = critical_enter/exit, each
  looking up curthread in a hash under ONE global spin lock + interrupt toggles.
- b120 (NMXGPU): per-CPU curthread cache, type-stable thread records (free list), boot-arg -nmxnotcache = off.
  Later options: fewer memory objects in NVMTL (sub-allocate images/buffers from big blocks), fewer submissions per frame.
- 10-21 b120 nmx-execbuf: ~970 us per submission (b119 ~983 us) - the curthread cache did not move the per-submit
  cost; buffers per submission 544. Kernel cost is per-BO i915 work, not the thread lookup -> reduce BO count instead.
- User: Dock animations and blur still missing while armed (Metal compositor). Suspect the "no Quartz Extreme" path:
  CGDisplayUsesOpenGLAcceleration / CoreDisplay usesOpenGLAccel / isAcceleratorBacked false -> Dock/vibrancy fall back.
  nmx-qe.sh checks it.
- 10-21 nmx-bos (20 s, opening/closing apps): 676 GPU memory allocations + 684 frees in WindowServer (~34/s each).
  258 = host-pointer imports for Metal newTiledTextureWithBytesNoCopy stand-ins (CA copy_image_to_texture, 4-8 KB
  mostly), 147 IOSurfaces live; 280 plain image allocations from CA transient surfaces (blur ping-pong, backdrop
  capture, ImagingNode render surfaces, corner masks).
- b121 (NVMTLDriver): stand-ins <= 512 KB copied into pooled memory (no userptr BO; off: nvmtl-nostandincopy);
  image memory recycling (150 ms quarantine, 2 s max age, 32 entries / 96 MB; off: nvmtl-noimgrecycle).
- 10-21 b121 nmx-bos: 540 allocations / 512 frees in 20 s (was 676/684). Host imports 169 (was 396): the stand-in copy
  works, but the linear alias over the pooled copy is refused, so newTiledTextureWithDescriptor falls back to a NEW
  opaque texture each time (239 "plain" allocations from that path). Image recycling helps little (sizes vary).
  -> next: sub-allocate small opaque images (<= 1 MB) from 16 MB blocks (one BO per block). Desktop now 1680x1050.
- User: unlocking from the lock screen freezes ~10 s, then fine. nmx-unlock.sh profiles the whole system through it.
- 10-21 nmx-unlock: no freeze reproduced; system busy with post-login background work (trustd SHA-256 checks,
  softwareupdated, mediaanalysisd, XProtect) - the earlier 10 s was most likely the first unlock after installing.
- b122 (NVMTLDriver): colour images <= 1 MB sub-allocated from shared 16 MB blocks (4 KB units, alignment kept,
  150 ms quarantine, <= 16 blocks, image placed == 2; off: /Library/GPUBundles/nvmtl-noslab). Goal: fewer buffer
  objects per submission and no kernel create/destroy for small textures.
- 10-21 b122 measured: GPU memory allocations 74 / 20 s (was 540-676), buffers per submission 141 (was 544-592),
  kernel time per submission ~0.32 ms (was ~1 ms), total 0.76 s / 15 s (was 2.48 s), 47 frames/s animating,
  WindowServer 36 % CPU. No visual problems reported from the shared blocks.
- User: lock-screen password field showed 2-3 dots for 4 typed characters. Likely the last frame of a burst copied
  to the panel before the GPU finished it (copy path has no wait on the frame's GPU work), and nothing redraws after.
- b123 (NMXAccel): settle copy - after each frame, the same surface is copied again 40 ms and 150 ms later unless a
  newer frame arrived (generation check under a lock shared with the transaction copy). sysctl debug.nvaccel_iop_settle.
  Proper fix later: wait on the frame's GPU fence before copying / flip at vblank (R2).
- 10-21 b123: password dots still missing (3rd of 4) -> NOT the copy-before-GPU-done race. New suspect: b121
  stand-in COPY - CA glyph/image atlases created with newTiledTextureWithBytesNoCopy are updated in place by the
  caller later; a copy at creation misses later writes. Test: /Library/GPUBundles/nvmtl-nostandincopy.
- User: choosing the Sequoia or Macintosh dynamic wallpaper leaves the previous (Sequoia Sunrise aerial) on screen;
  others work; predates b123. nmx-wall.sh collects the wallpaper agent/extension logs and crash reports.
- 10-21 ROOT CAUSE (wallpapers, probably the missing password dot and the slow menu bar too): the NVMTLDriver.bundle
  from nmxgpu-intel-build.yml has NO Contents/MacOS/air-runtime/libLLVM.dylib (only mesa-anv-build.yml ships it).
  Since b119 the whole bundle was replaced each time -> shader reader gone; air-opt fallback dies (wait status 6, it
  needs the same libLLVM). Only shaders already in the caches work; every NEW shader fails ("translation produced
  nothing"). Install plugin updates with tools/macos-helpers/nmx-plugin-update.sh (binary only, keeps air-runtime).
- 10-21 air-runtime restored on the user's Mac -> lock-screen dots OK, Sequoia + Macintosh wallpapers OK (user).
- b124: nmx-autoarm adds !WallpaperSequoiaExtension / !WallpaperMacintoshExtension to the allow list and warns when
  air-runtime/libLLVM.dylib is missing; PLUGIN-UPDATE.txt documents the binary-only plugin update.
  Kept from b119-b123: descriptor recycling, stand-in copy, image recycling, small-image blocks, settle copy.
- b125 (R2, step 1 - double-buffered flip): NMXGPU bridge gets a second scan-out buffer (drm client buffer, same pitch,
  scaled view for Retina sizes) and nmx_fb_flip_back/commit/home (atomic commit of each lit CRTC's PRIMARY plane only,
  blocking until the flip is on screen - cursor plane untouched; later mode sets keep the current front buffer).
  NMXGPUFB platform function "nmxFlip" (op 0 back, 1 commit, 2 home). NMXAccel: with nmxflip=1 or
  sysctl debug.nvaccel_iop_flipdb=1 frames (and settle copies) are copied into the back buffer and flipped at vblank;
  turning it off / pipe teardown flips home to buffer 0. Counters: nvaccel_iop_dbflips / _dbflip_fail / _dbflip_ns.
  Step 2 (zero copy) = WindowServer's own surfaces as GEM scan-out buffers (needs GPU-written-LLC flush before scanout).
- 10-21 nmx-flipcheck (b125, flip ON): 8 frames in 15 s, ~700 ms per flip. Cause: the i915 commit-trace patch calls
  nmx_step() ~37 times per atomic commit and every nmx_step writes + syncs NVRAM (slow firmware write). Flip OFF: 33
  frames/s, 1.3 ms per copy. User: password dots were fine with the flip ON (whole finished frames).
- b126 (NMXGPU): after WindowServer opens the framebuffer, "commit ..." breadcrumbs are dropped (no log, no NVRAM;
  -nmxsteps keeps them) - also speeds up DPMS / wake / resolution commits. Flip is NONBLOCKING
  (drm_atomic_nonblocking_commit); each CRTC's drm_crtc_commit is kept and waited for (flip_done, 100 ms cap) only
  before the CPU writes into the buffer being taken off the panel.
- 10-21 b126 flip ON -> panic "thread_call group 'high' reached max thread cap (500): blocked 499" (settle thread calls
  piling up on gIopCopyLock while flips held it). Next boot (flip off, uptime 13 s): panic "preemption_level -1
  (unlocking an unlocked mutex or spinlock)" in a kext stack - cause unknown (unsymbolicated); watch for a repeat.
  User: password dots fine on b126 with the flip off.
- b127: settle uses IOLockTryLock (busy -> re-arm 40 ms, never blocks a thread-call thread); flip wait capped at 40 ms.
  The flip stays OFF by default and is not to be retested until the boot panic is understood.
- 10-22 b127 flip ON: whole Mac stuck after ~10 s (b125's BLOCKING flip never hung, only slow). Suspect the
  nonblocking commit (commit tail on the port's workqueue thread vs vblank event delivery).
- b128: nmx_fb_flip_commit is blocking again; NMXGPUFB runs it on its own thread call (fFlipCall) so NMXAccel never
  waits for vblank: op 0 waits <= 40 ms for the previous flip (else EBUSY -> that frame is copied), op 1 starts the flip,
  op 2 waits <= 100 ms then goes home.
- 10-22 b128 flip: stable (591 flips, 0 failures, ~1.3 ms each, 35 fps, no lag). Typed characters (Safari address bar,
  lock-screen dots) sometimes stay invisible until the NEXT key - with the flip on AND off (user: also in earlier builds).
  A screenshot taken then shows the character, so WindowServer's frame has it but the panel shows an older one.
- b129 (NMXAccel only): late-change detector. A checksum of the surface at copy time and at every settle pass; settle now
  runs at 40, 190, 440 and 1040 ms (passes 2-3 only copy if the content changed). Counters debug.nvaccel_iop_late40/190/
  440/1040, _latechk_ns; sysctl debug.nvaccel_iop_latechk=0 turns it off. Test: tools/macos-helpers/nmx-late.sh.
  Late changes > 0 = the GPU finishes frames after performTransaction (missing fence wait); 0 while characters still
  vanish = the surface itself lacks the character (WindowServer/plugin side).
- 10-22 b129 nmx-late: 561 frames while typing, ZERO changed after being shown (40..1040 ms). The composited frame itself
  lacks the character -> not a missing GPU fence in the kernel path.
- b130 (Metal plugin + NMXAccel): suspected cause - an app's IOSurface write lock moves the seed when the write STARTS;
  surface-in can upload a half-drawn surface, record the new seed and never upload the finished glyph (unlock does not move
  the seed). Fix: after a seed-change upload, the next use uploads once more (_surfVerify). Off: touch
  /Library/GPUBundles/nvmtl-noverify. NMXAccel: late checksum now off by default (sysctl debug.nvaccel_iop_latechk=1).
- 10-22 sleep: Apple menu Sleep never sleeps - "Entering DarkWake ... Software Sleep" then any HID event -> FullWake.
  pmset sleepnow (hands off) DID sleep after 23 s: "Kernel Client Acks: WindowServer timed out (30000 ms)" and
  WindowServer logged "CoreRCPlugin: Display sleep not yet completed, defer ack of IOPM sleep". Our NMXGPUFB acks power
  changes itself and never runs IOFramebuffer's power protocol (base calls crashed before open, 10-15), so WindowServer
  never sees display sleep complete. Wake (power button): backlight on, screen black, WindowServer had a NEW pid
  (restarted). NMXGPU SetState 1 took 821 ms.
- b131 (NMXGPU only): NVRAM nmx-fbpm=1 (read at every power change) hands setPowerState / powerStateWillChangeTo /
  powerStateDidChangeTo to IOFramebuffer once opened. Off by default; sudo nvram -d nmx-fbpm restores b130 behaviour.
  nmx-sleep2.sh v2 adds the driver's and WindowServer's sleep/wake messages.
- 10-22 b131 nmx-fbpm=1: SLEEP WORKS (WindowServer acks in 70 ms, real sleep, same WindowServer pid after wake). Wake:
  backlight + cursor only. iop_ok frozen after wake (4659), fail 0; WindowServer: "Fallback GPU Display Pipe transaction
  submission failed". NMXAccel logged DM(modern) displayModeWillChange at sleep and NO displayModeDidChange after wake.
- b132 (NMXAccel only): registerSleepWakeInterest; 1.5 s after kIOMessageSystemHasPoweredOn (and 2 s, 4 s later) the
  display machine's mode-change nest (+0x10c) is checked; if > 0, display_mode_did_change(0) is delivered. Logs "pm:" and
  GATES. sysctl debug.nvaccel_wakefix (default 1), counter debug.nvaccel_wakefixes.
- Brightness slider: ioreg IODisplayParameters brightness = 8 (correct), WindowServer "Linear brightness after wake 0.08",
  yet Control Center's slider shows full after a restart. Open: CoreBrightness keeps its own value (generic LCD display,
  not AppleBacklightDisplay?).
- 10-22 b132: sleep + wake work every time (power button). debug.nvaccel_wakefixes = 1 after ~6 cycles - the open
  mode change happens sometimes; the fix closed it once.
- b133 (NMXGPU only): the IOFramebuffer power protocol is ON by default; NVRAM nmx-fbpm=0 turns it off.
  Keyboard wake (PS2 internal / USB) and the lock-on-wake delay are OpenCore/ACPI and macOS settings, not this driver.
- 10-22 nmx-bright: panel = plain AppleDisplay (IODisplayConnectFlags 0xaaaaaaaa = never answered), corebrightnessd
  "Found 0 useful devices" -> Control Center's slider is not tied to our backlight. (User still ran b131 NMXGPU: b133's
  default-on fbpm not loaded; nmx-fbpm=1 used.)
- b134 (NMXGPU only): with a working PWM backlight the panel is offered as built-in: kConnectionFlags = kIOConnectionBuiltIn,
  "built-in" on NMXGPUFB, "AAPL,backlight-control" = 1 on the GPU (if absent) -> AppleBacklightDisplay should match (needs
  the "backlight" service = NMXBacklight + generic-LCD sense, both present). Boot-arg -nmxnobldisp: off. Includes b133.
- 10-22 brightness slider: AppleBacklightDisplay now matches (b134), but CoreBrightness keeps its own value (1.0 after a
  restart). DisplayServicesSetBrightness(main, saved/100) from the user session fixes it (nmx-bright2: 1 -> 0.07).
  tools/macos-helpers/nmx-brightsync: LaunchAgent at login, syncs at 8 s and 30 s (install-brightsync.sh).
- 10-22: user confirms slider correct after restart with nmx-brightsync; sleep/wake fine, and with b134 mouse AND keyboard
  now wake the Mac too. b135: install-autoarm.sh also installs nmx-brightsync (one installer for the userland parts).
- 10-22 b136: autoarm v5 - acceleration came ~2 min after a restart (waits sized for the freeze hunt). Now WindowServer
  stable 8 s (was 45), >= 25 s since boot (was 120), 3 s watch after the arm (was 20). /Library/GPUBundles/nmx-autoarm-slow
  restores the old timings. Login before the arm still means no WindowServer restart (acceleration after log out).
- b137: opt-in /Library/GPUBundles/nmx-autoarm-fast: 3 s stable, no since-boot minimum, 1 s watch.
- 10-22 heat: the GPU ran at 800 MHz all the time (b101 fixed floor; VBL log "min 750 avg 799 max 800"), and RC6 is
  still blocked (nmxgtawake=2 default since the 10-18 freeze).
- b138 (NMXGPU only): clock governor. Every 200 ms nmx_i915_gpu_gov_tick reads i915's per-engine busy time; >= 15 % busy
  on any engine -> floor nmxgpumhz (800) / ceiling = part max; 1 s quiet -> floor = ceiling = nmxgpuidle (default 350).
  Paused across sleep, stopped at shutdown/stop. nmxgpuidle=0 = the old fixed floor. Log once a minute.
  RC6 (nmxgtawake=1 test) left for later - it was the 10-18 freeze.
- b139 (NMXGPU, includes b138): one-shot RC6 test - sudo nvram nmx-rc6-try=1; the governor tick (every 5 s) deletes
  the variable and releases the keep-awake hold for that boot only. Note: since b131 every sleep already releases the
  hold (setPowerState 0 -> releaseAwake) and wake never re-applies it, so after any sleep/wake RC6 has been allowed -
  no freeze was reported after wakes on 10-22, a good sign for making RC6 the default.
- b140: autoarm v6 - SIGTERM trap records 'STOP: clean shutdown/restart', so a user restart during the 60 s watch no longer trips the guard (10-22 false guard after a manual restart).
- 10-22 user: WITHOUT acceleration (guard boot, WindowServer unaccelerated) typed text never goes invisible. So the
  invisible-character bug is in the accelerated path: WindowServer compositing through the Metal plugin (CompositorMetal
  on NVMTLDriver) - not the kernel copy/flip (b129: frames never change after being shown). Suspects: a missing
  dependency/barrier between the app's layer upload (surface-in) and the composite draw, or a stale texture cache
  (image recycling b121 / slab b122 / descriptor recycling b119) - test with the nvmtl-noimgrecycle / nvmtl-noslab /
  nvmtl-norecycle switch files when we come back to it.
- 10-22 RC6 test: released at 20:35:55 (nmx-rc6-try), accelerated use (governor 20-34 % busy) - no freeze.
- b141 (NMXGPU): RC6 / GT parking allowed by default (nmxgtawake default 0). Block again: boot-arg nmxgtawake=2 or
  NVRAM nmx-rc6-block=1 (read at attach). Governor log shows the requested clock (rps->cur_freq; the actual-frequency
  register read 0).
- 10-23: b141 overnight - no freeze, no heat, no lag. Governor log showed BUSY at 350-450 MHz: bug - the busy branch
  clamped to intel_rps_get_max_frequency(), i.e. the current softlimit, which the idle step had lowered to the idle clock.
- b142 (NMXGPU): busy ceiling/floor from rps->max_freq (RP0). Busy now really reaches nmxgpumhz (800).
- 10-23: flip used by the user without problems (incl. b142). b143 (NMXAccel): double-buffered flip ON by default; nmxflip=0 turns it off.
- 10-23 invisible letters: nvmtl-nofbfetch, -noimgrecycle, -noslab, -norecycle (one at a time, log out/in) - no change.
- b144 (plugin): /Library/GPUBundles/nvmtl-nosurfhost turns off zero-copy IOSurface textures (host-pointer import) ->
  surface-in copies with the seed check (+ b130 verify unless nvmtl-noverify). Test: does the glyph still lag one update.
- 10-23: nvmtl-nosurfhost (copies instead of zero-copy app surfaces) - letters still vanish.
- b145 (plugin): file switches /Library/GPUBundles/nvmtl-g79off and nvmtl-e78off turn off the barrier-elision shortcuts
  (G79: lazy/folded barriers, narrowed encoder barriers, event/submit folding; E78: encoder hazard tracking rows). A
  missed barrier between a blit (layer update) and the composite render that samples it gives exactly "one update behind".
- 10-23: nvmtl-g79off + nvmtl-e78off - letters still vanish. New theory: the frame WITH the glyph never reaches the
  kernel. NMXDisplayPipeM finishes each transaction synchronously but never raised the transaction interrupt, so the
  family re-checked isTransactionComplete only on the next event; WindowServer keeps one transaction in flight, so its
  newest frame waited -> one update behind. b129's checksum watched the SHOWN surface, not the pending one.
- b146: NMXAccel raises signalTransactionInterrupt on a thread call after every successful transaction (sysctl
  debug.nvaccel_iop_txsig, counter _txsigs). NMXGPU: busy clock default 550 MHz (was 800; user: 800 warmer).
- 10-23 b146: txsigs 4079 (signals flowing) - letters still vanish. User: ONLY in Safari and on the lock screen - both
  are text over a blurred (vibrancy / backdrop) background. b73 found the async commit worker breaks backdrop-blur in
  Chromium helpers (black boxes); WindowServer still commits async.
- b147 (plugin): /Library/GPUBundles/nvmtl-synccommit -> synchronous commit in every process (incl. WindowServer).
- 10-23 invisible letters, verified in nvmtl-ws.log that the switches were live (pid 784 synchronous commit; g79/e78 off
  in an earlier WindowServer). Ruled out: fbfetch, image recycling, slab, descriptor recycling, zero-copy app surfaces,
  barrier/encoder shortcuts (g79/e78), async commit, blur/vibrancy (Reduce transparency on - still happens), kernel
  transaction completion (b146 txsig), late GPU writes after a transaction (b129). Still: only Safari's address bar and
  the lock-screen password field, only accelerated, always one update behind, screenshot correct.
  Next when we return: a trace build - WindowServer frame log (per commitAndWaitUntilSubmitted: cb id, encoders, damage
  if visible, transaction id) + the kernel transaction log, around one key press, to see whether the frame with the
  glyph is composited at all and where it stops.
- 10-23 cold boot panic 11:32:58, 16 s uptime: "NMIPI for unresponsive processor: TLB flush timeout" (panicked thread
  contextstored page-in; another CPU unresponsive with interrupts off). First boot after b141 dropped the boot-time
  keep-awake hold. b148 (NMXGPU): hold level 2 through the boot, release 90 s after attach (governor tick 450).
  nmx-rc6-block=1 or nmxgtawake=<n>: no auto release.
- Boot black screen (user: big Apple logo, ~10 s black, small logo): timeline - BSD root +0 s, NMXGPU attach +8 s (needs
  the disk for DMC firmware), fwfb taken over then a NEW 2560x1600 scan-out buffer painted black (+8.6 s; the firmware
  snapshot is off since b78); the small logo is the kernel's boot progress drawn on NMXGPUFB at the panel's native
  size. Test: boot-arg -nmxsnap (copies the firmware screen into the new buffer).
- b149 (NMXGPU): boot snapshot ON by default again, read from STOLEN memory with a cached mapping (the firmware screen
  is at GGTT 0 = start of stolen), never through the aperture (b78: 9-13 s uncached). 64 spread samples are compared with
  the aperture first; mismatch -> skipped (black as before). -nmxnosnap: off. Logs the copy time.
- 10-23 12:08/12:13 panics: MACHINE CHECK, IA32_MC11_ADDR 0x4c800000 = stolen memory base, ~8 s uptime - b149's CPU read
  of stolen memory. Tiger Lake: no CPU access to DSM. b150 (NMXGPU): snapshot code back to b148 (off by default).
  The black gap needs another way (e.g. keep scanning the firmware FB until the console draws) - parked.
- 10-23 13:01 boot panic: "thread_invoke: preemption_level -1, possible cause: unlocking an unlocked mutex or spinlock",
  NMXGPU attach -> nmx_fb_publish -> NMXGPUFB::start -> nmx_fb_cursor_init -> drm_client_buffer_vmap -> nmx_gem_vmap ->
  ggtt_pin -> shmem_get_pages -> shmem_sg_alloc_table -> cond_resched -> sched_relinquish -> thread_block. Same panic
  type as the 10-21 unexplained boot panic (first seen the day b120's per-CPU thread cache came in). The imbalance comes
  from an earlier unlock on that CPU, not from this path.
- b151 (NMXGPU): per-CPU thread cache off by default (-nmxtcache = on). Governor: busy clock as the floor during the
  first 90 s (login lag after a restart: trustd 85 % CPU, WindowServer 10-20 fps, all shader caches hit).
- b152 (NMXGPU, includes b151): stepped Windows-like governor - steps 300 / 450 / 650 / 900 MHz (user wants max 900):
  load >= 60 % top, >= 30 % third, >= 5 % second, else idle; up at once, down one step per 1 s of lower demand; first
  90 s at least step 2. Clock pinned per step (min = max) with i915's sysfs-equivalent calls (nmx_i915_gpu_pin).
  Boot-args nmxgpuidle (idle step; 0 = no governor, fixed floor nmxgpumhz) and nmxgpumax (top, 300..1300).
- b153 (NMXGPU + plugin): governor top step 1300 MHz, the part's maximum, as on Windows (user 10-23: 900 ran fine for
  days, wants Windows-like). Steps 300 / 500 / 850 / 1300; nmxgpumax=900 restores a 900 MHz cap.
  Brave white bars / flicker / stale overlapping text (10-23): only with Skia Graphite (Dawn on Metal);
  --disable-skia-graphite (Ganesh over ANGLE Metal) draws correctly on the GPU. Ruled out: partial raster
  (--disable-partial-raster), zero-copy surfaces (NVMTL_SURFACE_HOST=0, env propagation not verified). GPU helper log
  (with --disable-gpu-sandbox) shows no errors; every 4-sample pass begins with NO resolve target ("resolves colour 0x0
  of 0x1") and resolves in a separate empty pass. Trace (~/nvmtl-trace-request) extended: PASS lines carry samples, resolve
  target, depth load/store, render-target size; EVENT signal/wait and COMMIT lines added.
  Brave tests 10-23 (no build, env knobs/switches, all still bad): every plugin reuse/recycle/zero-copy/residency knob off;
  Dawn vertex pulling; Dawn without robustness; INTEL_DEBUG=nofc,noccs,nohiz; INTEL_DEBUG=sync. A screenshot shows the
  bad picture too, so Brave's own buffers hold it (not a WindowServer display problem). Graphite frame: expand previous
  window picture into a 4x MSAA buffer, composite raster tiles (1600x256 IOSurfaces, premultiplied src-over), depth-only
  and stencil-then-cover clip draws, then a shader resolve into the window picture (scissored to the damage).
  b154 (plugin): ~/nvmtl-dump-request saves every written IOSurface (tiles, window pictures) as PPM after its command
  buffer finished (~/nvmtl-dump, 80 files max, each surface every 4th write; DUMP lines in the trace) - tells whether the
  raster tiles are already wrong or only the composited window picture.
  10-23: after the b154 plugin install Brave died at start, "SIGKILL (Code Signature Invalid)" inside dlopen of the
  plugin (Metal MTLCopyAllDevices). nmx-plugin-update.sh copied the new binary INTO the old file (same inode), which
  macOS's code-signing cache rejects. Fixed: copy to NVMTLDriver.new, then mv over. A restart also clears it.
- b155 (translator): ROOT CAUSE of Brave's Graphite white bars / stale overlapping text / missing icons (10-23). The b154
  surface dumps showed the raster tiles already wrong: the tab-strip background missing (tile clear colour white shows).
  The draw that paints it - Graphite's CoverBounds step, vs 5b64f2be, instanced quads, instance data only - was emitted
  with NO vertex outputs (no gl_Position, no varying), so it drew nothing. The same step draws page backgrounds and the
  cover pass of stencil-then-cover paths. Cause in stage_output::rewrite_return: it looked the entry's return type up in
  the type table captured by build_stage_input; materialize_inlined_local_pointer_field_stores (run in between) gives
  entries that fill their return value through a non-inlined helper (Tint `v_4(..., &retval)`) a new return struct id,
  the lookup missed, and every output was silently dropped. Fix: look types defined since then up in the module; an
  entry return type that still has no definition is now an error, not a shader without outputs. Reproduced and checked
  on Linux with the user's ~/Library/Caches/nvmtl AIR: 3 of 77 cached vertex shaders had no position, 0 after the fix;
  all 194 cached entries translate and validate.
  nmxgpu-intel-build.yml now also builds libnvmtl_translate.dylib (out/nvmtl/); install with nmx-xlate-update.sh.
- b156: b155's translator was built arm64 (the GitHub macOS runners are Apple Silicon) - "incompatible architecture",
  every WindowServer start had no translator and WindowServer restarted every ~10 s at the login screen (10-23 20:22).
  The auto-arm guard kept the next boot unaccelerated; the old translator was restored. Workflow now builds
  --target x86_64-apple-darwin and checks lipo; nmx-xlate-update.sh refuses a non-x86_64 or export-less dylib.
- b157 (plugin): invisible typed letters (Safari address bar, lock-screen password) also happen with Reduce
  Transparency on (10-24, after the b156 translator) - not the blur. /Library/GPUBundles/nvmtl-wsdump makes WindowServer
  save the CHANGED rectangle of every finished frame on a >= 1000 px IOSurface (PPM, time in ms + position in the name)
  to nvmtl-wsdump/ next to nvmtl-ws.log. If the newest letter is in WindowServer's own picture at once, the display path
  (copy/flip in NMXAccel) shows it late; if it appears there only with the next update, WindowServer drew it late.
- b157 result (10-24 08:15): WindowServer's OWN frames lack the letter - case A, not the display path. In the frame
  after a keystroke the caret has moved but the last glyph(s) are blank ("gs  |" instead of "gsh|"); the next update
  shows them. WindowServer renders into two full-screen surfaces alternately (1/4), each copied on to 2/3. The caret is
  its own layer, so the stale part is the text field's own app IOSurface as WindowServer sampled it.
- b158 (plugin): with nvmtl-wsdump, WindowServer also saves every app IOSurface it samples (<= 1400x240) when its seed
  moved (in-*.ppm, CPU view at encode time, zero-copy "host" or "copy" in the name) - shows whether the app surface
  already held the glyph when WindowServer recorded the draw (then the GPU read stale data) or not (sync/timing).
- b158 result: during typing no sampled app surface was saved - every saved one was zero-copy ("host"), and the
  1400 px width limit left out Safari's toolbar (window-wide). b159: inputs up to 4096 x 400, saved when their CONTENT
  hash changes (not only the seed), hash in the file name.
- 10-24 video (Safari, typing): the dropdown suggestion list (another layer) already shows the new text while the
  address field shows the old one with the caret moved on; some frames show the field half-updated (new letters over
  "Search Google"). Mixed old/new pixels in one surface = the GPU reading the app's pages while part of the CPU's
  writes are not visible to it yet.
- b160 (plugin) tests: /Library/GPUBundles/nvmtl-hostflush - zero-copy surfaces get their pages written back from the
  CPU caches (clflush) before each GPU use (if this fixes it: the imported pages are mapped for the GPU without
  snooping - fix in NMXGPU's userptr mapping); /Library/GPUBundles/nvmtl-surfin-always - copy mode uploads at every
  use, seed or not (with nvmtl-nosurfhost; if this fixes it: apps change surfaces without moving the seed).
- b160 result: neither clflush of zero-copy IOSurfaces nor a fresh copy of every IOSurface at every use helped.
  b159/b160 dumps (wsdump3): while typing, NO sampled IOSurface changed content - the address field's text does not
  reach WindowServer as an IOSurface. Likely a shared-memory backing store, which WindowServer turns into a Metal
  buffer with newBufferWithBytesNoCopy (zero-copy host import) and copies into a texture.
- b161 (plugin): /Library/GPUBundles/nvmtl-nohostbuf - bytesNoCopy buffers become copies (resynced at every
  submission) instead of imported pages; /Library/GPUBundles/nvmtl-wstrace - G15 trace in WindowServer only, written
  next to nvmtl-ws.log (BLIT/REPLACE/PASS/FTEX lines show how the typed text reaches the screen).
- b161 result: nvmtl-nohostbuf (bytesNoCopy as copies) did not help. The 20000-line trace ended before the typing:
  WindowServer renders a 1679x947 IOSurface (surf 183, the Safari window size) itself and samples it later, and draws
  many small offscreen targets (64x64, 164x11, 212x10 ...). No REPLACE/BLIT seen in that window.
- b162 (plugin): WindowServer trace keeps only PASS/FTEX/REPLACE/BLIT/COMMIT/EVENT/SURFIN plus new NEWSURFTEX (IOSurface
  textures), NEWBUFTEX (textures over buffers) and NEWNOCOPY (bytesNoCopy buffers), each line with a time in ms, up to
  400000 lines - run together with nvmtl-wsdump to match the frame that lost the letter with where its content came from.
- b163 (plugin): Photo Booth showed black. Cause 1 (config, no code): it was not in nvmtl-allow.txt ("not allow-listed"). Cause 2: CoreImage draws the camera frame into the CAMetalLayer drawable with CIRenderDestination, which needs the drawable's -usage to include ShaderWrite ("texture usage must include MTLTextureUsageShaderWrite. The destination is nil"). IOSurface colour textures that are not framebufferOnly and are render targets (or Unknown usage) are now created as storage images and report ShaderWrite when that worked; WindowServer excluded; NVMTL_NO_SURFACE_SW=1 off. Log line "b163 surface texture usage". The camera's 420v planes come in as R32Uint 320x720 / 320x360 (PixelConverter).
- 10-24: invisible letters PARKED again. Ruled out on WindowServer as well (conf-file knobs, all at once): heap texture
  placement/recycle/aliasing, buffer-texture recycle, buffer park, pool reuse, residency, dirty-only, DontCare clears,
  pre-submit prologue, event folding, submit-ahead, skip-empty, async commit. Remaining lead: the text reaches the screen
  through WindowServer's own small offscreen renders (683x22, 697x309, buffer textures filled by replaceRegion); in the
  bad frame the address text is an OLDER state ("ghg" after "ghghg"), not a half-drawn one.
- Battery (10-24): root cause in the DSDT. Every battery/AC method reads the EC through H_EC.ECRD, which returns 0 unless
  ECAV = 1. ECAV is set in H_EC._INI only when _REV >= 2, or in H_EC._REG for an EmbeddedControl region - this EC has
  none (SystemMemory ERAM at 0xFE0B0300), so on macOS ECAV stays 0: BAT0._STA reports no battery and _BST/_BIF/_PSR read
  zeros - why SMCBatteryManager (and any other battery driver) never started. SSDT-ECAV sets ECAV/ECTZ on Darwin from a
  helper device's _INI. Shipped with SSDT-BATT + the _BIF/_BIX renames again (config8).
- 10-24: SSDT-ECAV alone (with SMCBatteryManager) still showed no AppleSmartBattery. b163: own driver NMXBattery.kext
  (a SEPARATE project, NMXBattery-source.zip, its own GitHub workflow - not part of this repository): matches the ACPI battery (PNP0C0A), reads _STA/_BIF/_BST and the
  adapter's (ACPI0003) _PSR every 15 s and on ACPI notifications, publishes an IOPMPowerSource (created by hand -
  IOPMPowerSource needs its own init()). Logs every reading ("NMXBattery:" in the kernel log); _STA 0x0B means the EC
  reads 0 (SSDT-ECAV missing or not effective). config9: NMXBattery on, SMCBatteryManager off, VirtualSMC kept.
- 10-24: the battery kext was renamed SimpleACPIBattery (bundle local.SimpleACPIBattery, no NullMoth/NMX names - user's
  wish), separate project SimpleACPIBattery-source.zip; SSDTs re-made with OEM ID "LOCAL" (helper device ECAX). With
  SSDT-ECAV loaded BAT0._STA reads 31 (0x1F, present) - the EC fix works; SMCBatteryManager still publishes nothing.
  The user's EFI partition (disk0s1) mounts as "NO NAME", not "EFI".
- b164 (NMXGPU): boot black screen. The bridge no longer commits its (black) scan-out buffer at attach when an output still
  shows the firmware screen (i915's initial-plane takeover keeps it lit): the first mode set waits until WindowServer opens
  NMXGPUFB (enableController -> thread call -> nmx_fb_go), or nmxdeferto seconds (default 120). Hotplug/DPMS/restore wait
  too; set_size and the accelerated flips do the hand-over first. The big boot logo and progress bar stay until the login
  window; the second, smaller logo (drawn on NMXGPUFB at 1x) is never shown. -nmxnodefer = old behaviour.
  Next (not done): acceleration before the login window needs NMXGPU loaded before WindowServer (OpenCore injection,
  firmware embedded instead of /Library/NullMoth/firmware) and a pre-arm, so no WindowServer restart is needed.
- 10-24 nmx-boottime (b164 boot): BSD root 0.9 s, kernelmanagerd + WindowServer + loginwindow 3.3 s, NMXGPU loaded 6.3 s
  (AuxKC via kernelmanagerd; the firmware folder was already there), NMXGPUFB 8.7 s, WindowServer opened it 9.9 s,
  autoarm (fast) armed 17 s, WindowServer restarted 21 s. WindowServer starts ~3 s before NMXGPU can exist.
- b165 (NMXGPU + autoarm v7): "hold" - with nmxws=1 nmxautoarm=1 (and the autoarm guard clear) the b164 hand-over waits
  past the first, unaccelerated WindowServer: the boot logo stays until the accelerated WindowServer's first flip, the
  helper's signal (NVRAM nmx-handover=1, sent on every exit of nmx-autoarm.sh and 2 s after the restarted WindowServer is
  up), or 60 s (nmxdeferto). The pointer is not shown over the logo. The lock screen appears once, accelerated - the
  restart is hidden behind the logo. -nmxnohold = b164. Truly earlier acceleration still needs NMXGPU in the boot KC.
- 10-24 b165 test: only the big logo now, but the hand-over came at 15.8 s, 2 s after the first (unaccelerated)
  WindowServer opened NMXGPUFB - its setDisplayMode -> nmx_fb_set_size called bridge_go. b166: set_size while deferred
  only notes the size (the hand-over's commit applies it); cursor_reapply does nothing while deferred. nmx-boottime prints
  the hand-over reason.
- 10-24 b166 test: hand-over at 12.0 s "first accelerated frame", before the arm (16 s): NMXAccel flips for the first,
  unaccelerated WindowServer too. b167: while deferred no flip reaches the panel (nmx_fb_flip_commit -> EAGAIN, the
  frame is remembered: pending_back) and flips never end the hold; the hand-over (helper signal / WindowServer open
  without auto-arm / timeout) shows the newest frame (buffer 2 if one is pending) under flip_lock.
- 10-24 b167 test: hand-over at 12.0 s with reason "the auto-arm helper is done" - before the helper had run: a signal
  left in NVRAM by the previous boot. b168: NMXGPUFB publishes a per-boot NMXHoldToken; only nmx-handover=<token> ends the
  hold; autoarm v8 reads the token from ioreg (no token = no hold = no signal).
- 10-24 b168: WORKS. Firmware logo held until 30.8 s, hand-over "autoarm helper signal" right after the accelerated
  WindowServer's first frames (flips refused with 35 x6 while held, then "1 frames flipped"); no plain lock screen first.
  nmx-boottime's 16.7 s was the "DPMS on before the boot screen hand-over" line - pattern fixed. Seen: one attach vmap
  (cursor buffer) took 3.9 s (cache level set) - NMXGPUFB at 12.1 s instead of 8.8 s; watch it.
- 10-24 user (b168): Apple logo, then ~6 s black screen with the pointer, then the lock screen - the restarted
  WindowServer's first frames are black. b169: after the helper's signal the hand-over waits until the newest held frame
  (buffer 2) shows a picture: >= 8 of 64 grid pixels not near-black (nmx_fb_back_content), checked every 100 ms, at most 10 s.
- 10-24 user: the ~6 s black + pointer before the lock screen is fine. b170 keeps the b168 hand-over by default; the b169
  picture wait is opt-in (boot-arg -nmxholdpic).
- 10-24 power baseline (b168, nmx-power.sh 120 s, battery, 11% backlight, idle desktop): 10.7 W average (5.1..14.9 W);
  GT parked by RC6 (clock 0 MHz), governor at step 0. The build keeps enable_psr / enable_fbc / enable_dc = 0 because the
  CPU-drawn (unaccelerated) framebuffer is not frontbuffer-tracked.
- b170: panel self refresh at run time. After 300 accelerated flips (atomic commits, which PSR follows) the bridge calls
  nmx_i915_psr(drm, 1) = intel_psr_debug_set(FORCE_PSR1) on every eDP with sink_support (psr_global_enabled ignores
  enable_psr=0 when the debug mode is not DEFAULT; PSR2 / panel replay stay off). nmx_fb_flip_home (back to the CPU-drawn
  buffer 0) sets it to DISABLE first and restarts the count. Cursor moves go through drm_mode_cursor2 -> legacy cursor
  update -> frontbuffer flush, which PSR handles. Off: -nmxnopsr. Log: "panel self refresh (PSR1) ON|not available".
- 10-24 b170 test: "panel self refresh (PSR1) ON after 300 accelerated frames (1)" ~1 min after boot. nmx-power.sh 120 s,
  same conditions (battery, 11%, idle): 5.21 W average (4.2..8.6 W) vs 10.7 W on b168 - about half.
- b171 (helpers only, kexts = b170): shorter boot. autoarm v9: the quick timings are the default (2 s stable WindowServer,
  no since-boot minimum, 1 s watch; v8 default was 8 / 25 / 3, nmx-autoarm-slow still restores 45 / 120 / 20); every wait
  polls every 0.25 s instead of 1 s; the nvaccelfb=1 try is read back at once and retried every 0.5 s (v8: >= 2 s even
  when the first try worked); 0.3 s instead of 1 s around nvaccelfb=4. Hand-over signal after 2 s of the new
  WindowServer as before. OpenCore: Misc/Boot/Timeout 5 -> 2 s (any key still stops the countdown).
- 10-25 b171 boot: helper started 13 s after the kernel (launchd), everything was already up; "settled" took 5 s, not 2:
  pgrep costs ~0.4 s, so polls != seconds. WindowServer restart 21.1 s (v8 boot before: ~3 s later per step), signal 23 s.
  Timeline said "armed 9.5 s" (matched NMXAccel's install line) and "hand-over 13.3 s" (to check).
- b172 (helpers only): autoarm v10 measures stability by the WindowServer's process age (ps etime): the login window's
  WindowServer is ~10 s old when the helper starts, so it arms at once. Same for the hand-over (new WindowServer >= 2 s
  old). nmx-boottime: armed = "scanout-backed display pipe ENABLED"; prints the hold / hand-over lines.
- 10-25 user video (b172 boot): Apple logo -> lock screen (unaccelerated, ~1 s) -> black + pointer ~7 s -> lock screen.
  The hold ended with reason "autoarm helper signal" right when the helper started (b171 boot: hand-over 13.3 s, helper
  started 13.0 s, its own "hand-over signal sent" line only at 23 s). Only the helper writes nmx-handover, and the early
  write left no log line -> it ran in a subshell: most likely bash 3.2 running the EXIT trap at the end of a $(...)
  subshell. b173: autoarm v11 has no EXIT trap; every exit (and the TERM trap) calls handover explicitly.
- 10-25 log (b173 boot): the hand-over itself was right - 04:51:57.3 "autoarm helper signal - committing our scan-out
  buffer 0", 0.3 s after the helper's signal (the "11.7 s" was an old nmx-boottime matching the DPMS line). But buffer 0
  is the IOFramebuffer memory where the FIRST, unaccelerated WindowServer drew the lock screen with the CPU (no flip was
  pending), so that old lock screen showed for ~2 s, then the restarted WindowServer's first flip (04:51:59.1, black +
  pointer), then its lock screen. The EXIT-trap theory (b173) was wrong; v11 is still fine. v8 sent its signal later
  (slow pgrep counting), after the new WindowServer's first flip, so b168 never showed the old picture.
- b174: on the helper's signal NMXGPUFB calls nmx_fb_go_on_flip: the hand-over happens inside the next flip (the
  restarted WindowServer's first frame, buffer 2), at most 4 s later (then nmx_fb_go as before).
- b175: NMXGPUFB sets "model" = "Intel Iris Xe Graphics" (data, as Apple's drivers do) and VRAM,totalsize / VRAM,totalMB
  = 1536 MB on the GPU's IOPCIDevice (About This Mac said "Display 15 MB"), unless the config already injects a model.
- 10-25: nmx-wakecheck.sh (helpers): after a sleep/wake test, collects powerd's sleep/wake list, the driver's sleep/wake/PSR lines, WindowServer restarts, Wi-Fi state, panics.
- 10-25 Wi-Fi after wake: the card stays on, only macOS's auto-join fails once and is not retried. WakeWiFi (separate
  helper: toggle Wi-Fi when on without an IPv4 address after a wake) made it worse (stayed disconnected) - uninstalled,
  PARKED. Note: laobamac/itlwm (GPL-2) still has an AirportItlwm-Sequoia target; its setPowerStateOn() only acks (no
  re-enable at wake) - a fork remains an option.
- 10-25 invisible letters, new lead: b121 stand-ins (newTiledTextureWithBytesNoCopy <= 512 KB) are COPIED once at
  creation and never re-read; CA updates such glyph/image backing stores in place, so the GPU keeps the OLDER content
  until a new stand-in is made - matches "older state, the next update shows it" and "no sampled IOSurface changed".
  The 10-21 test (nvmtl-nostandincopy) was never run against the letters (the libLLVM root cause came first). Test now.
- 10-25 user: nvmtl-nostandincopy FIXES the invisible letters. ROOT CAUSE: b121's stand-in copy (copied once, never
  re-read) of buffers that Core Animation updates in place. b176 (plugin): stand-ins are imported (zero copy) by default;
  the copy mode is opt-in with /Library/GPUBundles/nvmtl-standincopy. Watch WindowServer CPU (b121 made the copy for the
  userptr cost, ~260 imports per 20 s).
- 10-25 Photo Booth (user screenshots): live preview and all 9 Effects tiles are turned 180 degrees vs QuickTime's
  (mirrored) preview; the saved photo is right; QuickTime / browsers right. pb dumps (10-24): VideoToolbox's
  VTMTSComputeFunction output (420v, surf 236/238) is upright; the drawable (720x480 IOSurface) holds the rotated picture
  -> it happens in Core Image's ci_affine_* compute kernel (affine sampling rows at b0+80/+96, chroma scale b0+128,
  write flip when b0[0].w < 0). b177 (plugin): /Library/GPUBundles/nvmtl-cidump logs the first 12 ci_* dispatches'
  uniform buffer 0 (floats), the textures and pixel samplers bound (syslog "NVMTL CIDUMP" + the nvmtl log).
- 10-25 CIDUMP (Photo Booth preview, ci_affine_..._colormatrix into 720x480 IOSurface, 1280x720/640x360 420v planes):
  b0[0] = (0, 0, 720, 480) -> w >= 0: NO write flip. Dest coord rows: x' = -1.5 x + 1180.5, y' = -1.5 y + 720 (sampling
  rows b0+80/96 = identity, pixel samplers, clamp). So the mirror is there (x reversed, centre 1080 of 1280 columns) and
  the source is also read bottom-up (y' = 720 - 1.5 y) for a y-up destination - written top-down into the surface, the
  picture is upside down = what the screen shows. Core Image decided "destination not flipped" on the CPU side.
- b178 TEST (plugin): /Library/GPUBundles/nvmtl-ciflip lists process names; for them a ci_* dispatch into an IOSurface
  with b0[0].w >= 0 runs with a copy of buffer 0 whose w = 2*origin.y - height (the kernel's own flipped write). If the
  saved photo or other CI output turns wrong, the rule needs the destination kind, not just "IOSurface".
- 10-25 user: b178 with "Photo Booth" in nvmtl-ciflip - preview upright and mirrored, Effects right. b179: on by default
  for Photo Booth (no file needed); the file replaces the default list, an empty file = off. Open: Photo Booth's photo
  strip delete badge (x) draws as a black square (not caused by the flip; only seen in Photo Booth).
- 10-25 System Information shows "Metal: Supported" instead of "Metal 3": it is not in nvmtl-allow.txt, so it gets no
  Metal device (the plugin answers MTLGPUFamilyMetal3 = 5001). Add "!System Information" / "!system_profiler".
- b180 (NMXGPU): external displays, step 1 = mirroring. The panel (eDP) leads: the scan-out buffer has the panel's size
  even with a monitor/TV connected at boot (before: the largest output's - a 4K TV would have made a 4K desktop; and an
  external mode larger than the buffer could fail the first mode set = no framebuffer at all), and the other outputs
  are re-probed with the panel size as their limit. Externals get the desktop size as a real mode if they offer it
  (full-screen mirror of a scaled desktop), else their own mode <= panel showing the buffer 1:1; only eDP uses the
  edited-timing panel-fitter trick. Flips pick the view per output (ms_view). Any refused mode set with an external lit
  is repeated with the panel alone (bridge_commit) - a monitor cannot take the laptop screen down. Logs "NullMoth
  display: output <name> (...)" after hand-over, hot-plug and desktop size changes. -nmxnoext: externals never lit.
  tools/macos-helpers/nmx-display.sh collects it. macOS does not know about the external yet (mirror only).
- 10-25 TV test (b180): nothing on the TV. Log: the panel is 2560x1600 (desktop 1680x1050 scaled); at hand-over only
  eDP-1 is listed; plugging HDMI in later produced NO hot-plug event (the six DRM.CONNECTOR.HOTPLUG lines are only the
  ones at driver load) - no HPD interrupt reaches i915 on this port, so bridge_hotplug never ran.
- b181 (NMXGPU): nmx_i915_hpd_poll -> intel_hpd_poll_enable (i915's own fallback when HPD is unavailable): every HPD
  connector becomes polled, DRM's output poll worker checks them every 10 s and sends the normal hot-plug event. Turned
  on at the boot screen hand-over and again at every display-on (a resume may switch polling off). -nmxnoext: no polling.
- 10-25: external displays PARKED (b181 polling untested; user stays on b180). Next: macOS 26 Tahoe.
- Tahoe plan: T0 user installs Tahoe on its own APFS volume next to Sequoia (newest OpenCore + kexts on a second USB
  stick; our kexts live in each volume's /Library/Extensions, so Tahoe starts clean). T1 NMXGPU alone on Tahoe (display,
  brightness, PSR, sleep; no nmxws arming - the autoarm daemon is per volume). T2 NMXAccel for Tahoe: the upstream
  NM_TAHOE layout (re/ + tahoe_fwd.h) exists, but our Intel display classes are "NMX_ACCEL && !NM_TAHOE" (12 blocks).
  T3 the Metal plugin + translator on Tahoe (mtlprobe / mtlwindow first, then WindowServer).
- b182 (CI): ci-build-intel.sh also builds out/tahoe/NMXAccel.kext with -DNM_TAHOE (stage nmxaccel26, non-fatal) to
  see what the Tahoe accelerator build needs.
- 10-25: Tahoe installed on its own volume (EFI: OpenCore 1.0.8 + AirportItlwm-Tahoe as a second kext, 25.x only).
  Wireless mouse not working there (open, not the GPU driver).
- b183 (NMXGPU): the boot-logo hold also ends 15 s after NMXGPUFB start when no NMXAccel service exists ("no accelerator
  (NMXAccel) after 15 s") - for T1 on Tahoe (NMXGPU alone, same OpenCore boot-args as Sequoia, no autoarm helper on that
  volume) the logo otherwise stayed for the 60 s timeout.
- 10-26 Tahoe T1: NMXGPU from /Library/Extensions on Tahoe hung at the Apple logo (hot); Tahoe's log could not be read
  from Sequoia (log archive empty). NEW PROJECT (user): load the driver from OpenCore - design in
  docs/OPENCORE-INJECTION.md (U1 firmware in the kext, U2 NMXGPU injected on Sequoia, U3 NMXAccel too if its
  dependencies are in the boot collection, U4 Tahoe from the USB stick only).
- b184 (U1): xnu/nmx_fw_embedded.cpp + gen_fw_embedded.py (CI stage fwembed, before runtime): tgl_dmc_ver2_12,
  tgl_guc_70.1.1, tgl_huc_7.9.3 built in (SHA-256 checked); nmx_xnu_firmware_load tries them first, and no path lookup
  before a root vnode exists; nmx_xnu_wait_firmware_root returns at once when firmware is built in.
  tools/macos-helpers/nmx-bootkc-check.sh: are IOAcceleratorFamily2 / IOSurface in the boot collection (decides U3).
- 10-26 Tahoe 26.7.1: the earlier white screen / 1 fps was NO display driver at all (OpenCore blocked IONDRVSupport for
  every macOS, and NMXGPU was installed but not approved). Block limited to 24.x -> IONDRV desktop (slow). Then NMXGPU
  approved + IONDRV blocked again: T1 WORKS on Tahoe - "driving 8086:9a49", firmware framebuffer taken over, NMXGPUFB
  (NMXAppleIntelFramebuffer) 2560x1600@60 is the display, hardware cursor, backlight, "Intel Iris Xe Graphics 1536 MB".
  Same NMXGPU binary as Sequoia (b184). Next: T2 = out/tahoe/NMXAccel.kext loaded, not armed.
- 10-26 Tahoe T2/T3: NMXAccel (out/tahoe, NM_TAHOE build) loads after approval ("Dependencies: OK", "Bad code signature"
  is normal), IOGraphicsAccelerator2::start -> 1, WindowServer opens a display pipe; the Metal plugin (Sequoia's
  /Library/GPUBundles copied over) works: mtlprobe "Metal compute works", mtlwindow 60 fps, 0 GPU errors.
- b185 (NMXAccel, Tahoe): the Intel MODERN display classes (NMXSurfaceM / NMXDisplayPipeM / NMXDisplayMachineM, b116,
  what nmxmodern=1 uses on Sequoia) are now built for Tahoe too. tahoe_fwd_nmx.h forwards IOAccelSurface's 4 slots Tahoe
  made private (set_shape, set_shape_backing_length_ext, surface_lock_options, surface_unlock_options) through
  __ZTV14IOAccelSurface (same offsets as tahoe_fwd.h's NVSurface forwards). setStereo / submitCopyForward / submitUpdate
  belong to the legacy classes only. More unresolved symbols, if Tahoe reports any, get added the same way.
- 10-26 b185 on Tahoe 26.7.1: "display classes MODERN", NMXAccel loads, no unresolved symbols. With NVRMAGDC.kext and
  autoarm v11 installed on the Tahoe volume: armed at 16 s, WindowServer restarted, "armed WindowServer up for 60 s",
  debug.nvaccelfb = 1. FIRST ACCELERATED TAHOE BOOT (same NMXGPU / plugin binaries as Sequoia).
- 10-26 Tahoe accelerated: wallpaper + Dock right, a little lag. System Settings sidebar icons yellow squares (System
  Settings was on the GPU list copied from Sequoia; test: removed from nvmtl-allow.txt). nvmtl-ws.log: no translation
  errors. USB dead on Tahoe (not GPU): both xHCI (TXHC 9a13, PCH a0ed) attach, createPorts, then XHCI goes to D3 and no
  device ever enumerates (not even the OpenCore stick) - open, next: ioreg comparison with Sequoia.
- 10-26 Tahoe yellow System Settings icons: correct WITHOUT acceleration, yellow with it (also with System Settings off
  the GPU list) -> WindowServer's GPU path. Lead (JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe): Tahoe's icons use the
  10-bit extended-range layer format w30r / BGR10_XR, Apple-silicon only. No "pixel format ... NOT carried" in
  nvmtl-ws.log, so WindowServer does not ask for BGR10_XR textures - maybe it samples w30r surfaces as BGRA8.
  b186 (plugin): syslog "NVMTL SURFFMT" once per (IOSurface fourcc, Metal format) pair per process.
  Result (Tahoe, 10-26): WindowServer samples 'LA08' (luminance+alpha) 240x240 icon surfaces as RG8Unorm with no
  swizzle -> (L, A, 0): white = yellow. b187 (plugin): luminance IOSurfaces get the implied swizzle (LA -> R,R,R,G;
  L -> R,R,R,1), syslog "NVMTL LUMSWZ"; off with /Library/GPUBundles/nvmtl-nolumswizzle.
  b187 test (10-26): swizzle applied to the IOSurface texture, icons unchanged -> WindowServer reads the pixels via
  something else. b188 (plugin): the luminance reading follows views and blit copies of the texture (_lum flag,
  nvmtl_texture_lum_apply); binds of luminance textures are logged with whether the swizzled view was used.
  b188 test (10-26): the LA08 texture IS bound through the swizzled view, icons unchanged. Screenshot: the coloured
  icon tile is right, the glyph is a solid yellow square - not a yellow-tinted glyph. b189 diagnostic: syslog
  "NVMTL LUMDATA" (surface statistics + 20x10 alpha picture) and the fragment function / blend / target format at bind.

## b190 (10-26) Tahoe icons
  The yellow glyphs come from iconservicesagent (Tahoe IconRendering: stitched / visible-function Core Image graphs,
  custom_effect_fragment, [[color(n)]] reads), not from WindowServer - the LA08 surface was a drop shadow (b189 LUMDATA:
  L=0 everywhere, round alpha). With "- iconservicesagent" in nvmtl-allow.txt and the icon cache cleared, every icon is
  right (user 10-26). autoarm now does that on Darwin >= 25. The IconRendering shader path stays open.
  b190 speed / heat (docs/SPEEDUP.md S1): the per-commit "NullMoth commit:" printk (two per display commit, ~10k lines
  in 5 min) only before WindowServer owns the screen or on failure; CI builds the plugin quiet (-DNVMTL_RELEASE=1) as
  out/NVMTLDriver.bundle and verbose as out/debug/NVMTLDriver.bundle; tools/macos-helpers/nmx-speed.sh measures it.
  Tahoe USB fixed in the user's EFI (USBMap with Tahoe's usb-port-number / usb-port-type keys); Tahoe sound with
  SimpleHDA (Apple removed AppleHDA in Tahoe) - EFI-side, not driver.
  b191 (S3 icon test): plugin captures small render passes in iconservicesagent when /Library/GPUBundles/nvmtl-icontest
  exists (readback after the command buffer completes; files in its cache dir, syslog "NVMTL ICONTEST" with the
  fragment functions); tools/macos-helpers/nmx-icontest.sh on|collect|off; autoarm leaves iconservicesagent alone
  while the test file exists. nmx-speed.sh writes a new file per run.
  b192 (10-27): Tahoe hung in "commit tail: SAGV post-plane" (nullmoth-last-step 139) - first on the desktop, then on
  every boot after the boot guard was cleared: the PCODE request that relaxes the memory QGV points never returned.
  patch_i915_nosagv.py: intel_has_sagv() false unless boot-arg nmxsagv=1 (nmx_sagv_on() in nmx_kext.cpp), so i915 sends
  no SAGV / QGV PCODE requests - the state of a BIOS with SAGV disabled.
  b193 (10-27): the b192 icon capture (120 passes in iconservicesagent: path / accumulator / filter_distance /
  filter_blur / custom_effect / primitive_image pipelines) showed the icons drawn right on the GPU (FaceTime, Settings
  glyphs) and the user saw no yellow squares - so autoarm no longer denies iconservicesagent (it removes b190's
  "- iconservicesagent" once and clears the icon cache); /Library/GPUBundles/nvmtl-iconcpu forces the CPU fallback.
  nmx-icontest.sh off only stops the capture. nmx-speed.sh detects the quiet build with grep -a (strings needs the
  developer tools, not installed on Tahoe).
  b194 (10-27): b193 was wrong - the user's screen recording shows whole app icons (Activity Monitor, Photo Booth,
  Siri, Weather...) and the Settings glyphs as solid yellow squares with iconservicesagent on the GPU, although the
  captured render passes looked right (the capture reads our own image, not what the consumer sees). Default back to
  iconservicesagent off the GPU; nvmtl-icongpu opts in. Next lead: the hand-off of the finished icon (IOSurface /
  shared texture) from iconservicesagent to the app, not the drawing.
  Open (10-27, Tahoe only): at the hand-over the boot logo is redrawn once at ~(0.75, 0.7) of the screen, smaller,
  without the progress bar - as if centred in a ~3840x2400 screen (1.5x the 2560x1600 panel). Not our snapshot paint
  (-nmxnosnap is set) and not -nmxconsole. Lead: a size / scale property of NMXGPUFB that Tahoe reads differently from
  Sequoia; compare `ioreg -lw0 -c NMXGPUFB` on both. Cosmetic (~1 s).
  Tahoe logo shift: NMXGPUFB properties identical on both systems except the saved desktop mode (Sequoia 1600x1000,
  Tahoe 1680x1050); the user set Tahoe to 1600x1000 - the logo still jumps, so not the scaled mode. Parked (cosmetic).
  b195 (S3): the icon test also captures compute passes (every texture bound to a compute encoder, read back after
  the command buffer: comp-NNN-<fn>-t<i>-fmtF-WxH.raw) and logs every texture -getBytes in iconservicesagent
  ("NVMTL ICONTEST getBytes") - b191 saw correct render passes, so the yellow must come later: Core Image compute
  kernels or the CPU readback of the finished icon.
  b195 capture (10-27): 160 compute captures + 120 passes - all folder / document icons, all drawn right (no yellow);
  the 160 slots were used up by 16x16 glyphs. getBytes readbacks: 16x16 / 128x128 RGBA8 and 130x130 / 34x34
  RGB10A2 (fmt 90). b196: compute captures only >= 64 px, and every getBytes >= 64 px is saved (get-NNN-fmtF-WxH.raw)
  - the finished icon as the CPU receives it.
  b196 capture (10-28): 163 getBytes saved - 152 are 130x130 RGB10A2 maps (normal-map-like rainbow images of icon
  shapes); their flat areas read (R 1.0, G 0.5, B 0.5) where a normal map would hold (0.5, 0.5, 1.0) - red and blue
  look swapped. b197 experiment: /Library/GPUBundles/nvmtl-iconswap makes iconservicesagent store RGB10A2 as BGR10A2.
  b198 (boot speed + Tahoe logo jump, 10-28): nmx-boottime on Tahoe - WindowServer starts at 2.3 s (before NMXGPU at
  5.6 s, so a WindowServer restart stays necessary), opens our screen at 9.9 s, armed 12.8 s, restarted 14.7 s, helper
  signal 16.3 s, hand-over 20.4 s "no new frame in 4 s": the accelerated WindowServer's frames come by copy-forward,
  not flip_commit, so the go-on-flip wait always ran out. Wait 2 s now. The logo jump: hand-over output 1600x1000 on the
  2560x1600 panel (scaled desktop) showing buffer 0 = the first WindowServer's native-layout logo through the panel
  fitter -> logo at ~(0.8, 0.8). bridge_go clears buffer 0 (write only) when the desktop is scaled and no newer frame
  exists. Note: Tahoe's desktop runs a scaled 1x mode (1600x1000 / 1680x1050), not a 2x HiDPI one - check sharpness.
  b198 confirmed (10-28): the Tahoe boot logo no longer jumps.
  b197 swap test (10-28): no change - still yellow. Pattern in the screenshot: the icon BACKGROUND layers are right
  (App Store blue tile, Freeform with its scribble faintly visible), the FOREGROUND / glyph layer is a solid yellow
  fill (Settings glyphs, Preview, Magnifier, Boot Camp; App Store's glyph missing). Yellow = (1,1,0,1): what an RG16F
  distance-field texture (filter_distance passes) gives when sampled as colour with R,G >= 1 - suspect a wrong texture
  binding or a wrong format for the glyph layer's composite pass. b199: render-pass capture limited to >= 64 px (300).
  b199 capture (10-28): FOUND the yellow pass - pass 208/227 (132x132 RGBA16F) drawn by primitive_image_fragment alone
  comes out (1,1,0,1) everywhere; the icon's chain continues with it (yellow 160x160 BGRA blur passes, yellow 128x128
  squircle = the glyph layer). The image drawn there reads yellow everywhere - like a RG16F distance field (the
  filter_distance passes right before it) instead of the glyph picture: a wrong texture reaching the shader. Bindless
  heap lookups are keyed by the VkImageView pointer (nvmtl_bl_slot) - a reused view address could map to a stale slot.
  b200 experiment: /Library/GPUBundles/nvmtl-iconnobindless turns bindless off in iconservicesagent only.
  b200 no-bindless test (10-28): iconservicesagent then draws nothing ("?" placeholders) - inconclusive, reverted.
  b201 (10-28): ROOT CAUSE candidate - MTLHeap placed textures (nvmtlPlacedTexture, new and recycled) never applied
  the descriptor swizzle; every other creation path calls nvmtl_texture_desc_swizzle. A luminance+alpha glyph layer
  made on a heap with swizzle (L,L,L,A) sampled as (L,A,0,1) = (1,1,0,1) for a white glyph: the yellow squares.
  Fixed for every app (also Sequoia). Test: /Library/GPUBundles/nvmtl-icongpu puts iconservicesagent on the GPU.
  b201 test (10-28): still yellow. b202 icon test logs every texture creation in iconservicesagent (format, size,
  descriptor swizzle, heap-placed / IOSurface) and every gpuResourceID lookup (format, swizzle state) - to see which
  texture the yellow pass samples and whether it carries a swizzle we drop.
  Icons on the GPU PARKED by the user (10-28): iconservicesagent stays off the GPU (CPU icons, correct). Open lead: the symbol layer's shape mask reads fully covered (Default/Dark yellow, Tinted blue, Clear white squares); b202 capture logs texture formats/swizzles when resumed.
  b203 (10-28): IrisXe Manager - manager/IrisXeManager.swift (SwiftUI window) + manager/irisxe-manager.sh (status,
  scan, install, repair, armnow; installs with backups) + manager/build.sh; CI puts "IrisXe Manager.app" in out/.
  b204: app icon options manager/icons/icon-A/B/C.png (original drawings: a chip with an arrow), chosen by manager/icons/CHOSEN; build.sh makes AppIcon.icns.
  b205: manager status: an ARM/RESTART step while nmx-autoarm.sh is still running (its 60 s check after the WindowServer restart) is shown as "none yet - acceleration check running", not ACCELERATION LOCKED.
  b206: tools/macos-helpers/nmx-hidpi.sh (test): the driver's smaller desktop sizes are 1x modes stretched by the panel fitter (soft text). With WindowServer accelerated, HiDPI can be drawn at 2x and scaled on the GPU: a display override (scale-resolutions, HiDPI flag) for the panel's DisplayVendorID/ProductID adds looks-like 1280x800 (exactly the panel's pixels), 1440x900, 1600x1000, 1680x1050. No driver change; to check: Displays offers them, text sharp, GPU/heat cost of the scaled ones.
  b207 (icons, Tahoe): b206 capture (10-28) - the shape masks are right (R8 passes of Terminal, Settings, Music, Activity Monitor's pulse line), and most icons compose correctly on the GPU. The broken layers are single primitive_image_fragment+blend draws that fill the whole target with the style colour (Activity Monitor pass 177: 128x128 RGBA16F solid yellow 1,1,0,1; pass 195: an R8 target drawn by the same shader comes out all white; FaceTime pass 13 the same) - the image that draw samples reads as opaque (alpha 1), as an R8 texture sampled without its A8 / swizzle mapping would. New: every fragment texture bound in icon-test mode is logged ("ICONTEST ftex": encoder number, view and base format, swizzle, A8, IOSurface) and saved as src-<enc>-i<idx>-fmt<F>-<W>x<H>.raw; pass lines now carry the encoder number, so each broken pass can be matched with what it sampled.
  b208 (icons): the b207 capture (10-28) filled its 300 slots with icons that came out right (FaceTime, Terminal, QuickTime, Settings, Instruments); the broken ones appeared only in the getBytes read-backs: 130x130 RGB10A2 'normal' images of one flat colour (1, 0.5, 0.5) where the good ones show a bevel. Capture now logs every pass (4000) but saves pixels only for passes that ran primitive_image_fragment (500), together with the textures that encoder's fragment stage had bound (src-e<enc>-i<idx>-..., 800).
  b209 (10-28): a cold start on Tahoe (off ~3 h) hung at the Apple logo. Boot report: that boot logged nothing (stuck before the log was written); the guard boot after it said the attach never returned; nullmoth-last-step was already overwritten by the next good boot, so the stopping step is unknown. Now the guard boot copies the hung boot's last step to nvram nullmoth-hang-step and logs it; IrisXe Manager's Clear Safety Locks writes it to /Library/Application Support/IrisXe/hangs.txt before clearing, and status shows the last one.
  b210 (icons, Tahoe): b208 capture (10-28) traced a broken icon back: its shape layer is drawn by custom_effect_fragment (Core Image effects linked in as visible functions) and comes out opaque everywhere, so the distance field saturates, the bevel normal image is flat and the layer fills the squircle with the style colour (Brave, Terminal, Settings, Safari, Books - no custom effect in the shape - come out right). The only failures the plugin logged in iconservicesagent are the expected 'visible function reference' re-translates that defer these functions to linking. Found in nvmtl_link_stage: the function constants of every linked function were merged with the caller's into ONE table by index (caller wins, first linked function wins), but every Metal function has its own constant index space - a linked effect whose constant shares an index runs with someone else's value. Now each linked function's own constants are folded into its own AIR first (new translator export nvmtl_specialize_air, AIR -> AIR), and only the caller's constants go into the final translation. Logs 'NullMoth: link: ... folded' (first 40). Needs the new shader translator as well as the plugin.
  b211 (icons, Tahoe): b210 did not change the icons and logged no 'folded' line (the linked effects have no constants of their own; b210 stays, it is correct and harmless). The b210 capture's plugin failures in iconservicesagent: 16 'buffer texture: refused on memory types - image types 0x7 of 0x3f'. A texture over an MTLBuffer (newTextureWithDescriptor:offset:bytesPerRow:) had to accept EVERY memory type of the device; Iris Xe images take 0x7 of 0x3f, so all of them were refused and returned nil - the icon renderer's CPU-written inputs never reached the GPU and its shaders sampled whatever was bound before (the stale full-square mask of b208). Now the image must accept only the memory types this driver has allocated from (every vkAllocateMemory records its type); still refused if the buffer could be in a type the image refuses. Logs 'NullMoth: buffer texture: ... accepted ... (b211)' (first 3).
  b212 (10-29): b211 CONFIRMED on Tahoe - no yellow icons with iconservicesagent on the GPU (System Settings, Applications). Autoarm v12: GPU icons are the default when the installed plugin has the b211 fix (its binary contains 'b211; refused before'); an older plugin, or /Library/GPUBundles/nvmtl-iconcpu, keeps the CPU fallback. The parked icon item is closed.
  b213 (10-29): Sequoia still logged both 'NullMoth commit:' lines on every commit (~7/s, all ret=0): nmx_steps_quiet was set only in NMXGPUFB::enableController, which the accelerated WindowServer on Sequoia never calls. Now the boot screen hand-over (bridge_go, reached on every boot that shows the desktop) also switches the per-commit lines off (nmx_steps_go_quiet; -nmxsteps keeps them). Failed commits are still logged.
  b213 (cont.): the real cause on Sequoia: it was still RUNNING the driver from before b190 (no 'SAGV / QGV' line at boot) although IrisXe Manager had put the new NMXGPU.kext in /Library/Extensions - the auxiliary kext collection was never rebuilt. The manager's install now runs kmutil load -p on each changed kext (rebuilds only the auxiliary collection; --update-all needs a KDK); NMXGPU logs 'driver built <date time>' at attach so the loaded build can be checked.
  b214 (speed, 10-29): Tahoe idle now 5.11 W average (was 12-13 W on 10-26) - with icons on the GPU and quiet logging Tahoe matches Sequoia. Shader caches checked on Tahoe: /Library/GPUBundles/nvmtl 62 MB, user nvmtl 16 MB, user Mesa cache 2.8 MB, iconservicesagent Mesa cache 7.4 MB - but WindowServer's Mesa cache folder is EMPTY (Mesa turns its disk cache off when real and effective user differ; WindowServer is uid 0 / euid 88), so WindowServer compiled every pipeline again after every boot. New: a VkPipelineCache per process kept on disk (<DARWIN_USER_CACHE_DIR>/nvmtl-pcache/<process>.bin), loaded at device creation, written 5 s after new pipelines and at exit; used for every graphics and compute pipeline. Off: /Library/GPUBundles/nvmtl-nopcache.
  b215 (brightness, 10-29 video): after boot, or when the screen came back after dimming / display sleep, the panel was at full brightness while the slider showed the low level (the next brightness key fixed it). i915 lights the panel at its OWN backlight level whenever it enables it (hand-over mode set, DPMS on, resume) and never learns ours - NMXBacklight writes the PWM duty register directly. NMXBacklight now checks every 2 s while the panel is on: duty or frequency not ours -> written back, first 6 logged ('i915 relit it at its own level'). -nmxnoblwatch turns the watch off.
  b216 (boot hang, 10-29 16:29, Tahoe): b209's record worked - the guard boot logged 'the hung boot stopped after step "07 commit tail: waiting for the commit fence"' (first mode set, #1, at attach): inside intel_atomic_commit_fence_wait -> dma_fence_wait_timeout on the boot framebuffer's plane fence. It has a timeout (i915_fence_timeout), yet the Mac sat at the logo ~90 s. patch_i915_fencewait.py (all-i915.sh, after the commit trace): the wait polls dma_fence_is_signaled() (non-blocking, asks the fence itself) every 1 ms, at most 1500 ms per fence; unsignaled -> logged 'plane fence NOT signalled after N ms - committing anyway', slower than 100 ms -> logged. Tested against the pinned drm-kmod intel_display.c (applies, idempotent).
  b217 (brightness again, 10-29 20:29 / 20:44 log): at each wake 'backlight: on at 10%' was followed 0.1-0.2 s later by 'backlight: 100%', and after boot by a row of 100% sets, while the slider kept the user's level - macOS itself asked for 100, so b215's watch (which restores OUR level) could not help. NMXBacklight applied both display parameters, brightness and linear-brightness, as the panel level; the linear writes come from the display's power / dimming handling. Now only brightness sets the panel; linear-brightness writes are ignored and logged (first 12); applied levels from linear say '[linear-brightness]' (only with boot-arg nmxbllinear=1, the old behaviour).
  b218 (boot hang #3, 10-29 21:06, Tahoe, driver b217): the guard boot recorded 'the hung boot stopped after step "50 commit tail: hw_done"' - the b216 fence fix worked (the boot got 43 steps further), and it hung somewhere else in the same first mode set. The two hangs on 10-29 stopped at unrelated steps; common to both: the auto-arm helper started the same second as NMXGPU's attach and wrote NVRAM (nmx-autoarm-step 'wait: started') while the driver wrote its breadcrumb + NVRAM sync at every step. Suspect: firmware variable writes from two places at once. Autoarm v13 writes NVRAM only for ARM / RESTART / DONE / SKIP / STOP (the guard and report need those; the wait phase only polls). Also found: IrisXe Manager installs the autoarm that is BUNDLED in the app - the copy in /Applications was b204's, so 'install autoarm' kept installing v11 (why Tahoe stayed on CPU icons). The app itself has to be replaced from the new build.
  b219 (brightness, 10-30): b217 made the slider do nothing - the panel stayed at 100%. On this Mac the slider and keys write linear-brightness; the 100% writes at wake / after boot come on brightness (the opposite of b217's guess). Now linear-brightness always sets the panel; brightness sets it only until the first linear write of the boot, and a 100% on brightness before any linear write is ignored for 120 s after the display came up (boot burst). Ignored writes logged (first 12). nmxbllinear=1: both always applied.
  b220 (10-30): IrisXe Manager install stopped after the first kext - 'CHANGED_KEXTS: unbound variable' (set -u, b213's list was never initialised). Initialised per install; tested install_kext under set -u.
  b221 (manager, 10-30): (1) 'Choose Mesa Build...' - a second folder for the Mesa ANV + Metal plugin artifact; only the full Metal plugin is taken from it (install passes fullplugin@<folder>), installed before the quiet plugin update when both are ticked. (2) autoarm is taken from the CHOSEN build (the helpers inside its out/IrisXe Manager.app), the app's own copy only as a fallback - an old app no longer installs an old helper.
  b222 (logo jump again, Tahoe 10-30 06:43): the hand-over committed buffer 0 at the NATIVE size (old boot logo in the 2560x1600 layout) and 0.16 s later WindowServer's desktop size (1600x1000) was committed - the panel fitter enlarged the logo's top-left part. b198 cleared buffer 0 only when the size was already scaled at the hand-over. nmx_fb_set_size now clears buffer 0 to black before every desktop size commit after the hand-over (a picture in the old layout is wrong after a size change; WindowServer redraws anyway).
  b223 (manager, 10-30): the two downloads are named and checked - 'NMXGPU build' (Choose NMXGPU Build...) must contain NMXGPU.kext, 'Mesa ANV build' (Choose Mesa ANV Build...) must contain the full Metal plugin; choosing one kind with the other button is refused with a note saying which button it belongs to. Rows show the folder name (full path on hover).
