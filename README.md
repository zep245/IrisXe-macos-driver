# Intel Iris Xe graphics driver for macOS (experimental)

An experimental GPU driver that gives an 11th-gen **Intel Iris Xe (Tiger Lake, `8086:9A49`)** a working display and
**Metal acceleration on macOS Sequoia and Tahoe** (Hackintosh / OpenCore, x86_64). macOS has never supported this GPU: Apple's
last Intel graphics drivers stop at Ice Lake.

It is built on Intel's own open-source Linux display driver (**i915**, through FreeBSD's `drm-kmod`) running inside the
macOS kernel, Mesa's **ANV** Vulkan driver, and a Metal-to-Vulkan layer that macOS loads as its Metal driver.

This repository is a fork of [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver). The
kernel-port design, the Metal plugin and the shader translator come from NullMoth Systems; this fork turned the Intel
(i915) path into a working driver for Iris Xe on Sequoia. The original NVIDIA README is kept at
[`docs/README-NVIDIA.md`](docs/README-NVIDIA.md). The day-by-day engineering log is [`DESIGN.md`](DESIGN.md).

> **Experimental.** It runs daily on one machine (below), but it is a kernel driver for the GPU that drives your
> screen. A bad build can leave the screen black or panic the Mac. Keep a way back (see [Safety](#safety)).

## Tested machine

| | |
|---|---|
| Laptop | Xiaomi Mi Notebook Pro, Core i5-11300H, Iris Xe `8086:9A49`, 2560x1600 eDP panel |
| macOS | Sequoia 15.8.1 and Tahoe 26.7.1 (same laptop, two volumes), SMBIOS `MacBookPro16,2`, OpenCore |
| Desktop | 1680x1050 (scaled), accelerated |

Other Tiger Lake Iris Xe / UHD laptops may work; nothing else has been tested.

Sequoia and Tahoe use the same display driver, display helper and Metal plugin. Only the accelerator differs:
the artifact's `out/NMXAccel.kext` is for Sequoia, `out/tahoe/NMXAccel.kext` for Tahoe (IrisXe Manager picks the right one).

## What works

| Area | State |
|---|---|
| Display | Built-in panel driven by i915: mode set, hardware cursor, brightness (slider and keys), no flicker |
| Boot | Apple logo kept on screen until the accelerated desktop is ready (no black gap, no second logo) |
| Acceleration | WindowServer composites with Metal on the Iris Xe; frames reach the panel by page flip at vblank |
| Metal apps | Through the Metal plugin, per app allow-list (`/Library/GPUBundles/nvmtl-allow.txt`); reports Metal 3 |
| Browsers | Brave (Skia Graphite) and Safari: text, scrolling, video |
| Camera apps | Photo Booth (incl. Effects), QuickTime, browser cameras |
| Power | GPU idles in RC6 (0 MHz when idle), clock governor 300-1300 MHz, panel self refresh (PSR1) once accelerated |
| Sleep / wake | Lid, Apple menu and display sleep; acceleration and PSR come back after wake |
| About This Mac | "Intel Iris Xe Graphics 1536 MB" (shared memory, as Apple shows for Intel GPUs) |

Measured on the tested machine, idle desktop on battery at 11 % brightness: about **10.7 W before PSR, 5.2 W with it**.

### Known issues

- Photo Booth: the delete badge on photos in the strip draws as a black square.
- After a restart the desktop shows ~6 s of black with the pointer before the lock screen: macOS's display server is
  restarted once, accelerated, because the driver loads after it starts.
- External displays (HDMI / USB-C): mirroring of the laptop screen only, not yet tested on hardware; no extended desktop.
- A Metal app that is not on the allow-list gets no GPU device.
- **Tahoe is slower and warmer than Sequoia** on the same laptop: the Liquid Glass interface needs much more GPU work
  per frame, and a new install indexes for days. System Settings -> Accessibility ->
  Display -> **Reduce transparency** removes most of the glass work.
- Tahoe's desktop is scaled 1x (not HiDPI) on the tested panel.

## Architecture

```
 Metal app / WindowServer
    |
    v
 NVMTLDriver.bundle   Metal device on top of Vulkan       plugin/        (/Library/GPUBundles)
    |  shaders: Apple AIR -> SPIR-V                        translator/    (libnvmtl_translate.dylib)
    v
 Mesa ANV (Intel Vulkan) + libdrm for Darwin                gpu-port/patches/  (mesa-anv-build.yml)
    |  ioctls on /dev/dri/renderD*
 ---------------------------- user space / kernel ----------------------------
    |
    +- NMXAccel     IOAccelerator: Metal command submission, IOSurfaces, display pipe   kexts/NVRM/accel
    +- NMDRM        DRM device nodes for libdrm                                          kexts/NMDRM
    +- NMXGPU.kext  i915 + DRM core + LinuxKPI on xnu; NMXGPUFB framebuffer,             gpu-port/xkpi
                    page flips, cursor, backlight, power (RC6, PSR)
                         |
                         v
                    macOS kernel (IOKit, IOPCIFamily, IOGraphicsFamily)  ->  Iris Xe
```

| Layer | Origin |
|---|---|
| `i915`, DRM core, TTM | FreeBSD `drm-kmod`, compiled as-is (`NMX_NO_XE=1`: the newer `xe` driver is left out) |
| LinuxKPI | FreeBSD `freebsd-src` |
| `gpu-port/xkpi/bsdcompat/`, `override/`, `xnu/` | The FreeBSD kernel API on xnu, the port's replacements, the IOKit glue (NMXGPU, NMXGPUFB) |
| `plugin/`, `translator/`, `kexts/` | Metal plugin, shader translator, accelerator and DRM nodes |
| `tools/macos-helpers/` | Install, update and diagnostic scripts (`nmx-*.sh`) |

## Building

GitHub Actions (the machines Apple provides there have the right Xcode):

1. **Actions -> "NMXGPU Intel (i915) kext build" -> Run workflow** (`nmxgpu-intel-build.yml`). It builds `NMXGPU.kext`,
   the Metal plugin binary, the shader translator (`--target x86_64-apple-darwin`) and the Metal test tools. Download the
   `nmxgpu-*` artifact.
2. **"Mesa ANV + Metal plugin"** (`mesa-anv-build.yml`) builds Mesa ANV and the full `NVMTLDriver.bundle` including
   `air-runtime/` (the shader reader). Needed once, and again only when Mesa changes.

Locally on a Mac with Xcode: `NMX_MIN_OS=14.0 gpu-port/xkpi/ci-build-intel.sh ~/nmx-build`.

Pinned upstream versions (bumping them means fixing the port):
`freebsd/drm-kmod` `c16de0f9fa84f70b9551f69fde837a7f3a73c0e9`,
`freebsd/freebsd-src` `b42793d4bcdf73475b77a26c9dbacd45f8876621`.

## Installing

**IrisXe Manager** (`manager/`, built into the artifact as `out/IrisXe Manager.app`) does the steps below from a window:
pick the unzipped build folder, tick the parts (display driver, accelerator - the right one for the running macOS is
picked -, display helper, Metal plugin update or full plugin, shader translator, acceleration at every boot) and press
Install; the old files are kept in `/Library/Application Support/IrisXe/backup/`. "Clear Safety Locks" removes the
NVRAM guards after a hang, "Turn Acceleration On Now" arms without a restart. The same actions by hand:
`manager/irisxe-manager.sh status | scan <folder> | install <folder> <parts> | repair | armnow`.

The app is not signed by a registered Apple developer, so macOS warns the first time. Copy it to Applications, then:
`xattr -dr com.apple.quarantine "/Applications/IrisXe Manager.app"`. In the first minute after login the "locks" line
reads "acceleration check running" - the helper is still watching the restarted WindowServer; that is not a lock.

The artifact's `out/NVMTLDriver.bundle` is the **quiet** plugin (logs failures only, less CPU); `out/debug/NVMTLDriver.bundle`
is the verbose one, for diagnosing a problem.


Requirements: OpenCore with SIP reduced and `SecureBootModel Disabled` (the kexts are unsigned), the Intel Tiger Lake
firmware (`i915/tgl_dmc_*`, `tgl_guc_*`, `tgl_huc_*`, see `app/Resources/nullmoth-gpu-firmware.txt`) in
`/Library/NullMoth/firmware/i915/`.

1. Copy `NMXGPU.kext` (and the accelerator kexts) to `/Library/Extensions`, approve them in System Settings ->
   Privacy & Security, and restart (macOS adds them to the auxiliary kext collection).
2. Metal plugin: first time, the full bundle from the Mesa build into `/Library/GPUBundles/`; after that only the binary:
   `sudo tools/macos-helpers/nmx-plugin-update.sh <new NVMTLDriver.bundle>` - it keeps `air-runtime/`. Shader
   translator: `tools/macos-helpers/nmx-xlate-update.sh`.
3. Acceleration at every boot: `sudo tools/macos-helpers/nmx-autoarm/install-autoarm.sh` (also installs the brightness
   slider sync).
4. Boot-args used on the tested machine:
   ```
   keepsyms=1 nmxintel=1 nmxaccel=1 -nmintelfboff nmxws=1 nmxcopyfwd=1 nmxautoarm=1 nmxmodern=1 nmxintelfb=1 -nmxnosnap
   ```
   (`nmxintel=1` opts the GPU in - without it the driver declines the device; `nmxws=1` + `nmxautoarm=1` turn on
   accelerated WindowServer at boot.)
5. OpenCore: `DeviceProperties` for `PciRoot(0x0)/Pci(0x2,0x0)`: `AAPL,ig-platform-id` = `0000499A`, `device-id` =
   `499A0000`, `AAPL,slot-name` = `built-in`; WhateverGreen is not used. Kernel -> Block `com.apple.iokit.IONDRVSupport`
   (Exclude) so the driver's framebuffer is display 0.

## Switches

Boot-args (OpenCore `boot-args`):

| Boot-arg | Effect |
|---|---|
| `-nvoff` | Park every NullMoth GPU driver for one boot (recovery) |
| `-nmxnopsr` | No panel self refresh |
| `-nmxnoext` | Never light external displays |
| `-nmxnohold` / `-nmxnodefer` | Do not hold the Apple logo until the accelerated desktop / commit the screen at once |
| `nmxgtawake=2` | Keep the GPU out of RC6 |
| `nmxautoarm=2` / `3` | Acceleration helper: dry run / arm without restarting WindowServer |
| `nmxsagv=1` | Let i915 control SAGV / memory clock points (off by default: its firmware request hung the boot on Tahoe) |

Files in `/Library/GPUBundles/` (read at run time, no rebuild):

| File | Effect |
|---|---|
| `nvmtl-allow.txt` | Apps allowed to use the GPU, one `!Name` per line |
| `nvmtl-ciflip` | Apps whose Core Image output is flipped (default: Photo Booth; an empty file turns it off) |
| `nvmtl-standincopy` | Old copy mode for small image uploads (causes stale text - diagnostic only) |
| `nmx-autoarm-slow` / `nmx-autoarm-fast` | Slower / faster acceleration helper timings |
| `nvmtl-iconcpu` | Tahoe: draw app icons on the CPU (the fallback before b211; slower) |
| `nvmtl-icontest` | Tahoe: icon test mode with captures (use `nmx-icontest.sh`) |
| `nvmtl-nolumswizzle` | Turn off the luminance swizzle for one-channel (LA08 / L008) surfaces |
| `nvmtl-nopcache` | No on-disk pipeline cache (b214; compiled GPU programs are kept per process in the user cache folder) |

## Diagnostics

| Script (`tools/macos-helpers/`) | Shows |
|---|---|
| `nmx-boottime.sh` | When each part of the boot happened (driver, screen, WindowServer, acceleration) |
| `nmx-power.sh` | Idle power from the battery, GPU RC6 / clock state |
| `nmx-wakecheck.sh` | What happened in the last sleeps and wakes |
| `nmx-display.sh` | Outputs the driver sees (panel, HDMI, DisplayPort), their modes, hot-plug events |
| `nmx-autoarm/nmx-autoarm-report.sh` | The acceleration helper's last boot |
| `nmx-speed.sh` | CPU time of WindowServer, logd, iconservicesagent and others over a minute, driver log lines, quiet / verbose plugin |
| `nmx-icontest.sh` | Tahoe icon test: `on`, `collect`, `off` |
| `nmx-hidpi.sh` | `on` / `off` / `status`: HiDPI (Retina) desktop sizes for the panel through a display override - sharper text and icons (test) |

Driver log: `log show --last boot --predicate 'eventMessage contains "NullMoth"'`.

## Safety

- Keep OpenCore on a USB stick with a known-good config, and know how to reset NVRAM.
- `-nvoff` boots without the GPU driver. A boot that hangs during the driver's start is detected the next time
  (NVRAM `nullmoth-gpu-attaching`) and the driver parks itself; clear it with `sudo nvram -d nullmoth-gpu-attaching`.
- If acceleration froze the desktop, the helper does not arm on the next boot (`nvram nmx-autoarm-step`); clear with
  `sudo nvram -d nmx-autoarm-step`.
- Unsigned kexts need reduced SIP and Secure Boot off. Understand what that costs your machine's security.

## Repository map

| Path | Contents |
|---|---|
| `gpu-port/xkpi/` | Kernel port: build scripts, `bsdcompat/`, `override/` (incl. the display bridge `nmx_fb_bridge.c`), `xnu/` (NMXGPU, NMXGPUFB) |
| `gpu-port/patches/` | Mesa (ANV) and libdrm patches for macOS |
| `kexts/NVRM/accel`, `kexts/NMDRM` | Accelerator, DRM nodes |
| `plugin/` | Metal driver (NVMTLDriver.bundle) |
| `translator/` | AIR -> SPIR-V shader translator |
| `tools/macos-helpers/` | Install, update and diagnostic scripts |
| `.github/workflows/` | `nmxgpu-intel-build.yml`, `mesa-anv-build.yml` |
| `DESIGN.md` | Engineering log: every build (b1xx), what was tried, what was found |
| `docs/` | Upstream NVIDIA documents and the original Sonoma bring-up notes |

## License

This fork keeps the upstream licensing; it does not change it.

- NullMoth's code (`plugin/`, `kexts/`, `build/`, `package/`) is **PolyForm Noncommercial 1.0.0** (`LICENSE`): personal,
  research, educational and non-profit use only. **Commercial use, including selling builds, is not allowed.**
- `translator/` is LGPL-3.0-or-later. Mesa changes are MIT.
- The kernel port compiles code from FreeBSD `drm-kmod` and `freebsd-src` at build time, and the NPU driver is
  GPL-2.0. Check the licenses of any source you redistribute. See `NOTICE`.
- GPU firmware belongs to Intel and is redistributed only under its own license.

## Credits

[NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver) for the driver and the AMD/Intel port;
the FreeBSD `drm-kmod` and LinuxKPI maintainers; Intel's i915 and Mesa ANV developers.
