#!/bin/bash
# nmx-autoarm.sh (v13) - Phase 3: turn GPU acceleration (WindowServer on the Iris Xe) on at every boot, no script to run.
# Runs once at boot from the LaunchDaemon com.nullmoth.autoarm. Log: /var/log/nmx-autoarm.log
#
# The boot-arg picks the mode (no nmxautoarm boot-arg = off):
#   nmxautoarm=2  DRY RUN: every wait and breadcrumb, but it arms nothing and restarts nothing (tests the helper itself)
#   nmxautoarm=3  ARM ONLY: arms at the login window, does NOT restart WindowServer (tests the arm at boot by itself)
#   nmxautoarm=1  FULL: arms, then restarts the login window's WindowServer once so it comes back accelerated
# Also needs nmxws=1.
#
# Why WindowServer must be restarted at all: NMXGPU attaches only after the disks are mounted, so WindowServer is already
# running (unaccelerated, Metal plugin name withheld) before NMXAccel exists. Arming inside the kernel cannot change that
# WindowServer; only a new one picks up the plugin. So the order is: wait until the boot is completely settled -> arm ->
# restart WindowServer once, and only at the login window (no session is ever lost).
#
# Breadcrumbs: every step is stored in NVRAM (nmx-autoarm-step), which survives a forced power-off. After a freeze:
#   nvram nmx-autoarm-step        (or run nmx-autoarm-report.sh)
# Guard: if the last boot stopped inside an ARM or RESTART step (= froze there), this boot does NOT arm.
#   To try again: sudo nvram -d nmx-autoarm-step
# Waiting steps never trip the guard (v3 tripped it after a normal shutdown during its wait, or after any early exit).
LOG=/var/log/nmx-autoarm.log
exec >>"$LOG" 2>&1
echo "=== $(date) boot (autoarm v13)"
# v5 (b136, 10-22): the waits were sized for the freeze hunt (45 s stable WindowServer, 120 s since boot, 20 s watch):
# acceleration came ~2 min after a restart. The driver is stable now, so: 8 s stable, 25 s since boot, 3 s watch.
# The old, cautious timings come back with: sudo touch /Library/GPUBundles/nmx-autoarm-slow
if [ -f /Library/GPUBundles/nmx-autoarm-slow ]; then W_STABLE=45; W_BOOT=120; W_WATCH=20; echo "slow timings (nmx-autoarm-slow)"
elif [ -f /Library/GPUBundles/nmx-autoarm-fast ]; then W_STABLE=2; W_BOOT=0; W_WATCH=1; echo "fast timings (nmx-autoarm-fast)"
else W_STABLE=2; W_BOOT=0; W_WATCH=1; echo "quick timings (v9 default)"; fi
# v9 (b171, 10-25): shorter boot. The fast timings are the default now (2 s stable, the driver has been stable for days),
# and every wait polls 4x a second instead of once (each 1 s poll cost up to 1 s per step). Old v8 default: 8 / 25 / 3.
TICK=0.25; TPS=4
# v10: stability by the process's own age (ps etime), not by counting polls - pgrep takes ~0.4 s on macOS, so v9's
# "2 s" of polls was 5 s, and the login window's WindowServer has usually been running for 10 s when this script starts.
age() { ps -o etime= -p "$1" 2>/dev/null | awk '{t=$1; d=0; if (index(t,"-")) {split(t,x,"-"); d=x[1]; t=x[2]}
    n=split(t,a,":"); s=0; for (i=1; i<=n; i++) s=s*60+a[i]; print d*86400+s}'; }
BA=$(sysctl -n kern.bootargs)
MODE=$(tr ' ' '\n' <<<"$BA" | sed -n 's/^nmxautoarm=\([0-9]\)$/\1/p' | head -1)
grep -qw "nmxws=1" <<<"$BA" || { echo "boot-arg nmxws=1 missing - not arming"; exit 0; }
case "$MODE" in
    1) echo "mode 1: FULL (arm + one WindowServer restart at the login window)";;
    2) echo "mode 2: DRY RUN (waits and breadcrumbs only)";;
    3) echo "mode 3: ARM ONLY (no WindowServer restart)";;
    *) echo "boot-arg nmxautoarm=1/2/3 missing - not arming (that is the off switch)"; exit 0;;
esac

# v13 (b218): only the steps the guard and the report need go to NVRAM (ARM / RESTART / DONE / SKIP / STOP). The "wait:"
# steps ran while NMXGPU was still in its first mode set - which writes its own NVRAM breadcrumb at every step - and two
# boots hung inside that mode set at unrelated steps (10-29: "07 commit fence" 16:29, "50 hw_done" 21:06), both times
# while this helper had just written NVRAM. Firmware variable writes from two places at once are the suspect; the
# wait phase only polls, so it is logged, not stored.
step() {
    echo "$(date +%T) $*"
    case "$*" in ARM*|RESTART*|DONE*|SKIP*|STOP*) nvram nmx-autoarm-step="$(date +%T) m$MODE $*" 2>/dev/null; sync ;; esac
}
# v7 (b165): NMXGPU keeps the boot logo on screen until the accelerated WindowServer draws ("hold"). Whatever way this
# script ends - armed, skipped, guarded, refused - it tells the driver to show the desktop now (NVRAM nmx-handover=1,
# read and deleted by the driver within 250 ms; harmless when the hand-over has already happened).
# v8 (b168): the signal carries this boot's token (ioreg NMXHoldToken) - a signal left over from an earlier boot ended
# b167's hold at 12 s. No token = the driver is not holding the logo: nothing to send.
handover() {
    [ -n "$HANDED" ] && return; HANDED=1
    local tok; tok=$(ioreg -r -c NMXGPUFB -d 1 -l 2>/dev/null | sed -n 's/.*"NMXHoldToken" = "\([0-9a-f]*\)".*/\1/p' | head -1)
    if [ -n "$tok" ]; then nvram nmx-handover="$tok" 2>/dev/null; echo "$(date +%T) hand-over signal sent ($tok)"
    else echo "$(date +%T) no hold token - no hand-over signal needed"; fi
}
# v11 (b173, 10-25): NO EXIT trap any more. macOS's bash 3.2 also ran it when a $(...) subshell ended, so the
# hand-over signal went out silently as soon as this script started (13 s into the boot): the unaccelerated lock screen
# appeared, then the restart's black screen, then the lock screen again. Every exit below calls handover itself.
# v6 (b140): a normal Restart / Shut Down while this script still watches sends it SIGTERM - record that as a CLEAN stop,
# so the guard below does not mistake the user's own restart for a freeze (10-22: "no freeze, I restarted myself").
trap 'step "STOP: clean shutdown/restart (not a freeze)"; handover; exit 0' TERM
prev=$(nvram nmx-autoarm-step 2>/dev/null | cut -f2-)
if [ -n "$prev" ] && grep -qE ' m[0-9] (ARM|RESTART)' <<<"$prev"; then
    echo "GUARD: the last boot stopped inside a risky step: $prev"
    echo "       Not arming this boot. To try again: sudo nvram -d nmx-autoarm-step"
    handover; exit 0
fi
[ -n "$prev" ] && echo "last boot's last step: $prev"
# Phase 4 guard (nmx-app-gpu.sh): an app test that never finished froze the Mac - take those apps off the GPU list
# before this boot arms, or the first login would start them on the GPU again
apps=$(nvram nmx-app-test 2>/dev/null | cut -f2-)
if [ -n "$apps" ]; then
    AL=/Library/GPUBundles/nvmtl-allow.txt
    IFS='|' read -r -a bad <<<"$apps"
    for n in "${bad[@]}"; do [ -n "$n" ] && { grep -vxF "!$n" "$AL" > "$AL.tmp"; cat "$AL.tmp" > "$AL"; rm -f "$AL.tmp"; }; done
    nvram -d nmx-app-test
    echo "$(date) $apps froze the Mac (removed at boot by autoarm)" >> /Users/Shared/nmx-app-frozen.txt
    echo "app test guard: took off the GPU list (it froze the Mac last boot): $apps"
fi
step "wait: started"
T0=$(date +%s)
el() { echo $(( $(date +%s) - T0 )); }

# 1. the screen (NMXGPUFB) and the accelerator's controls (NMXAccel)
fbprops() { ioreg -r -c NMXGPUFB -d 1 -l 2>/dev/null; }
has_fb() { ioreg -r -c NMXGPUFB -d 1 2>/dev/null | grep -q NMXGPUFB; }
for i in $(seq 1 $((180 * TPS))); do has_fb && break; sleep $TICK; done
has_fb || { step "SKIP: no NMXGPUFB after 180 s (driver parked? boot guard nullmoth-gpu-attaching?)"; handover; exit 0; }
for i in $(seq 1 $((60 * TPS))); do sysctl debug.nvaccelfb >/dev/null 2>&1 && break; sleep $TICK; done
sysctl debug.nvaccelfb >/dev/null 2>&1 || { step "SKIP: debug.nvaccelfb missing (NMXAccel not loaded)"; handover; exit 0; }
if [ "$(sysctl -n debug.nvaccelfb)" != 0 ]; then step "DONE: already armed ($(sysctl -n debug.nvaccelfb))"; handover; exit 0; fi
echo "$(el)s: screen and accelerator present"

# 2. the login window's processes
for i in $(seq 1 $((240 * TPS))); do pgrep -x WindowServer >/dev/null && pgrep -x loginwindow >/dev/null && break; sleep $TICK; done
pgrep -x loginwindow >/dev/null || { step "SKIP: no login window after 240 s"; handover; exit 0; }
step "wait: login window processes up at $(el)s"

# 3. the display really up: WindowServer opened NMXGPUFB, its power registration ran, a VBL client is registered
#    (ioreg properties from NMXGPU build 103; an older kext has none of them, then the time limits below are all we have)
ready=""
for i in $(seq 1 $((120 * TPS))); do
    P=$(fbprops)
    if grep -q '"NMXFBOpened" = Yes' <<<"$P" && grep -q '"NMXFBPower" = "' <<<"$P" \
       && grep -qE '"NMXFBVBLClients" = [1-9]' <<<"$P"; then ready=1; break; fi
    sleep $TICK
done
if [ -n "$ready" ]; then
    step "wait: display up at $(el)s ($(grep -o '"NMXFBPower" = "[a-z]*"' <<<"$P"), $(grep -o '"NMXFBVBLClients" = [0-9]*' <<<"$P"))"
elif grep -q '"NMXFB' <<<"$P"; then
    step "SKIP: display never came up fully in 120 s: $(grep -o '"NMXFB[A-Za-z]*" = [^,}]*' <<<"$P" | tr '\n' ' ')"; handover; exit 0
else
    step "wait: NMXGPU older than build 103 (no NMXFB* properties) - using time only"
fi

# 4. WindowServer must be stable: the same pid for 45 s in a row (a crash loop restarts the count), and 120 s since boot
stable=0; last=""
for i in $(seq 1 $((300 * TPS))); do
    pid=$(pgrep -x WindowServer | head -1); last=$pid
    if [ -n "$pid" ]; then stable=$(age "$pid"); stable=${stable:-0}; else stable=0; fi
    [ "$stable" -ge "$W_STABLE" ] && break
    sleep $TICK
done
[ "$stable" -ge "$W_STABLE" ] || { step "SKIP: WindowServer never stayed up $W_STABLE s (pid $last) - not arming"; handover; exit 0; }
boot=$(sysctl -n kern.boottime | sed -E 's/^[^0-9]*sec = ([0-9]+).*/\1/')   # "{ sec = N, usec = M }": v3 read usec here
while [ $(( $(date +%s) - boot )) -lt $W_BOOT ]; do sleep 1; done
user=$(stat -f %Su /dev/console 2>/dev/null)
step "wait: settled at $(el)s, $(( $(date +%s) - boot ))s since boot, WindowServer $last, console user '$user'"

if [ "$MODE" = 2 ]; then
    step "DONE: DRY RUN finished - this is where mode 1/3 would arm"
    handover; exit 0
fi

# 5. the arm, as nmx-ws-arm.sh (each step stored BEFORE it runs: a freeze inside a sysctl leaves that step's name)
touch /tmp/nvmtl.log; chmod 666 /tmp/nvmtl.log
A=/Library/GPUBundles/nvmtl-allow.txt
grep -qx '!WindowServer' "$A" 2>/dev/null || echo '!WindowServer' >> "$A"
# b124: the live Sequoia / Macintosh wallpapers draw with Metal; without a device they fail and the previous wallpaper stays
for n in WallpaperSequoiaExtension WallpaperMacintoshExtension; do grep -qx "!$n" "$A" 2>/dev/null || echo "!$n" >> "$A"; done
# b190: iconservicesagent (Tahoe's icon renderer) kept off the GPU - on our device app icons and Settings glyphs came
# out as solid yellow squares. b211 (10-29) fixed the cause (buffer-backed textures were refused, so the renderer's
# shapes never reached the GPU): from a plugin that has the b211 fix on, icons are drawn on the GPU again (v12).
# Older plugins still get the CPU fallback. Back to the CPU by hand: sudo touch /Library/GPUBundles/nvmtl-iconcpu
P=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
ICON_GPU=1
if [ "$(uname -r | cut -d. -f1)" -ge 25 ] && [ ! -e /Library/GPUBundles/nvmtl-icongpu ] && [ ! -e /Library/GPUBundles/nvmtl-icontest ]; then
    if [ -e /Library/GPUBundles/nvmtl-iconcpu ] || ! grep -aq 'b211; refused before' "$P" 2>/dev/null; then ICON_GPU=0; fi
fi
if [ "$ICON_GPU" = 0 ]; then
    if ! grep -q '^ *- *iconservicesagent *$' "$A" 2>/dev/null; then
        { echo '- iconservicesagent'; cat "$A"; } > "$A.tmp" && cat "$A.tmp" > "$A"; rm -f "$A.tmp"
        rm -rf /Library/Caches/com.apple.iconservices.store; step "Tahoe: iconservicesagent kept off the GPU (nvmtl-iconcpu, or a plugin before b211), icon cache cleared"
    fi
elif grep -q '^ *- *iconservicesagent *$' "$A" 2>/dev/null; then
    grep -v '^ *- *iconservicesagent *$' "$A" > "$A.tmp" && cat "$A.tmp" > "$A"; rm -f "$A.tmp"
    rm -rf /Library/Caches/com.apple.iconservices.store; step "iconservicesagent on the GPU (b211 plugin), icon cache cleared"
fi
# b124: the shader reader must be there (a whole-bundle replace from the kext build deletes it) - say so loudly
[ -f /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib ] || step "WARNING: NVMTLDriver.bundle has NO air-runtime/libLLVM.dylib - new shaders will fail (restore it: nmx-plugin-update.sh)"
# b112: NMXAccel refuses nvaccelfb until WindowServer has opened the screen (arming earlier was the Phase 3 panic,
# IOFramebuffer::getAttributeExt on an unopened framebuffer). Retry for up to 60 s; never go on to AGDC / iop without it.
step "ARM 1/5 nvaccelfb=1"
# v9: read back right after each try, retry every 0.5 s (v8: 2 s per try, and 1 s even when the first try worked)
for t in $(seq 1 120); do sysctl -w debug.nvaccelfb=1; [ "$(sysctl -n debug.nvaccelfb)" = 1 ] && break; sleep 0.5; done
if [ "$(sysctl -n debug.nvaccelfb)" != 1 ]; then
    step "DONE: nvaccelfb refused for 60 s (screen not opened by WindowServer?) - nothing else armed"; handover; exit 0
fi
if [ "$(sysctl -n debug.nvaccelfb)" = 1 ]; then sleep 0.3; step "ARM 2/5 nvaccelfb=4"; sysctl -w debug.nvaccelfb=4; sleep 0.3; fi
if sysctl debug.nvrmfb_agdc >/dev/null 2>&1; then
    step "ARM 3/5 agdc_k5=1"; sysctl -w debug.nvrmfb_agdc_k5=1
    step "ARM 4/5 agdc=1"; sysctl -w debug.nvrmfb_agdc=1
    for k in $(seq 1 10); do [ "$(sysctl -n debug.nvrmfb_agdc)" = 1 ] && break; sleep 0.5; done
    echo "AGDC state: $(sysctl -n debug.nvrmfb_agdc)"
else
    step "DONE: NVRMAGDC.kext not loaded - WindowServer could not show GPU frames; stopped before the pipe"; handover; exit 0
fi
step "ARM 5/5 iop=1"; sysctl -w debug.nvaccel_iop=1
ARMED=$(date +%s)
if [ "$(sysctl -n debug.nvaccelfb)" != 1 ]; then step "DONE: nvaccelfb reads $(sysctl -n debug.nvaccelfb), not 1 - the screen did not register"; handover; exit 0; fi
step "ARM done - armed, watching $W_WATCH s before going on"
sleep $W_WATCH
pid=$(pgrep -x WindowServer | head -1)
[ "$pid" = "$last" ] || echo "note: WindowServer changed by itself after the arm ($last -> $pid)"

if [ "$MODE" = 3 ]; then
    step "DONE: arm-only mode - armed 20 s with no freeze; WindowServer $pid not restarted (log out and in, or restart it by hand)"
    handover; exit 0
fi

# 6. restart WindowServer once, only at the login window
[ -n "$pid" ] || { step "DONE: no WindowServer after the arm - the next one starts armed"; handover; exit 0; }
user=$(stat -f %Su /dev/console 2>/dev/null)
if [ "$user" != "root" ] && [ -n "$user" ]; then
    step "DONE: armed, but $user is logged in - WindowServer NOT restarted (no lost work); acceleration starts after a log out / restart of WindowServer"
    handover; exit 0
fi
step "RESTART killing login window WindowServer $pid"
kill -9 "$pid"
# the guard comes off once the new WindowServer has run 60 s with the same pid
stable=0; last=""
for i in $(seq 1 $((150 * TPS))); do
    sleep $TICK
    npid=$(pgrep -x WindowServer | head -1); last=$npid; stable=0
    if [ -n "$npid" ] && [ "$npid" != "$pid" ]; then stable=$(age "$npid"); stable=${stable:-0}; fi
    # v7: the new WindowServer has been up 2 s - tell the driver to show it
    [ "$stable" -ge 2 ] && handover
    [ "$stable" -ge 60 ] && break
done
if [ "$stable" -ge 60 ]; then step "DONE: armed WindowServer $last up for 60 s"
else step "RESTART not clean: WindowServer did not stay up 60 s (now '$last') - the next boot will not arm"; fi
handover; exit 0
