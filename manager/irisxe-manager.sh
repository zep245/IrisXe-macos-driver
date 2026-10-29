#!/bin/bash
# irisxe-manager.sh - the back end of "IrisXe Manager" (also usable by hand).
#
#   irisxe-manager.sh status                    what is installed / loaded now (no root needed)
#   irisxe-manager.sh scan <folder>             which driver parts a downloaded build folder contains (no root needed)
#   sudo irisxe-manager.sh install <folder> <part>...   install the chosen parts from that folder
#        parts: gpu accel agdc plugin fullplugin xlate autoarm
#   sudo irisxe-manager.sh repair               clear the two safety locks (after a hang); restart afterwards
#   sudo irisxe-manager.sh armnow               turn acceleration on now (restarts the desktop, open apps close)
#
# Every replaced file is kept in /Library/Application Support/IrisXe/backup/<date-time>/ first.
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
HERE=$(cd "$(dirname "$0")" && pwd)
EXT=/Library/Extensions; GB=/Library/GPUBundles; BKROOT="/Library/Application Support/IrisXe/backup"
MAJ=$(sw_vers -productVersion | cut -d. -f1)

say()  { echo "$*"; }
need_root() { [ "$(id -u)" = 0 ] || { say "ERROR: needs administrator rights"; exit 1; }; }

# ---------- finding the parts in a build folder ----------
# The GitHub artifact unzips to <artifact>/out/... (and ci/); a Mesa artifact has out/nmx-metal/NVMTLDriver.bundle.
find_part() {   # $1 folder, $2 part -> path or nothing
    local d=$1 p=$2 f
    case $p in
    gpu)   find "$d" -maxdepth 5 -type d -name NMXGPU.kext 2>/dev/null | head -1 ;;
    agdc)  find "$d" -maxdepth 5 -type d -name NVRMAGDC.kext 2>/dev/null | head -1 ;;
    accel) if [ "$MAJ" -ge 25 ]; then find "$d" -maxdepth 5 -type d -path '*/tahoe/NMXAccel.kext' 2>/dev/null | head -1
           else find "$d" -maxdepth 5 -type d -name NMXAccel.kext -not -path '*/tahoe/*' 2>/dev/null | head -1; fi ;;
    plugin) # the QUIET plugin binary (not out/debug/)
           find "$d" -maxdepth 5 -type d -name NVMTLDriver.bundle -not -path '*/debug/*' 2>/dev/null |
               while IFS= read -r f; do [ -f "$f/Contents/MacOS/NVMTLDriver" ] && { echo "$f"; break; }; done ;;
    fullplugin) # the full bundle from the Mesa build (has the shader reader air-runtime/)
           find "$d" -maxdepth 6 -type d -name NVMTLDriver.bundle 2>/dev/null |
               while IFS= read -r f; do [ -f "$f/Contents/MacOS/air-runtime/libLLVM.dylib" ] && { echo "$f"; break; }; done ;;
    xlate) find "$d" -maxdepth 6 -type f -name libnvmtl_translate.dylib -not -path '*/air-runtime/*' 2>/dev/null | head -1 ;;
    autoarm) [ -f "$HERE/helpers/nmx-autoarm/install-autoarm.sh" ] && echo "$HERE/helpers/nmx-autoarm" ;;
    esac
}

cmd_scan() {
    local d=${1:?folder}
    [ -d "$d" ] || { say "ERROR: no folder $d"; exit 1; }
    for p in gpu accel agdc plugin fullplugin xlate autoarm; do
        f=$(find_part "$d" "$p")
        if [ -n "$f" ]; then say "$p=$f|$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$f" 2>/dev/null)"; else say "$p="; fi
    done
}

# ---------- status ----------
kext_line() {   # $1 name
    local k="$EXT/$1.kext"
    if [ -d "$k" ]; then
        local when; when=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$k/Contents/MacOS/$1" 2>/dev/null || stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$k")
        local id; id=$(defaults read "$k/Contents/Info" CFBundleIdentifier 2>/dev/null)
        local loaded=no; kmutil showloaded 2>/dev/null | grep -q " $id " && loaded=yes
        say "$1|installed $when|loaded $loaded"
    else say "$1|not installed|"; fi
}
cmd_status() {
    say "macos|$(sw_vers -productVersion) ($(sw_vers -buildVersion))|"
    kext_line NMXGPU; kext_line NMXAccel; kext_line NVRMAGDC
    local P="$GB/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver"
    if [ -f "$P" ]; then
        local kind=VERBOSE; grep -aq nvmtl-off.log "$P" && kind=QUIET
        local air=missing; [ -f "$GB/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib" ] && air=present
        say "plugin|$kind, $(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$P")|shader reader $air"
    else say "plugin|not installed|"; fi
    if [ -f "$GB/nvmtl/libnvmtl_translate.dylib" ]; then say "xlate|$(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$GB/nvmtl/libnvmtl_translate.dylib")|"
    else say "xlate|not installed|"; fi
    if [ -f /usr/local/libexec/nmx-autoarm.sh ]; then
        say "autoarm|installed ($(grep -o 'autoarm v[0-9]*' /usr/local/libexec/nmx-autoarm.sh | head -1))|$(tail -1 /var/log/nmx-autoarm.log 2>/dev/null | cut -c1-90)"
    else say "autoarm|not installed|"; fi
    local fb; fb=$(sysctl -n debug.nvaccelfb 2>/dev/null); [ -z "$fb" ] && fb="off (NMXAccel not ready)"
    case $fb in 1) fb="ON";; 0) fb="off";; esac
    say "accel|$fb|"
    local g1 g2; g1=$(nvram nmx-autoarm-step 2>/dev/null | cut -f2-); g2=$(nvram nullmoth-gpu-attaching 2>/dev/null | cut -f2-)
    local lock=""
    [ -n "$g2" ] && lock="DISPLAY DRIVER PARKED (boot guard)"
    if [ -n "$g1" ] && grep -qE ' m[0-9] (ARM|RESTART)' <<<"$g1"; then
        # while nmx-autoarm.sh is still running it is in its 60 s check, not stuck: that is not a lock (b205)
        if pgrep -f nmx-autoarm.sh >/dev/null 2>&1; then lock="${lock:+$lock + }none yet - acceleration check running (1 min)"
        else lock="${lock:+$lock + }ACCELERATION LOCKED"; fi
    fi
    say "locks|${lock:-none}|"
    local hl; hl=$(tail -1 "/Library/Application Support/IrisXe/hangs.txt" 2>/dev/null)
    [ -n "$hl" ] && say "last hang|${hl//|/;}|"
    local ba; ba=$(sysctl -n kern.bootargs)
    local miss=""; for a in nmxintel=1 nmxaccel=1 nmxws=1 nmxautoarm=1; do grep -qw -- "$a" <<<"$ba" || miss="$miss $a"; done
    say "bootargs|${miss:+missing:$miss}${miss:-ok}|"
}

# ---------- install ----------
BK=""
backup() {   # $1 path to keep
    [ -e "$1" ] || return 0
    [ -n "$BK" ] || { BK="$BKROOT/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$BK"; }
    ditto "$1" "$BK/$(basename "$1")" && say "  kept the old one: $BK/$(basename "$1")"
}
install_kext() {   # $1 source kext, $2 name
    local s=$1 n=$2 d="$EXT/$2.kext"
    [ -f "$s/Contents/MacOS/$n" ] || { say "  ERROR: $s has no Contents/MacOS/$n"; return 1; }
    backup "$d"
    rm -rf "$d" && ditto "$s" "$d" || { say "  ERROR: copy failed"; return 1; }
    chown -R root:wheel "$d"; chmod -R 755 "$d"; xattr -dr com.apple.quarantine "$d" 2>/dev/null
    say "  $n.kext installed - macOS adds it at the next restart (approve it in Privacy & Security if asked)"
    KEXTS_CHANGED=1; CHANGED_KEXTS="$CHANGED_KEXTS $2"
}
install_plugin_binary() {   # quiet plugin: replace only the binary (+Info.plist), keep air-runtime/ (nmx-plugin-update.sh)
    local s=$1 D="$GB/NVMTLDriver.bundle"
    [ -d "$D/Contents/MacOS" ] || { say "  ERROR: no plugin installed yet - install the FULL plugin (Mesa build) first"; return 1; }
    backup "$D/Contents/MacOS/NVMTLDriver"
    cp "$s/Contents/MacOS/NVMTLDriver" "$D/Contents/MacOS/NVMTLDriver.new" && mv -f "$D/Contents/MacOS/NVMTLDriver.new" "$D/Contents/MacOS/NVMTLDriver" \
        || { say "  ERROR: copy failed"; return 1; }
    [ -f "$s/Contents/Info.plist" ] && cp "$s/Contents/Info.plist" "$D/Contents/Info.plist"
    chown -R root:wheel "$D"; chmod -R go-w,a+rX "$D"; xattr -dr com.apple.quarantine "$D" 2>/dev/null
    local kind=VERBOSE; grep -aq nvmtl-off.log "$D/Contents/MacOS/NVMTLDriver" && kind=QUIET
    say "  Metal plugin updated ($kind) - restart (or turn acceleration on again) to use it"
}
install_plugin_full() {
    local s=$1 D="$GB/NVMTLDriver.bundle"
    backup "$D"; mkdir -p "$GB"
    rm -rf "$D" && ditto "$s" "$D" || { say "  ERROR: copy failed"; return 1; }
    chown -R root:wheel "$D"; chmod -R go-w,a+rX "$D"; xattr -dr com.apple.quarantine "$D" 2>/dev/null
    [ -f "$GB/nvmtl-allow.txt" ] || printf '# processes that get the Intel Metal device (one name per line)\nmtlprobe\n' > "$GB/nvmtl-allow.txt"
    say "  full Metal plugin installed (with the shader reader)"
}
install_xlate() {
    local s=$1
    grep -aq "_nvmtl_translate" "$s" || { say "  ERROR: $s is not the shader translator"; return 1; }
    for D in "$GB/nvmtl" "$GB/NVMTLDriver.bundle/Contents/MacOS"; do
        local T="$D/libnvmtl_translate.dylib"
        if [ -f "$T" ] || [ "$D" = "$GB/nvmtl" ]; then
            mkdir -p "$D"; backup "$T"
            cp "$s" "$T.new" && mv -f "$T.new" "$T" || { say "  ERROR: copy to $T failed"; return 1; }
            chown root:wheel "$T"; chmod 755 "$T"; xattr -d com.apple.quarantine "$T" 2>/dev/null
        fi
    done
    say "  shader translator updated"
}
cmd_install() {
    need_root
    local d=${1:?folder}; shift
    [ $# -gt 0 ] || { say "nothing chosen"; exit 0; }
    KEXTS_CHANGED=0; local fails=0
    for p in "$@"; do
        local f; f=$(find_part "$d" "$p")
        say "== $p"
        [ -n "$f" ] || { say "  not in this folder - skipped"; continue; }
        say "  from: $f"
        case $p in
        gpu)        install_kext "$f" NMXGPU || fails=$((fails+1)) ;;
        accel)      install_kext "$f" NMXAccel || fails=$((fails+1)) ;;
        agdc)       install_kext "$f" NVRMAGDC || fails=$((fails+1)) ;;
        plugin)     install_plugin_binary "$f" || fails=$((fails+1)) ;;
        fullplugin) install_plugin_full "$f" || fails=$((fails+1)) ;;
        xlate)      install_xlate "$f" || fails=$((fails+1)) ;;
        autoarm)    bash "$f/install-autoarm.sh" 2>&1 | sed 's/^/  /' ;;
        esac
    done
    if [ "$KEXTS_CHANGED" = 1 ]; then
        touch "$EXT"
        # b213: rebuild macOS's cached copy of third-party drivers (the auxiliary kext collection). Without it Sequoia kept
        # running the build from before b190 although the new files were in /Library/Extensions (10-29: no SAGV line, the
        # per-commit log lines still on).
        # (kmutil install/create --update-all also rebuilds Apple's boot/system collections and needs a KDK - not that;
        # kmutil load on each changed kext rebuilds only the auxiliary collection and stages it for the next boot)
        say "updating macOS's driver cache (kmutil) - takes a minute"
        for k in $CHANGED_KEXTS; do
            say "  $k:"; kmutil load -p "$EXT/$k.kext" 2>&1 | tail -3 | sed 's/^/    /'
        done
        say ""; say "Kexts changed: restart the Mac. If macOS says a system extension was blocked, open"
        say "System Settings -> Privacy & Security, click Allow, and restart again."
    fi
    say ""; [ $fails = 0 ] && say "DONE" || say "DONE with $fails error(s) - see above"
}

# ---------- repair / arm ----------
cmd_repair() {
    need_root
    # b209: keep what the stuck boot left behind before clearing the locks (the next good boot overwrites the breadcrumb)
    local H="/Library/Application Support/IrisXe/hangs.txt"; mkdir -p "$(dirname "$H")"
    local hs ls as; hs=$(nvram nullmoth-hang-step 2>/dev/null | cut -f2-); ls=$(nvram nullmoth-last-step 2>/dev/null | cut -f2-)
    as=$(nvram nmx-autoarm-step 2>/dev/null | cut -f2-)
    if [ -n "$(nvram nullmoth-gpu-attaching 2>/dev/null)" ] || [ -n "$hs" ]; then
        echo "$(date '+%Y-%m-%d %H:%M') macOS $(sw_vers -productVersion): display driver stopped after step: ${hs:-$ls}; autoarm: ${as:-none}" >> "$H"
        say "recorded the stuck boot in $H: last step '${hs:-$ls}'"
    fi
    nvram -d nullmoth-hang-step 2>/dev/null
    nvram -d nmx-autoarm-step 2>/dev/null && say "acceleration safety lock cleared" || say "acceleration safety lock: was not set"
    nvram -d nullmoth-gpu-attaching 2>/dev/null && say "display driver boot guard cleared" || say "display driver boot guard: was not set"
    say "Restart the Mac now. If it hangs again, hold the power button - the guard parks the driver again by itself."
}
cmd_armnow() {
    need_root
    [ -f /usr/local/libexec/nmx-autoarm.sh ] || { say "ERROR: the acceleration helper is not installed (install 'autoarm' first)"; exit 1; }
    [ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ] && { say "acceleration is already ON"; exit 0; }
    nvram -d nmx-autoarm-step 2>/dev/null
    say "turning acceleration on - the screen goes black for a moment and open apps close (like logging out)"
    nohup /bin/bash /usr/local/libexec/nmx-autoarm.sh >/dev/null 2>&1 &
    say "started"
}

case "${1:-}" in
status)  cmd_status ;;
scan)    shift; cmd_scan "$@" ;;
install) shift; cmd_install "$@" ;;
repair)  cmd_repair ;;
armnow)  cmd_armnow ;;
*) sed -n 2,12p "$0"; exit 1 ;;
esac
