#!/bin/bash
# run-vtdec.sh <video file> [frames] - hardware video decoding, stage 3 (b243): VideoToolbox + the Iris Xe video engine.
# Put this folder's files (vtdectest, libixdec.dylib, libavcodec/libavutil ...) into a Mesa build's nmx-vulkan folder
# (the one where run-vkvideo.sh worked) and run it there, NOT with sudo. Changes nothing on the Mac.
# Writes vtdec-log.txt here and on the Desktop; the decoder's own log goes to /tmp/ixdec-<pid>.log.
cd "$(dirname "$0")" || exit 1
IN="${1:?usage: ./run-vtdec.sh <video file> [frames]}"; N="${2:-0}"
[ -f "$IN" ] || { echo "no such file: $IN"; exit 1; }
xattr -dr com.apple.quarantine . 2>/dev/null
[ -x ./vtdectest ] && [ -f ./libixdec.dylib ] && [ -f ./libvulkan_intel.dylib ] || { echo "vtdectest, libixdec.dylib and libvulkan_intel.dylib must all be in this folder"; exit 1; }
export DYLD_INSERT_LIBRARIES="$PWD/nmxmmap.dylib" DYLD_LIBRARY_PATH="$PWD"
export ANV_VIDEO_DECODE=1 ANV_DEBUG="${ANV_DEBUG:-video-decode}" MESA_SHADER_CACHE_DISABLE=1
touch /tmp/ixdec.log
{ echo "=== $(date)  macOS $(sw_vers -productVersion)  file: $(basename "$IN")"; ./vtdectest "$IN" "$N" 2>&1
  echo "--- decoder log"; cat $(ls -t /tmp/ixdec-*.log 2>/dev/null | head -1) 2>/dev/null | tail -25; } | tee vtdec-log.txt
rm -f /tmp/ixdec.log
cp vtdec-log.txt ~/Desktop/ 2>/dev/null
echo "(saved to $PWD/vtdec-log.txt and your Desktop)"
