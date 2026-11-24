#!/bin/bash
# nmx-powertest.sh - power cost of each HiDPI size and of video playback, measured by the battery. Changes nothing
# lasting: it switches the desktop size for the test and puts the size you had back at the end.
#
#   1. unplug the charger, set the brightness you normally use, close busy apps
#   2. put this script next to cgmodes (the NMXGPU download's out/ folder) and run:  ./nmx-powertest.sh
#   3. do not touch the laptop while it measures (about 10 minutes); it asks once to start a video
#   ./nmx-powertest.sh video   only the video test (appends to the same file)
#
# Writes ~/Desktop/nmx-powertest.txt.
DIR=$(cd "$(dirname "$0")" && pwd)
CG="$DIR/cgmodes"
OUT="$HOME/Desktop/nmx-powertest.txt"
SECS=${SECS:-120}
[ -x "$CG" ] || { chmod +x "$CG" 2>/dev/null; xattr -d com.apple.quarantine "$CG" 2>/dev/null; }
[ -x "$CG" ] || { echo "cgmodes not found next to this script ($CG)"; exit 1; }

val() { ioreg -r -c IOPMPowerSource -l 2>/dev/null | sed -n "s/.*\"$1\" = \\(-*[0-9]*\\).*/\\1/p" | head -1; }
watts() {
    local a v; a=$(val Amperage); v=$(val Voltage)
    [ -n "$a" ] && [ -n "$v" ] || return 1
    [ ${#a} -ge 19 ] && a=$(echo "18446744073709551616 - $a" | bc)
    a=${a#-}
    awk -v a="$a" -v v="$v" 'BEGIN{ printf "%.2f", a * v / 1000000 }'
}
# CPU % of the processes whose name matches $1 (summed)
cpu() { ps -Ao %cpu,comm | awk -v p="$1" 'index($0, p) { s += $1 } END { printf "%.1f", s }'; }
# the CGS index of the HiDPI mode that looks like WxH
hidpi_n() { "$CG" 2>/dev/null | awk -v m="$1" '$3 == m && /density 2.0/ { print $2; exit }'; }

measure() {   # $1 = label
    local end n=0 ws=0 bg=0 sw=0 w last=""
    end=$(( $(date +%s) + SECS ))
    while [ "$(date +%s)" -lt "$end" ]; do
        w=$(watts)
        if [ -n "$w" ] && [ "$w" != "$last" ]; then sw=$(awk -v s="$sw" -v w="$w" 'BEGIN{print s + w}'); n=$((n + 1)); last=$w; fi
        ws=$(awk -v s="$ws" -v c="$(cpu WindowServer)" 'BEGIN{print s + c}')
        bg=$(awk -v s="$bg" -v c="$(cpu 'Brave Browser Helper (GPU)')" 'BEGIN{print s + c}')
        k=$((k + 1)); sleep 5
    done
    printf "%-34s %6s W   WindowServer %5s %% CPU   Brave GPU %5s %% CPU   (%d battery readings)\n" "$1" \
        "$(awk -v s="$sw" -v n="$n" 'BEGIN{ if (n) printf "%.2f", s / n; else print "-" }')" \
        "$(awk -v s="$ws" -v k="$k" 'BEGIN{ printf "%.1f", s / k }')" \
        "$(awk -v s="$bg" -v k="$k" 'BEGIN{ printf "%.1f", s / k }')" "$n"
}

ext=$(ioreg -r -c IOPMPowerSource -l 2>/dev/null | sed -n 's/.*"ExternalConnected" = \([A-Za-z]*\).*/\1/p' | head -1)
[ "$ext" = "Yes" ] && { echo "Unplug the charger first - the battery only shows the laptop's power when it is unplugged."; exit 1; }
now=$("$CG" 2>/dev/null | sed -n 's/.*now \([0-9]*x[0-9]*\) points.*/\1/p' | head -1)

MODE=${1:-all}
[ "$MODE" = video ] || {
echo "=== nmx-powertest $(date) - $(sw_vers -productVersion), ${SECS} s per test, now looks like ${now:-?}"
for m in 1280x800 1440x900 1680x1050; do
    n=$(hidpi_n "$m")
    if [ -z "$n" ]; then echo "$m HiDPI: not in the mode list - skipped"; continue; fi
    "$CG" set "$n" >/dev/null 2>&1; sleep 20
    k=0; measure "idle, looks like $m HiDPI"
done
n=$(hidpi_n "${now:-1440x900}"); [ -n "$n" ] && "$CG" set "$n" >/dev/null 2>&1
} | tee "$OUT"

echo
echo "Video test: open a 1080p YouTube video in Brave (do not start it yet), come back here and press Enter."
echo "Then within 20 s switch to Brave, start the video and make it full screen. Stay on the video and do not touch"
echo "the laptop for $((SECS + 20)) s - Brave stops drawing a video you cannot see, so it must stay on screen."
read -r _
echo "measuring starts in 20 s..."
sleep 20
{ k=0; measure "1080p video in Brave, looks like ${now:-?}"
  echo "--- busiest processes during the video (top, 2nd sample)"
  top -l 2 -s 3 -o cpu -n 12 -stats pid,cpu,command 2>/dev/null | awk '/^PID/ { n++ } n == 2' 
  echo "--- Brave processes"
  ps -Ao %cpu,comm | grep -i brave | sort -rn | head -8
  echo "--- GPU: video decoder in use?"
  log show --last 3m --style compact --predicate 'eventMessage contains "ixdec"' 2>/dev/null | tail -5
} | tee -a "$OUT"
echo
echo "Done - you can stop the video. Send me: $OUT"
