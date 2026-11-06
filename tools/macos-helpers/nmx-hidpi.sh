#!/bin/bash
# nmx-hidpi.sh status|on|off - sharper text and icons: HiDPI ("Retina") desktop sizes for the built-in panel (b206).
#
#   sudo ./nmx-hidpi.sh status   shows the panel's display IDs, the mode now in use and whether the override is there
#   sudo ./nmx-hidpi.sh on       adds HiDPI sizes for the panel; restart, then System Settings -> Displays
#   sudo ./nmx-hidpi.sh off      removes them again; restart
#   sudo ./nmx-hidpi.sh on-noiog b273: as 'on' without IOGFlags (only if 'on' lost sizes)
#   sudo ./nmx-hidpi.sh big      b265: also the larger sharp sizes (HiDPI step 2, test); nobig turns that off
#
# Why: the driver's smaller desktop sizes (1920, 1680, 1600, 1440, 1280 wide) are real 1x modes - macOS draws the
# desktop that small and the display engine stretches it to the 2560x1600 panel, so every pixel becomes ~1.5 pixels and
# text is soft. With acceleration on, WindowServer can instead draw every point as 2x2 pixels ("HiDPI", as on a real
# MacBook) and scale that to the panel on the GPU. "1280 x 800 HiDPI" is exactly the panel's pixels (sharpest, no
# scaling); 1440x900 / 1600x1000 / 1680x1050 HiDPI give more space and are scaled down on the GPU (more GPU work).
#
# How: a display override file (scale-resolutions), the same mechanism macOS uses for Apple's own displays, in
# /Library/Displays/Contents/Resources/Overrides/DisplayVendorID-<v>/DisplayProductID-<p>. Nothing in the driver changes.
# If the screen is wrong after the restart: boot the other macOS (or Recovery) and delete that file, or run 'off'.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
OV=/Library/Displays/Contents/Resources/Overrides

# the built-in panel's IDs as IODisplay sees them (decimal in ioreg; the folder names use hex without leading zeros)
ids() {
    local io; io=$(ioreg -lw0 -r -c IODisplayConnect 2>/dev/null)
    VID=$(awk -F'= ' '/"DisplayVendorID"/{print $2; exit}' <<<"$io")
    PID=$(awk -F'= ' '/"DisplayProductID"/{print $2; exit}' <<<"$io")
    if [ -z "$VID" ] || [ -z "$PID" ]; then echo "ERROR: no display IDs found (ioreg IODisplayConnect)"; exit 1; fi
    VHEX=$(printf '%x' "$VID"); PHEX=$(printf '%x' "$PID")
    DIR="$OV/DisplayVendorID-$VHEX"; FILE="$DIR/DisplayProductID-$PHEX"
}

# one scale-resolutions entry: width, height, flags, 0x00200000 (big-endian, as in Apple's own override files);
# flags 1 = HiDPI (the width/height given are the pixel size, 2x the "looks like" size)
entry() {
    local w=$1 h=$2 f=$3
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((w>>24&255)) $((w>>16&255)) $((w>>8&255)) $((w&255)))"
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((h>>24&255)) $((h>>16&255)) $((h>>8&255)) $((h&255)))"
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((f>>24&255)) $((f>>16&255)) $((f>>8&255)) $((f&255)))"
    printf '\x00\x20\x00\x00'
}
b64() { entry "$@" | base64 | tr -d '\n'; }
# b273: entries as in Apple's file for its 13" 2560x1600 MacBook Pro panel (DisplayVendorID-610/DisplayProductID-a048):
# a HiDPI size is its pixel size only (8 bytes, 2x the "looks like" size); a 1x size has flags 1 (12 bytes)
be32() { printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($1>>24&255)) $(($1>>16&255)) $(($1>>8&255)) $(($1&255)))"; }
hidpi64() { { be32 "$1"; be32 "$2"; } | base64 | tr -d '\n'; }
onex64() { { be32 "$1"; be32 "$2"; be32 1; } | base64 | tr -d '\n'; }

case "${1:-status}" in
status)
    ids
    echo "panel IDs: vendor $VID (0x$VHEX), product $PID (0x$PHEX)"
    echo "override:  $([ -f "$FILE" ] && echo "ON  ($FILE)" || echo "off")"
    echo "step 2:    $(nvram nmx-hidpi2 2>/dev/null | cut -f2- | grep -q 1 && echo "on (nmx-hidpi2=1)" || echo off)"
    echo "driver:    nmxhidpi $(ioreg -lw0 -r -c NMXGPUFB 2>/dev/null | awk -F'= ' '/"NMXHiDPI"/{print ($2=="Yes")?"on":"off"; exit}'), panel $(ioreg -lw0 -r -c NMXGPUFB 2>/dev/null | awk -F'= ' '/"NMXPanelWidth"/{w=$2} /"NMXPanelHeight"/{h=$2} END{print w"x"h}')"
    echo "--- what macOS shows now"
    system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Resolution|UI Looks like|Retina|Main Display|Display Type' | sed 's/^ */  /'
    ;;
on|on-noiog)
    ids
    mkdir -p "$DIR"
    reg=$(ioreg -lw0 -r -c NMXGPUFB 2>/dev/null)
    NW=$(awk -F'= ' '/"NMXPanelWidth"/{print $2; exit}' <<<"$reg");  NH=$(awk -F'= ' '/"NMXPanelHeight"/{print $2; exit}' <<<"$reg")
    BW=$(awk -F'= ' '/"NMXBufferWidth"/{print $2; exit}' <<<"$reg"); BH=$(awk -F'= ' '/"NMXBufferHeight"/{print $2; exit}' <<<"$reg")
    [ -n "$NW" ] && [ -n "$NH" ] || { NW=2560; NH=1600; }
    [ -n "$BW" ] && [ -n "$BH" ] || { BW=$NW; BH=$NH; }
    {
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
    echo '<plist version="1.0"><dict>'
    echo "  <key>DisplayProductName</key><string>Built-in Retina Display</string>"
    echo "  <key>DisplayVendorID</key><integer>$VID</integer>"
    echo "  <key>DisplayProductID</key><integer>$PID</integer>"
    # b273: Apple's own entries for its 13" MacBook Pro panel. IOGFlags 4: only these sizes and the driver's, none
    # made up by macOS. target-default-ppmm: the default size is the one with about 10.07 pixels per mm of panel
    # (looks like 1440x900 on a 30 cm wide 2560x1600 panel, as on the MacBook Pro)
    [ "$1" = on ] && echo "  <key>IOGFlags</key><integer>4</integer>"
    echo "  <key>target-default-ppmm</key><real>10.0699301</real>"
    echo '  <key>scale-resolutions</key><array>'
    # HiDPI: largest first, pixel size up to the driver's scan-out buffer (b265 step 2), the panel's own half size,
    # then smaller ones; same shape as the panel only
    seen=""
    for wh in 1920x1200 1680x1050 1600x1000 1440x900 "$((NW/2))x$((NH/2))" 1280x800 1152x720 1024x640; do
        w=${wh%x*}; h=${wh#*x}
        [ $((w * NH)) -eq $((h * NW)) ] && [ $((w*2)) -le "$BW" ] && [ $((h*2)) -le "$BH" ] || continue
        case " $seen " in *" $wh "*) continue ;; esac; seen="$seen $wh"
        echo "    <data>$(hidpi64 $((w*2)) $((h*2)))</data>"
    done
    # one plain (1x) size, as on the MacBook Pro
    for wh in 1920x1200 1680x1050; do
        w=${wh%x*}; h=${wh#*x}
        [ "$w" -lt "$NW" ] && [ $((w * NH)) -eq $((h * NW)) ] || continue
        echo "    <data>$(onex64 "$w" "$h")</data>"; break
    done
    echo '  </array>'
    echo '</dict></plist>'
    } > "$FILE"
    chmod 644 "$FILE"; chown root:wheel "$FILE"
    if ! plutil -lint "$FILE" >/dev/null; then echo "ERROR: the file did not come out right - removed"; rm -f "$FILE"; exit 1; fi
    nvram -d nmx-hidpi 2>/dev/null; echo "driver: HiDPI on (the default since b264)"
    echo "HiDPI sizes added: $FILE"
    plutil -p "$FILE"
    echo
    echo "Now:"
    echo "  1. Restart the Mac."
    echo "  2. System Settings -> Displays: pick a size (HiDPI ones are sharp)."
    echo "  3. If nothing changed, send me: sudo $0 status"
    ;;
big)
    # b265: HiDPI step 2 - larger sharp sizes (looks like 1440x900 / 1680x1050 on 2560x1600): the display driver keeps
    # scan-out buffers larger than the panel and shrinks the desktop with the display engine's plane scaler
    nvram nmx-hidpi2=1 && echo "HiDPI step 2 ON (nvram nmx-hidpi2=1)"
    echo "Now: 1. restart  2. System Settings -> Displays -> turn on 'Show all resolutions' -> pick a size marked HiDPI"
    echo "     (b272: the driver offers 1440x900, 1600x1000 and 1680x1050 HiDPI itself)"
    ;;
nobig)
    nvram -d nmx-hidpi2 2>/dev/null; echo "HiDPI step 2 off - restart"
    ;;
off)
    ids
    rm -f "$FILE"; rmdir "$DIR" 2>/dev/null
    nvram nmx-hidpi=0   # b264: HiDPI is on by default in the driver - 0 turns it off (the driver's own 1x sizes)
    echo "HiDPI override removed and the driver switched to its own 1x sizes (nvram nmx-hidpi=0) - restart the Mac"
    ;;
*) echo "usage: sudo $0 status|on|on-noiog|off|big|nobig"; exit 1 ;;
esac
