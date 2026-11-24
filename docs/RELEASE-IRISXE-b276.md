# Iris Xe driver - build 276 (2026-11-10)

Changes since build 274. Tested on Sequoia 15.8.1; Tahoe builds are made from the same sources.

## Fixed

- **Brave 157 had no GPU acceleration at all** (brave://gpu: "Software only", "GPU access is disabled due to frequent
  crashes"), so pages and videos ran on the processor: a 1080p video used 21-24 W. Two causes:
  - Brave 157 starts all its helpers under one new name, "Brave Browser Helper (Aperitif)". The Metal plugin now
    recognises Brave's GPU process under that name through the existing `!Brave Browser Helper (GPU)` line in
    `/Library/GPUBundles/nvmtl-allow.txt`; Brave's other helpers stay without the GPU.
  - Brave's new GPU sandbox blocks the check whether the accelerator is armed, so the plugin turned Brave away. A
    blocked check no longer counts as "not armed".
  - Result: brave://gpu is fully hardware accelerated again with the sandbox on, AV1 and H.264 are decoded on the GPU,
    and the same video uses about 14.6 W.
  - After installing, quit Brave completely (Cmd+Q) and open it again.

## New

- The Metal plugin logs when an app is not in `nvmtl-allow.txt` (it was refused silently before).
- Trace switch for finding problems: `sudo touch /Library/GPUBundles/nvmtl-trace` writes every plugin message of every
  app (except WindowServer) to `/tmp/nmx-mtltrace.log`; `sudo rm /Library/GPUBundles/nvmtl-trace` turns it off.
- `nmx-powertest.sh` (in the NMXGPU download's `out/` folder): measures battery power and CPU at each HiDPI size, and
  with `video` while a video plays in Brave. Unplug the charger first.

## Known issues

- Video still uses more power than on a MacBook (about 14.6 W for 1080p): the first time macOS needs a new drawing
  shader for video, translating it takes a lot of processor time in WindowServer.
- YouTube VP9 videos are still decoded on the processor. The browser extension enhanced-h264ify with only "Block VP9"
  ticked makes YouTube send AV1 or H.264 instead.

Support the work: [buymeacoffee.com/zep245](https://buymeacoffee.com/zep245)
