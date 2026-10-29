#!/bin/bash
# sudo ./install-autoarm.sh            install: acceleration turns on by itself at every boot (needs boot-arg nmxautoarm=N)
# sudo ./install-autoarm.sh --remove   remove it
# Modes (boot-arg): nmxautoarm=2 dry run, nmxautoarm=3 arm only (no WindowServer restart), nmxautoarm=1 full.
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
P=/Library/LaunchDaemons/com.nullmoth.autoarm.plist; S=/usr/local/libexec/nmx-autoarm.sh
BS="$HERE/../nmx-brightsync"
if [ "${1:-}" = "--remove" ]; then launchctl bootout system "$P" 2>/dev/null; rm -f "$P" "$S"
    [ -x "$BS/install-brightsync.sh" ] && "$BS/install-brightsync.sh" remove >/dev/null; echo "removed"; exit 0; fi
mkdir -p /usr/local/libexec
cp "$HERE/nmx-autoarm.sh" "$S" && chmod 755 "$S" && chown root:wheel "$S"
cp "$HERE/com.nullmoth.autoarm.plist" "$P" && chmod 644 "$P" && chown root:wheel "$P"
xattr -d com.apple.quarantine "$S" "$P" 2>/dev/null
plutil -lint "$P" >/dev/null || { echo "plist broken"; exit 1; }
echo "installed (v12): $P -> $S"
# b135: the login brightness sync (Control Center's slider follows the panel after a restart) comes with it
if [ -x "$BS/install-brightsync.sh" ]; then "$BS/install-brightsync.sh"; else echo "note: nmx-brightsync folder not found next to this one - brightness slider sync not installed"; fi
echo "It acts only when the boot-args contain nmxws=1 AND nmxautoarm=1, 2 or 3:"
echo "  2 = dry run (waits + breadcrumbs, arms nothing)   3 = arm only (no WindowServer restart)   1 = full"
echo "After each test boot: sudo ./nmx-autoarm-report.sh   (log: /var/log/nmx-autoarm.log)"
BA=$(sysctl -n kern.bootargs)
grep -qE '(^| )nmxautoarm=[0-9]' <<<"$BA" && echo "(this boot has: $(grep -oE 'nmxautoarm=[0-9]' <<<"$BA"))"
prev=$(nvram nmx-autoarm-step 2>/dev/null | cut -f2-)
[ -n "$prev" ] && echo "last recorded step: $prev   (clear with: sudo nvram -d nmx-autoarm-step)"
exit 0
