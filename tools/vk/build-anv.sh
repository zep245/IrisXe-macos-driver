#!/bin/bash
# build-anv.sh <work dir> - builds Mesa's Intel Vulkan driver (ANV) and libdrm for macOS x86_64 with the NullMoth
# patches (gpu-port/patches), plus the test tools, into <work>/out/nmx-vulkan. Used by .github/workflows/mesa-anv-build.yml.
set -u
W="${1:?work dir}"; mkdir -p "$W"; W=$(cd "$W" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MESA_REF="${MESA_REF:-bffd919}"; LIBDRM_REF="${LIBDRM_REF:-b97cbde}"
P="$W/prefix"; OUT="$W/out/nmx-vulkan"; LOG="$W/logs"; mkdir -p "$P" "$OUT" "$LOG"
BREW=$(brew --prefix)
export PATH="$BREW/opt/bison/bin:$BREW/opt/flex/bin:$BREW/opt/llvm/bin:$PATH"
export PKG_CONFIG_PATH="$P/lib/pkgconfig:$BREW/lib/pkgconfig:$BREW/opt/llvm/lib/pkgconfig"
export MACOSX_DEPLOYMENT_TARGET="${NMX_MIN_OS:-14.0}"
say() { echo; echo "=== $*"; }

# meson setup that drops options this source revision does not know, so one option list fits both trees
msetup() { # srcdir builddir options...
  local src=$1 bld=$2; shift 2; local opts=("$@") try
  for try in 1 2 3 4 5 6 7 8 9 10 11 12; do
    rm -rf "$bld"
    if meson setup "$bld" "$src" --prefix "$P" --buildtype release "${opts[@]}" > "$LOG/$(basename "$src")-setup.log" 2>&1; then return 0; fi
    # a feature the auto_features=disabled default turned off but another enabled one needs ("CLC requires
    # SPIRV-Tools"): turn it on and try again
    local need; need=$(grep -oE 'Feature [A-Za-z0-9_-]+ cannot be disabled' "$LOG/$(basename "$src")-setup.log" | awk '{print $2}' | head -1)
    if [ -n "$need" ]; then echo "enabling required feature: $need"; opts+=("-D$need=enabled"); continue; fi
    local bad; bad=$(grep -oE 'Unknown options?: "[^"]+"' "$LOG/$(basename "$src")-setup.log" | sed -E 's/.*: "([^"]+)"/\1/' | tr ',' '\n' | tr -d ' ')
    [ -n "$bad" ] || { tail -40 "$LOG/$(basename "$src")-setup.log"; return 1; }
    echo "dropping unknown options: $bad"
    local keep=() o b skip
    for o in "${opts[@]}"; do skip=0; for b in $bad; do [[ "$o" == "-D$b="* ]] && skip=1; done; [ $skip = 0 ] && keep+=("$o"); done
    opts=("${keep[@]}")
  done
  return 1
}

say "sources"
fetch() { # name url ref
  [ -d "$W/$1/.git" ] || git clone -q --filter=blob:none "$2" "$W/$1" || return 1
  git -C "$W/$1" checkout -q "$3"
}
fetch libdrm https://gitlab.freedesktop.org/mesa/drm.git "$LIBDRM_REF" || fetch libdrm https://github.com/freedesktop/mesa-drm.git "$LIBDRM_REF" || exit 1
fetch mesa https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_REF" || fetch mesa https://github.com/mesa3d/mesa.git "$MESA_REF" || exit 1
(cd "$W/libdrm" && git checkout -q . && patch -p1 < "$REPO/gpu-port/patches/libdrm-macos.patch" \
  && python3 "$REPO/tools/vk/libdrm-darwin-minor.py" xf86drm.c \
  && python3 "$REPO/tools/vk/libdrm-darwin-etime.py" xf86drm.c) || { echo "libdrm patch failed"; exit 1; }
(cd "$W/mesa" && git checkout -q . && patch -p1 < "$REPO/gpu-port/patches/mesa-macos.patch") || { echo "mesa patch failed"; exit 1; }
# GPU-memory maps from inside Mesa (the Metal plugin dlopen()s ANV into apps): mmap/munmap -> NMXGPU's map ioctl
cp "$REPO/tools/vk/nmx_mmap_impl.c" "$W/mesa/src/util/nmx_mmap_impl.c"
python3 - "$W/mesa/src/util/meson.build" <<'PY' || { echo "could not add nmx_mmap_impl.c to Mesa"; exit 1; }
import re, sys
p = sys.argv[1]; s = open(p).read()
if 'nmx_mmap_impl.c' not in s:
    m = re.search(r"files_mesa_util\s*=\s*files\(", s)
    if not m: sys.exit("files_mesa_util not found")
    s = s[:m.end()] + "\n  'nmx_mmap_impl.c'," + s[m.end():]
    open(p, 'w').write(s)
print("mesa: nmx_mmap_impl.c added to libmesa_util")
PY
MMAPINC="-include$REPO/tools/vk/nmx_mmap_redirect.h"

say "libdrm"
msetup "$W/libdrm" "$W/b-libdrm" -Dintel=disabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled \
  -Dtegra=disabled -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false || exit 1
ninja -C "$W/b-libdrm" install > "$LOG/libdrm-build.log" 2>&1 || { tail -60 "$LOG/libdrm-build.log"; exit 1; }

say "mesa (ANV)"
# b240: video codecs on (H.264/H.265/AV1/VP9 decode through Vulkan Video) for hardware video decoding, stage 1:
# the probe below asks the GPU what it can decode. NMX_VIDEO_CODECS= (empty) builds without them, as before b240.
# Mesa builds the Intel driver's internal shaders with its own OpenCL compiler (mesa_clc), so LLVM, Clang, libclc and
# the SPIR-V translator are needed at build time only (from conda-forge, MESA_LLVM_PREFIX; Homebrew compiles LLVM from
# source on Intel runners). The finished driver does not link LLVM.
LP="${MESA_LLVM_PREFIX:-}"
NATIVE=()
if [ -n "$LP" ]; then
  export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$LP/lib/pkgconfig:$LP/share/pkgconfig"
  printf "[binaries]\nllvm-config = '%s/bin/llvm-config'\n" "$LP" > "$W/llvm-native.ini"
  NATIVE=(--native-file "$W/llvm-native.ini")
  echo "LLVM: $("$LP/bin/llvm-config" --version) at $LP"
fi
export CC=/usr/bin/clang CXX=/usr/bin/clang++
msetup "$W/mesa" "$W/b-mesa" ${NATIVE[@]+"${NATIVE[@]}"} -Dauto_features=disabled -Dvulkan-drivers=intel -Dgallium-drivers= -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Dbuild-tests=false -Dtools= -Dvideo-codecs=${NMX_VIDEO_CODECS:-all} -Dintel-rt=disabled -Dmesa-clc=enabled -Dllvm=enabled \
  -Dshared-llvm=enabled -Dspirv-tools=enabled -Dshader-cache=enabled -Dexpat=enabled -Dzlib=enabled \
  "-Dc_args=$MMAPINC" "-Dcpp_args=$MMAPINC" || exit 1
ninja -C "$W/b-mesa" > "$LOG/mesa-build.log" 2>&1 || { grep -m30 -E "error|FAILED" "$LOG/mesa-build.log"; tail -40 "$LOG/mesa-build.log"; exit 1; }
ANV=$(find "$W/b-mesa" -name 'libvulkan_intel*.dylib' | head -1)
[ -n "$ANV" ] || { echo "no libvulkan_intel.dylib"; exit 1; }

say "bundle"
cp "$ANV" "$OUT/libvulkan_intel.dylib"
cp "$P/lib/libdrm.2.dylib" "$OUT/" 2>/dev/null || cp "$(ls "$P"/lib/libdrm*.dylib | head -1)" "$OUT/libdrm.2.dylib"
cp "$BREW/lib/libvulkan.1.dylib" "$OUT/" || cp "$(ls "$BREW"/opt/vulkan-loader/lib/libvulkan.1*.dylib | head -1)" "$OUT/libvulkan.1.dylib"
# point every non-system dependency at the copy next to it (one level is enough for these three)
fixdeps() {
  local f=$1 d
  install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null
  for d in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vE '^/usr/lib|^/System|^@'); do
    local b; b=$(basename "$d")
    case "$b" in libdrm*) b=libdrm.2.dylib;; esac
    [ -f "$OUT/$b" ] || cp "$d" "$OUT/$b"
    install_name_tool -change "$d" "@loader_path/$b" "$f"
  done
}
for f in "$OUT"/*.dylib; do fixdeps "$f"; done
for f in "$OUT"/*.dylib; do fixdeps "$f"; done   # copies made in the first pass
cat > "$OUT/intel_icd.json" <<JSON
{ "file_format_version": "1.0.0", "ICD": { "library_path": "./libvulkan_intel.dylib", "api_version": "1.3.0" } }
JSON
clang -arch x86_64 -O2 -dynamiclib "$REPO/tools/vk/nmxmmap.c" -o "$OUT/nmxmmap.dylib" || exit 1
glslangValidator -V "$REPO/gpu-port/tools/fill.comp" -o "$W/fill.spv" > /dev/null || exit 1
(cd "$W" && xxd -i fill.spv > fill_spv.h)
VKH=$(brew --prefix vulkan-headers)/include
clang -arch x86_64 -O2 -I"$VKH" -I"$W" "$REPO/tools/vk/vkfill.c" -L"$OUT" -lvulkan.1 -Wl,-rpath,@executable_path -o "$OUT/vkfill" || exit 1
install_name_tool -change "$(otool -L "$OUT/vkfill" | awk '/libvulkan/{print $1}')" "@executable_path/libvulkan.1.dylib" "$OUT/vkfill"
# drmlist: lists DRM devices through libdrm the way Mesa finds them (device discovery check)
clang -arch x86_64 -O2 -I"$P/include" -I"$P/include/libdrm" "$REPO/gpu-port/tools/drmlist.c" -L"$OUT" -ldrm.2 -o "$OUT/drmlist" \
  && install_name_tool -change "$(otool -L "$OUT/drmlist" | awk '/libdrm/{print $1}')" "@executable_path/libdrm.2.dylib" "$OUT/drmlist" \
  || echo "drmlist did not build"
# b240: vkvideoprobe - can this GPU decode video through Vulkan Video (and does NMXGPU reach the video engine)?
clang -arch x86_64 -O2 -I"$VKH" "$REPO/tools/vk/vkvideoprobe.c" -L"$OUT" -lvulkan.1 -Wl,-rpath,@executable_path -o "$OUT/vkvideoprobe" \
  && install_name_tool -change "$(otool -L "$OUT/vkvideoprobe" | awk '/libvulkan/{print $1}')" "@executable_path/libvulkan.1.dylib" "$OUT/vkvideoprobe" \
  || echo "vkvideoprobe did not build"
cp "$REPO/tools/vk/run-vk.sh" "$REPO/tools/vk/run-vkvideo.sh" "$OUT/" && chmod +x "$OUT/run-vk.sh" "$OUT/run-vkvideo.sh" "$OUT/vkfill"
[ -x "$OUT/vkvideoprobe" ] && chmod +x "$OUT/vkvideoprobe"
otool -L "$OUT/libvulkan_intel.dylib" > "$LOG/anv-deps.txt"
echo "ANV meson config:"; grep -iE "clc|llvm|intel" "$LOG/mesa-setup.log" | head -20
ls -la "$OUT"
