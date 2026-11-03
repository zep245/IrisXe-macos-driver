#!/bin/bash
# run-ffvideo.sh <video file> [frames] - hardware video decoding, stage 2 (b242).
# Decodes the first frames (default 300) of a video twice - on the processor and on the Iris Xe's video engine (Vulkan
# Video through this folder's Mesa) - and compares every frame. Then times both over the whole file.
# Run inside the nmx-vulkan folder (with ffmpeg copied in), NOT with sudo. Changes nothing on the Mac.
# Writes ffvideo-log.txt here and on the Desktop.
cd "$(dirname "$0")" || exit 1
IN="${1:?usage: ./run-ffvideo.sh <video file> [frames]}"; N="${2:-300}"
[ -f "$IN" ] || { echo "no such file: $IN"; exit 1; }
xattr -dr com.apple.quarantine . 2>/dev/null
export VK_DRIVER_FILES="$PWD/intel_icd.json" VK_ICD_FILENAMES="$PWD/intel_icd.json"
export DYLD_INSERT_LIBRARIES="$PWD/nmxmmap.dylib" DYLD_LIBRARY_PATH="$PWD"
export ANV_VIDEO_DECODE=1 ANV_DEBUG="${ANV_DEBUG:-video-decode}" MESA_SHADER_CACHE_DISABLE=1
FF=./ffmpeg; [ -x "$FF" ] || { echo "copy ffmpeg (from the ffmpeg-vulkan download) into this folder first"; exit 1; }
HW=(-init_hw_device "vulkan=vk:0" -hwaccel vulkan -hwaccel_device vk -hwaccel_output_format vulkan)
T=$(mktemp -d)
{
echo "=== $(date)  macOS $(sw_vers -productVersion)  file: $(basename "$IN")"
./ffprobe -hide_banner -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,nb_frames -of default=nw=1 "$IN"
echo "--- 1. processor decode, first $N frames"
"$FF" -hide_banner -v error -i "$IN" -an -frames:v "$N" -pix_fmt yuv420p -f framemd5 "$T/sw.md5"; echo "exit $?"
echo "--- 2. video engine decode (Vulkan Video), first $N frames"
"$FF" -hide_banner -v warning "${HW[@]}" -i "$IN" -an -frames:v "$N" -vf "hwdownload,format=nv12,format=yuv420p" -f framemd5 "$T/hw.md5" 2>&1 | tail -15; echo "exit ${PIPESTATUS[0]}"
sw=$(grep -vc '^#' "$T/sw.md5" 2>/dev/null); hw=$(grep -vc '^#' "$T/hw.md5" 2>/dev/null)
same=$(paste <(grep -v '^#' "$T/sw.md5" | awk -F, '{print $NF}') <(grep -v '^#' "$T/hw.md5" | awk -F, '{print $NF}') 2>/dev/null | awk '$1==$2' | wc -l | tr -d ' ')
echo "--- 3. compare: processor $sw frames, video engine $hw frames, identical $same"
if [ -n "$hw" ] && [ "$hw" -gt 0 ] && [ "$same" = "$sw" ] && [ "$sw" = "$hw" ]; then echo "RESULT: every frame identical - the video engine decodes this file correctly"
elif [ -n "$hw" ] && [ "$hw" -gt 0 ]; then echo "RESULT: the video engine decoded frames, but $(( sw - same )) differ from the processor's"
else echo "RESULT: the video engine decoded nothing"; fi
echo "--- 4. speed over the whole file (user = processor time, real = clock)"
# (b243: -benchmark prints at info level - b242 ran it with -v error and printed nothing)
echo "processor:";    "$FF" -hide_banner -v info -nostats -benchmark -i "$IN" -an -f null - 2>&1 | grep -E "^bench"
echo "video engine:"; "$FF" -hide_banner -v info -nostats -benchmark "${HW[@]}" -i "$IN" -an -f null - 2>&1 | grep -E "^bench|rror" | tail -5
} 2>&1 | tee ffvideo-log.txt
cp ffvideo-log.txt ~/Desktop/ 2>/dev/null
rm -rf "$T"
echo "(saved to $PWD/ffvideo-log.txt and your Desktop)"
