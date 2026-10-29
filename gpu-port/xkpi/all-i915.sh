#!/bin/bash
# all-i915.sh [N]: compiles every source drm-kmod's i915 Makefile builds (evaluated against amd.flags, the shared CONFIG
# set) into obj-i915/, and ranks the remaining errors. The DRM core, TTM and dma-buf come from all.sh (obj/).
X=${NMX_X:-/Volumes/MiniData/build/gpu-src/xkpi}
K=${NMX_K:-/Volumes/MiniData/build/gpu-src/kpi}
cd "$X" || exit 1
mkdir -p obj-i915 logs-i915 && rm -f logs-i915/*.log logs-i915/*.failed
# intel_ggtt_gmch.c: pre-Gen6 GTT through FreeBSD agp(4); override/i915/nmx_i915_gmch.c answers for it
# i915_gem_mman.c: forked by fork_i915_gem_mman.py into override/i915
EXCLUDE="intel_ggtt_gmch.c i915_gem_mman.c"
# Conservative display power defaults while the panel is owned by the firmware framebuffer. macOS draws into that framebuffer
# with the CPU, which i915's frontbuffer tracking never sees, so panel self refresh (PSR), deep display power states (DC5/6)
# and power-well shutdown leave the panel showing a stale frame (frozen console, then black). All four stay off until the
# port tracks its own framebuffer writes. Idempotent; the values are what the screen test needs, not a long-term choice.
DP=$(find "$K/drm-kmod" -path '*i915/display/intel_display_params.h' | head -1)
if [ -n "$DP" ]; then
    sed -i.orig -E \
        -e 's/(param\(int, enable_dc, )-1,/\10,/' \
        -e 's/(param\(int, enable_psr, )-1,/\10,/' \
        -e 's/(param\(int, disable_power_well, )-1,/\10,/' \
        -e 's/(param\(int, enable_fbc, )-1,/\10,/' "$DP"
    grep -E "param\(int, (enable_dc|enable_psr|disable_power_well|enable_fbc)," "$DP"
else
    echo "WARNING: intel_display_params.h not found; display power defaults unchanged"
fi
# Keep GEM and i915's error-capture slot out of the GGTT range that still maps the firmware framebuffer (see the script).
GG=$(find "$K/drm-kmod" -path '*i915/gt/intel_ggtt.c' | head -1)
if [ -n "$GG" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_fwfb.py "$GG" || { echo "ERROR: firmware framebuffer patch failed"; exit 1; }
else
    echo "ERROR: gt/intel_ggtt.c not found"; exit 1
fi
GS=$(find "$K/drm-kmod" -path '*i915/gem/i915_gem_stolen.c' | head -1)
if [ -n "$GS" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_stolen_fwfb.py "$GS" || { echo "ERROR: stolen framebuffer patch failed"; exit 1; }
else
    echo "ERROR: gem/i915_gem_stolen.c not found"; exit 1
fi
GM=$(find "$K/drm-kmod" -path '*i915/gem/i915_gem_object.c' | head -1)
if [ -n "$GM" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_vmap.py "$GM" || { echo "ERROR: GEM vmap patch failed"; exit 1; }
else
    echo "ERROR: gem/i915_gem_object.c not found"; exit 1
fi
GV=$(find "$K/drm-kmod" -path '*i915/display/intel_display_driver.c' | head -1)
if [ -n "$GV" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_novga.py "$GV" || { echo "ERROR: VGA knob patch failed"; exit 1; }
else
    echo "ERROR: display/intel_display_driver.c not found"; exit 1
fi
GD=$(find "$K/drm-kmod" -path '*i915/display/intel_display.c' | head -1)
if [ -n "$GD" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_committrace.py "$GD" || { echo "ERROR: commit trace patch failed"; exit 1; }
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_fencewait.py "$GD" || { echo "ERROR: fence wait patch failed"; exit 1; }
else
    echo "ERROR: display/intel_display.c not found"; exit 1
fi
GW=$(find "$K/drm-kmod" -path '*i915/display/skl_watermark.c' | head -1)
if [ -n "$GW" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_nosagv.py "$GW" || { echo "ERROR: SAGV patch failed"; exit 1; }
else
    echo "ERROR: display/skl_watermark.c not found"; exit 1
fi
GI=$(find "$K/drm-kmod" -path '*i915/display/intel_plane_initial.c' | head -1)
if [ -n "$GI" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_initial_fb.py "$GI" || { echo "ERROR: initial framebuffer patch failed"; exit 1; }
else
    echo "ERROR: display/intel_plane_initial.c not found"; exit 1
fi
GP=$(find "$K/drm-kmod" -path '*i915/display/intel_display_power.c' | head -1)
if [ -n "$GP" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_display_rpm.py "$GP" || { echo "ERROR: display runtime-PM patch failed"; exit 1; }
else
    echo "ERROR: display/intel_display_power.c not found"; exit 1
fi
GU=$(find "$K/drm-kmod" -path '*i915/gem/i915_gem_userptr.c' | head -1)
if [ -n "$GU" ]; then
    ${NMX_PY:-/opt/homebrew/bin/python3} patch_i915_userptr.py "$GU" || { echo "ERROR: userptr patch failed"; exit 1; }
else
    echo "ERROR: gem/i915_gem_userptr.c not found"; exit 1
fi
${NMX_PY:-/opt/homebrew/bin/python3} srclist.py "$K/drm-kmod" amd.flags i915 2>/dev/null > srcs-i915.all
: > srcs-i915.txt
while read -r f; do
    case " $EXCLUDE " in
        *" $(basename "$f") "*) ;;
        *) echo "$f" >> srcs-i915.txt ;;
    esac
done < srcs-i915.all
${NMX_PY:-/opt/homebrew/bin/python3} fork_i915_gem_mman.py "$K/drm-kmod" override/i915/i915_gem_mman.c >/dev/null
for f in "$X"/override/i915/*.c; do echo "$f" >> srcs-i915.txt; done
n=$(wc -l < srcs-i915.txt | tr -d ' ')
one() { b=$(basename "$1" .c); rm -f logs-i915/$b.failed; ./cc-i915.sh "$1" > logs-i915/$b.log 2>&1 || touch logs-i915/$b.failed; }
export -f one
xargs -P 10 -I{} bash -c 'one "$@"' _ {} < srcs-i915.txt
bad=$(find logs-i915 -name "*.failed" | wc -l | tr -d ' ')
echo "sources: $n  compiled: $((n - bad))  failed: $bad"
for f in $(find logs-i915 -name "*.failed"); do cat "${f%.failed}.log"; done | grep -h "error:" | sed -E 's|^[^ ]*/([^/]+:[0-9]+):[0-9]+: error: |\1 |' | sed -E 's/^[^ ]+ //' \
    | sort | uniq -c | sort -rn | head -"${1:-40}"
