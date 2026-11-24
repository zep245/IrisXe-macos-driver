# Iris Xe driver - build 274 (2026-11-06)

Changes since build 259. Tested on Sequoia 15.8.1; Tahoe builds are made from the same sources.

## New: sharp HiDPI (Retina) sizes

- System Settings -> Displays now offers sharp sizes like a MacBook Pro: **1280 x 800, 1440 x 900, 1600 x 1000 and
  1680 x 1050 (HiDPI)** on the 2560x1600 panel. macOS draws the desktop at 2x and the display engine shrinks it to the
  panel. The size you choose is kept across restarts.
- 1280 x 800 (HiDPI) is on by default. For the larger sizes:
  1. In the NMXGPU download's `out/` folder run `sudo ./nmx-hidpi.sh big`, then restart.
  2. System Settings -> Displays -> turn on **Show all resolutions** and pick a size marked HiDPI.
- Undo: `sudo ./nmx-hidpi.sh nobig` (back to 1280 x 800 HiDPI only), or `sudo nvram nmx-hidpi=0` (HiDPI off).

## Fixed

- The panel now shows as "Built-in Retina Display" with its real maker and model (read from its EDID) instead of an
  unknown display.

## Known issues

- System Settings marks 3360 x 2100 (1x, very small text) as "Default", and its short list also shows some large 1x
  sizes. macOS chooses these itself. Pick a HiDPI size once and macOS keeps it.

Support the work: [buymeacoffee.com/zep245](https://buymeacoffee.com/zep245)
