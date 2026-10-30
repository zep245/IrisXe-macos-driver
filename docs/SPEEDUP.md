# Speed and heat - design (2026-10-26)

Goal: a desktop that feels quick and stays cool on **Tahoe and Sequoia**: icons appear at once in Applications and
System Settings, apps open without a pause, and the idle laptop draws about what Sequoia drew after b170 (~5 W).

## What we know (measured 10-26)

| Observation | Where | Meaning |
|---|---|---|
| WindowServer used 0.08 s of CPU in an idle minute | Tahoe, `ps` | the composited desktop costs nothing at idle |
| PSR1 on after 300 frames, GPU 0-8 % busy at 300-500 MHz | Tahoe, nmx-power | display and GPU power saving work on Tahoe too |
| Idle power swings 4.6-32 W, average 12-13 W | Tahoe, nmx-power | short CPU bursts - not the GPU |
| 10381 `NullMoth commit:` kernel lines in 5 minutes | Tahoe log | two printk per display commit, ~35 lines/s at 60 Hz: logd work, all the time the screen changes |
| Every Metal process logs every plugin step (syslog + a file; WindowServer fsyncs each line) | plugin built without `NVMTL_RELEASE` | CPU, disk writes and frame-time spikes in every app that draws with Metal |
| iconservicesagent kept off the GPU (b190) | Tahoe | right icons, but drawn on the CPU: slow the first time each icon is drawn (~200 % CPU while the cache refills) |

## Work, in order

| Step | Work | Done when |
|---|---|---|
| S1 | **b190: quiet logging.** Kernel: the per-commit printk only before WindowServer owns the screen, or on a failed commit (`patch_i915_committrace.py`). Plugin: CI builds the quiet plugin (`-DNVMTL_RELEASE=1`, failures only) as `out/NVMTLDriver.bundle`; the verbose one stays as `out/debug/NVMTLDriver.bundle` for diagnosis | `nmx-speed.sh`: NMXGPU per-commit lines 0, plugin "QUIET", logd and WindowServer CPU lower than before while using the desktop |
| S2 | **Measure, both systems.** `tools/macos-helpers/nmx-speed.sh` (CPU seconds per process + log lines in a window), idle and while opening Applications / System Settings / scrolling Safari; `nmx-power.sh` on battery after Tahoe has settled (a day) | numbers for Sequoia and Tahoe, before and after S1 |
| S3 | **Icons on the GPU again (Tahoe).** iconservicesagent's IconRendering draws glyphs through stitched Core Image graphs, `custom_effect_fragment` with a linked visible function, and pipelines that read `[[color(n)]]` (framebuffer fetch). One of these produces the solid yellow square. Opt-in test mode: allow iconservicesagent for one run with an icon-pipeline dump (pipeline names, blend, the colour attachment after each draw for one 64x64 icon), compare with the CPU result, fix the translation | **Done (b211, 10-29):** buffer-backed textures were refused (memory-type check too strict), so the renderer's shapes never reached the GPU. Icons right on the GPU; autoarm v12 leaves iconservicesagent on the GPU |
| S4 | **Shader start-up cost.** Every app's first frames translate AIR -> SPIR-V and build Vulkan pipelines; the caches (spvcache, fc cache, refl cache, Mesa disk cache) hide it on the second launch. Check hit rates per app (verbose build) and pre-warm WindowServer's and Finder's pipelines at arm time | b214: WindowServer had no compiled-pipeline cache at all (Mesa disk cache off for uid 0 / euid 88) - own VkPipelineCache on disk per process. Check: `ls -la /private/var/folders/*/*/C/WindowServer/nvmtl-pcache` after a boot |
| S5 | **Compiler optimisation.** (b231: done for the quiet plugin) Plugin at `-O2` instead of `-O1` once S1 is confirmed stable (one change at a time) | same tests pass; nmx-speed CPU numbers equal or lower |
| S6 | **CPU bursts / heat.** After S1 and a settled Tahoe: identify the burst source (`nmx-speed.sh`, `top -o time`). Out of the driver's reach: Spotlight / photo analysis settle by themselves; CPU power management (XCPM data for MacBookPro16,2 on a Tiger Lake CPU) is an EFI question, CPUFriend only if idle stays well above Sequoia | Tahoe idle average within ~1 W of Sequoia |

## Risks

- **Quiet plugin hides a regression's first clue.** The verbose build is still produced; install it with
  `nmx-plugin-update.sh <artifact>/debug/NVMTLDriver.bundle` when a problem needs logs. Failures are still logged.
- **The quiet build has been used less.** Bugs specific to `NVMTL_RELEASE` existed before (the `access` macro,
  fixed). `nmx-plugin-update.sh` keeps the previous binary as `/Library/GPUBundles/NVMTLDriver.previous`.
