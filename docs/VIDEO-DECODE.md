# Hardware video decoding - plan (b240, 2026-11-01)

Goal: videos (YouTube in Brave/Safari, QuickTime, IINA, ...) decoded by the Iris Xe's video engine instead of the CPU:
much less heat and battery while watching. Today every frame is decoded in software.

Route: Mesa ANV already drives the video engine on Linux through Vulkan Video (VK_KHR_video_decode_h264/h265/av1/vp9).
macOS apps decode through VideoToolbox, which loads hardware decoders as plugins. So: Vulkan Video underneath, a
VideoToolbox decoder on top.

| Stage | Work | Done when |
|---|---|---|
| 1 | **b240/b241 - done 11-01** (H.264, VP9, AV1 to 4096x4096; HEVC not offered by this ANV; the video engine answers): Mesa built with video codecs; `vkvideoprobe` (nmx-vulkan/run-vkvideo.sh): what the GPU offers, then one empty command on the video engine through NMXGPU | the probe shows a decode queue, H.264/HEVC capabilities, and the video engine answers |
| 2 | **b242 - done 11-01** (H.264 1080p: identical frames, 87 % less processor time): decode real frames with FFmpeg's Vulkan Video decoder (ffmpeg-vulkan-build.yml + run-ffvideo.sh): every frame compared with the CPU decode, then timed | a test clip decodes identically to the CPU, with less CPU time |
| 3a | **b243-b248 - done 11-02** (H.264 1080p through VideoToolbox: 299/299 pictures identical, processor 1.82 s vs 4.59 s): tools/video/ixdec_vt.c + vtdectest. A VideoToolbox decoder (VTRegisterVideoDecoder-style plugin, as nvenc did for encoding) that hands VideoToolbox's compressed samples to stage 2 and returns IOSurface-backed frames | vtdectest: identical pictures, less processor time |
| 3b | **b249:** the plugin loads libixdec in every GPU app on Intel (NVMTLDevice.m nvmtl_ixdec_register) on the plugin's own ANV, with ANV's video queue switched on (ANV_VIDEO_DECODE before the driver's first instance). libixdec asks the video engine which codecs it decodes and registers only those. Installed by IrisXe Manager's "video" part (from the ffmpeg-vulkan build) into the plugin bundle. Off: /Library/GPUBundles/nvmtl-novideo (all), nvmtl-novideo-h264 / -vp9 / -av1 (one codec) | Brave (allow-listed) plays YouTube with "Video Decode: Hardware accelerated" and lower CPU; the plugin log shows "ixdec: ... registered" and "session end" lines |
| 3b+ | **b250-b257:** Brave's GPU sandbox (b254: in-memory /dev/dri listing, b252: in-memory anonymous files) and AV1 without film grain (b257) | AV1 YouTube decodes in Brave |
| 3c | zero-copy output: hand VideoToolbox the decoded image's own IOSurface instead of copying it (2.9 ms per 1080p picture today) | copy time near 0 |
| 4 | HEVC, then AV1/VP9 (YouTube uses VP9/AV1 in most browsers); browsers' own decoders (Chromium uses VideoToolbox for H.264/HEVC) | YouTube in Brave plays with low CPU |

Risks: the kernel side never ran anything on the video engine (VCS) before stage 1 - an engine that does not answer
needs i915 work first; VideoToolbox's private decoder interface; sandboxed players (Safari's WebKit GPU process cannot
load our plugin, as found 11-01).
