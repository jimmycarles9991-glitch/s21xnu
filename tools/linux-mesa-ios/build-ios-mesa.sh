#!/usr/bin/env bash
# Fresh Mesa 26.2.4 Panfrost iOS cross-build for the S21 Mali-G78 port.
#
# This script never uses the deleted Mesa tree/branch. It downloads the pinned
# upstream tarball, applies tools/linux-mesa-ios/patch_mesa_{ios,g78}.py, builds
# libs21drm.dylib plus EGL/GLES, and writes signed artifacts to OUT_DIR.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tools/linux-mesa-ios"
MESA_VERSION=${MESA_VERSION:-26.2.4}
MESA_SHA256=${MESA_SHA256:-bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9}
MESA_URL=${MESA_URL:-https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz}
WORK_DIR=${WORK_DIR:-/tmp/opencode/s21-ios-mesa}
OUT_DIR=${OUT_DIR:-$ROOT/out/linux-mesa-ios}
IOS_MIN=${IOS_MIN:-16.0}
SDK=${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}

command -v curl >/dev/null || { echo "curl missing" >&2; exit 1; }
command -v xcrun >/dev/null || { echo "xcrun/iOS SDK required" >&2; exit 1; }
command -v meson >/dev/null || { echo "meson required (pip install meson ninja)" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja required (pip install meson ninja)" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 required" >&2; exit 1; }
[[ -d "$SDK" ]] || { echo "SDK missing: $SDK" >&2; exit 1; }

if command -v brew >/dev/null 2>&1; then
   LLVM_PREFIX=$(brew --prefix llvm 2>/dev/null || true)
   if [[ -n "$LLVM_PREFIX" && -x "$LLVM_PREFIX/bin/llvm-config" ]]; then
      export PATH="$LLVM_PREFIX/bin:$PATH"
   fi
   # spirv-llvm-translator is keg-only; meson needs its .pc file to find
   # LLVMSPIRVLib at the version that matches Homebrew's LLVM.
   for pkg in llvm spirv-llvm-translator spirv-tools; do
      p=$(brew --prefix "$pkg" 2>/dev/null || true)
      if [[ -n "$p" && -d "$p/lib/pkgconfig" ]]; then
         export PKG_CONFIG_PATH="$p/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
      fi
   done
fi
command -v llvm-config >/dev/null || { echo "llvm-config required (brew install llvm)" >&2; exit 1; }
LLVM_CONFIG=$(command -v llvm-config)
export LLVM_CONFIG

mkdir -p "$WORK_DIR" "$OUT_DIR"
TARBALL="$WORK_DIR/mesa-${MESA_VERSION}.tar.xz"
if [[ ! -f "$TARBALL" ]]; then
   curl -fL --retry 3 -o "$TARBALL" "$MESA_URL"
fi
ACTUAL=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
if [[ "$ACTUAL" != "$MESA_SHA256" ]]; then
   echo "ERROR: Mesa tarball SHA-256 $ACTUAL != $MESA_SHA256" >&2
   exit 1
fi

MESA="$WORK_DIR/mesa"
rm -rf "$MESA"
mkdir -p "$MESA"
tar xf "$TARBALL" -C "$MESA" --strip-components=1
python3 "$HERE/patch_mesa_ios.py" "$MESA"
python3 "$HERE/patch_mesa_g78.py" "$MESA"

# Host-model test must pass before an iOS build is attempted.
CC=${CC:-cc} MESA_DIR="$MESA" "$HERE/shim/build-host-test.sh"

# Shared shim. Both EGL and the Panfrost DRI dylib link this one dylib, so
# BO/syncobj state is not duplicated across the two images.
SHIM="$OUT_DIR/libs21drm.dylib"
xcrun clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min="$IOS_MIN" \
   -O2 -fPIC -dynamiclib \
   -I"$HERE/shim" -I"$HERE/include" -I"$MESA/include" \
   "$HERE/shim/s21_drm.c" "$HERE/shim/s21_iokit_darwin.c" \
   -framework CoreFoundation -framework IOKit \
   -Wl,-install_name,@rpath/libs21drm.dylib -o "$SHIM"
codesign -f -s - "$SHIM"

# Native (build-machine) shim: the native panfrost compiler links pan_kmod.c,
# which includes our fake <xf86drm.h>. Only the host-test IOKit backend is needed.
NATIVE_SHIM="$WORK_DIR/native-shim"
mkdir -p "$NATIVE_SHIM"
xcrun -sdk macosx clang -arch x86_64 -dynamiclib -DS21_HOST_TEST \
   -I"$HERE/shim" -I"$HERE/include" -I"$MESA/include" \
   "$HERE/shim/s21_drm.c" "$HERE/shim/s21_iokit_host.c" -pthread \
   -o "$NATIVE_SHIM/libs21drm.dylib"

# Mesa's CLC shader compiler tools must be built for the Mac (build machine)
# and then reused with -Dmesa-clc=system; a cross build cannot link its native
# vtn_bindgen2 against host libraries.
CLC_BUILD="$WORK_DIR/native-clc"
rm -rf "$CLC_BUILD"
meson setup "$CLC_BUILD" "$MESA" \
   --wrap-mode=nofallback --buildtype=release \
   -Dprefix="$WORK_DIR/clc-prefix" -Dlibdir=lib \
   -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= \
   -Dglx=disabled -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dgbm=disabled \
   -Dglvnd=disabled -Dllvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
   -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
   -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
   -Dvalgrind=disabled -Dlibunwind=disabled -Dvideo-codecs= -Dtools= \
   -Dbuild-tests=false -Dgallium-va=disabled -Dlmsensors=disabled \
   -Dc_args="-DS21_IOS -I$HERE/shim -I$HERE/include" \
   -Dc_link_args="-L$NATIVE_SHIM -ls21drm -Wl,-rpath,$NATIVE_SHIM"
ninja -C "$CLC_BUILD" \
   src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
   src/panfrost/clc/panfrost_compile

export PATH="$CLC_BUILD/src/compiler/clc:$CLC_BUILD/src/compiler/spirv:$CLC_BUILD/src/panfrost/clc:$PATH"

CROSS="$WORK_DIR/ios-arm64.cross"
cat > "$CROSS" <<EOF
[binaries]
c = ['xcrun', '-sdk', 'iphoneos', 'clang']
cpp = ['xcrun', '-sdk', 'iphoneos', 'clang++']
objc = ['xcrun', '-sdk', 'iphoneos', 'clang']
objcxx = ['xcrun', '-sdk', 'iphoneos', 'clang++']
ar = ['xcrun', '-sdk', 'iphoneos', 'ar']
strip = ['xcrun', '-sdk', 'iphoneos', 'strip']
pkg-config = ['pkg-config']
llvm-config = ['$LLVM_CONFIG']

[properties]
sys_root = '$SDK'
c_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$IOS_MIN',
          '-DS21_IOS', '-I$HERE/shim', '-I$HERE/include']
cpp_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$IOS_MIN',
            '-DS21_IOS', '-I$HERE/shim', '-I$HERE/include']
c_link_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$IOS_MIN',
               '-L$OUT_DIR', '-ls21drm']

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

BUILD="$WORK_DIR/build"
rm -rf "$BUILD"
meson setup "$BUILD" "$MESA" --cross-file "$CROSS" \
   --wrap-mode=nofallback --buildtype=release \
   -Dprefix=/usr/local/s21-mesa -Dlibdir=lib \
   -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= \
   -Dglx=disabled -Dgles1=disabled -Dgles2=enabled -Degl=enabled -Dgbm=disabled \
   -Dglvnd=disabled -Dllvm=enabled -Dmesa-clc=system -Dprecomp-compiler=system -Dpanfrost-rust=false \
   -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
   -Dshader-cache=enabled -Dshader-cache-default=false -Dvalgrind=disabled \
   -Dlibunwind=disabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false \
   -Dgallium-va=disabled -Dlmsensors=disabled
ninja -C "$BUILD"

# Copy and normalise install names so the bundle is relocatable on iOS.
cp -v "$BUILD"/src/egl/libEGL*.dylib "$OUT_DIR/" 2>/dev/null || true
cp -v "$BUILD"/src/mapi/es2api/libGLESv2*.dylib "$OUT_DIR/" 2>/dev/null || true
cp -v "$BUILD"/src/gallium/targets/dri/libgallium*.dylib "$OUT_DIR/" 2>/dev/null || true

install_name_fix() {
   local f=$1 name
   name=$(basename "$f")
   install_name_tool -id "@rpath/$name" "$f"
   otool -L "$f" | awk '/lib(EGL|GLESv2|gallium|s21drm)/ {print $1}' | while read -r old; do
      [[ -n "$old" ]] || continue
      install_name_tool -change "$old" "@rpath/$(basename "$old")" "$f"
   done
   codesign -f -s - "$f"
}
for f in "$OUT_DIR"/*.dylib; do install_name_fix "$f"; done

# One-process GLES smoke test. It prints and writes the clear colour.
xcrun clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min="$IOS_MIN" \
   -O2 -I"$MESA/include" -I"$HERE/test" \
   -L"$OUT_DIR" -lEGL -lGLESv2 -Wl,-rpath,@executable_path \
   "$HERE/test/s21_gpu_test.c" -o "$OUT_DIR/s21-gpu-test"
codesign -f -s - "$OUT_DIR/s21-gpu-test"

echo "iOS Mesa artifacts in $OUT_DIR:"
ls -lh "$OUT_DIR"
