#!/bin/bash
# run-vkvideo.sh - hardware video decoding, stage 1 (b240): runs vkvideoprobe on the Iris Xe through this folder's Mesa ANV.
#   ./run-vkvideo.sh info   what the GPU offers (nothing runs on it)
#   ./run-vkvideo.sh        also one empty command on the video engine (save your work first)
# Changes nothing on the Mac. Do NOT use sudo (macOS drops DYLD_* variables for sudo).
# Writes vkvideoprobe-log.txt next to this script (and copies it to the Desktop).
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
export VK_DRIVER_FILES="$PWD/intel_icd.json" VK_ICD_FILENAMES="$PWD/intel_icd.json"
export DYLD_INSERT_LIBRARIES="$PWD/nmxmmap.dylib" DYLD_LIBRARY_PATH="$PWD"
# ANV keeps video behind switches in some Mesa versions: both spellings, harmless where unknown
export ANV_VIDEO_DECODE=1 ANV_DEBUG="${ANV_DEBUG:-video-decode}"
export MESA_SHADER_CACHE_DISABLE=1 VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error,warn}"
{
  echo "=== $(date)  macOS $(sw_vers -productVersion)"
  ./vkvideoprobe "$@" 2>&1
  echo "--- kernel lines from the last minute (video engine)"
  log show --last 1m --predicate 'sender == "NMXGPU"' 2>/dev/null | grep -iE "vcs|video|engine|hang|reset|fault|error" | tail -30
} | tee vkvideoprobe-log.txt
cp vkvideoprobe-log.txt ~/Desktop/ 2>/dev/null
echo "(saved to $PWD/vkvideoprobe-log.txt and your Desktop)"
