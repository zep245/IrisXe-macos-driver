# Iris Xe driver - build 274 (2026-11-06)

Changes since the brightness fixes (b230). Tested on Sequoia 15.8.1; Tahoe builds are made from the same sources.

## New: hardware video decoding

- **H.264 and AV1 are decoded on the Iris Xe video engine** for apps that use VideoToolbox. Tested in Brave with YouTube
  at 1080p: every picture identical to Apple's software decoder, about **72 % less processor time** than software
  decoding (1080p H.264 test file: 1.27 s of processor time against 4.56 s).
- The GPU writes each finished picture straight into the app's buffer, with no extra processor copy (b259).
- Install it with IrisXe Manager: **Choose Video Build...** (the `ffmpeg-vulkan` download), tick
  **Hardware video decoding**, then reopen your apps.
- VP9 is off by default: it hangs the Tiger Lake video engine in Mesa's driver. YouTube VP9 videos play on the
  processor as before (most YouTube videos come as AV1).
- Switches (create the file with `sudo touch`, remove it with `sudo rm`, then reopen apps):
  `/Library/GPUBundles/nvmtl-novideo` (all hardware decoding off), `nvmtl-novideo-h264` / `-av1` (one codec off),
  `nvmtl-video-nohostmap` (old copy path), `nvmtl-video-vp9` (VP9 on, for testing only).

## New: sharp HiDPI (Retina) sizes

- System Settings -> Displays now offers sharp sizes like a MacBook Pro: **1280 x 800, 1440 x 900, 1600 x 1000 and
  1680 x 1050 (HiDPI)** on the 2560x1600 panel (b261-b274). macOS draws the desktop at 2x and the display engine
  shrinks it to the panel. Your choice is kept across restarts.
- 1280 x 800 (HiDPI) is on by default. For the larger sizes: in the NMXGPU download's `out/` folder run
  `sudo ./nmx-hidpi.sh big`, restart, then System Settings -> Displays -> turn on **Show all resolutions** and pick a size
  marked HiDPI. `sudo ./nmx-hidpi.sh nobig` undoes it; `sudo nvram nmx-hidpi=0` turns HiDPI off.
- The panel now shows as "Built-in Retina Display" with its real maker and model (from its EDID); brightness works as
  before.

## Fixed

- **Brave's GPU acceleration with its GPU sandbox on** (b251-b254): sandboxed GPU processes (Brave, the icon service)
  could not start the Intel driver. The driver no longer writes anything to disk in a sandbox: the GPU device list and
  Mesa's temporary files are kept in memory.
- **Apple-logo boot hang** (b245): the boot progress record is no longer written to NVRAM at every step (about 220 slow
  firmware writes per boot). It is written at the first steps, every 8th step, and by a 1-second watchdog when a step
  stalls. `-nmxstepsall` restores the old behaviour.
- **Shorter black screen after boot**: about 1 s before the lock screen (was about 6 s).
- IrisXe Manager's full plugin install now also updates the Mesa driver files in `/Library/GPUBundles/nvmtl` and keeps
  an installed video decoder.

## Faster

- **Less processor work per frame in WindowServer** (b231-b239): the Metal plugin reuses about 68 % of its descriptor
  sets instead of writing a new one for nearly every draw, and the installed (quiet) plugin is built with -O2.
  WindowServer's time in the plugin per frame is about 20 % lower.
  `/Library/GPUBundles/nvmtl-nodynbuf` turns the new address-table path off.

## Install

1. Run the **NMXGPU**, **Mesa ANV** and **FFmpeg with Vulkan Video** builds on GitHub and download the three artifacts.
2. Open **IrisXe Manager** (in the NMXGPU download's `out/` folder) and choose the three downloads with
   **Choose NMXGPU Build...**, **Choose Mesa ANV Build...** and **Choose Video Build...**.
3. Tick the parts you want (the display driver for the boot-hang fix, the quiet plugin, and hardware video decoding),
   click **Install**, and restart.

## Known issues

- VP9 hardware decoding is off (see above).
- Safari's web pages do not get hardware video decoding (Safari's GPU process cannot load the Metal plugin).
- Films that use AV1 "film grain" play without the added grain.
- HiDPI: System Settings marks 3360 x 2100 (1x, very small text) as "Default" and its short list shows some large 1x
  sizes; macOS chooses these itself. Pick a HiDPI size once and macOS keeps it.

Support the work: [buymeacoffee.com/zep245](https://buymeacoffee.com/zep245)
