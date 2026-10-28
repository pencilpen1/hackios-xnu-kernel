#!/usr/bin/env bash
# Reproducible source recipe: pinned official Mesa + this Lancelot-only overlay.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO=$(cd -- "$HERE/../.." && pwd -P)
MESA="$HERE"
B=${MESA_BUILD:-$REPO/local/mesa-lancelot-build}
REFERENCE=${REFERENCE:-/home/pencilqwq/hackios-lancelot}
SDK=${SDK:-$REFERENCE/local-toolchain/theos/sdks/iPhoneOS16.5.sdk}
CC=${CC:-$REFERENCE/xnu-deps/host/bin/xnu-clang}
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
[[ -f "$MESA/apply-lancelot.py" && -f "$MESA/lancelot-drm/lancelot_drm.c" ]] || {
 echo 'Lancelot Mesa overlay files are missing beside this script' >&2; exit 1; }
mkdir -p "$B/tmp" "$B/out"
export TMPDIR="$B/tmp" XDG_CACHE_HOME="$B/cache" PIP_CACHE_DIR="$B/pip-cache" CCACHE_DISABLE=1
if [[ ! -f "$B/mesa-$VERSION.tar.xz" ]]; then
 curl --fail --location --retry 3 --silent --show-error -o "$B/mesa-$VERSION.tar.xz" "https://archive.mesa3d.org/mesa-$VERSION.tar.xz"
fi
printf '%s  %s\n' "$SHA256" "$B/mesa-$VERSION.tar.xz" | sha256sum --check --status
if [[ ! -x "$B/venv/bin/meson" ]]; then
 python3 -m venv "$B/venv"
 "$B/venv/bin/pip" install --no-cache-dir 'meson==1.12.1' 'ninja==1.13.2' 'mako==1.4.3' 'pyyaml==6.0.3' 'packaging==26.3'
fi
export PATH="$B/venv/bin:$REFERENCE/xnu-deps/host/bin:$PATH"
# A changed overlay gets a fresh source/build directory; never reuse stale patched sources.
FINGERPRINT=$(python3 "$MESA/fingerprint.py")
KEY=$(printf '%s' "$FINGERPRINT" | sha256sum | cut -c1-16)
S="$B/source-$KEY/mesa-$VERSION"
BUILD="$B/darwin-$KEY"
if [[ ! -f "$S/.lancelot-overlay" ]]; then
 mkdir -p "$B/source-$KEY"
 tar -xJf "$B/mesa-$VERSION.tar.xz" -C "$B/source-$KEY"
 python3 "$MESA/apply-lancelot.py" "$S"
 python3 "$MESA/apply-lancelot-fences.py" "$S"
 python3 "$MESA/apply-lancelot-screen.py" "$S"
 printf '%s\n' "$FINGERPRINT" > "$S/.lancelot-overlay"
fi
printf '%s\n' "$S" > "$B/current-source"
printf '%s\n' "$BUILD" > "$B/current-build"
if [[ ${1:-} == --prepare ]]; then exit 0; fi
cat > "$B/cross.ini" <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
INI
if [[ ! -f "$BUILD/build.ninja" ]]; then
 meson setup "$BUILD" "$S" --cross-file "$B/cross.ini" --wrap-mode=nofallback \
  --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled \
  -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
  -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
  -Dlmsensors=disabled -Dbuild-tests=false
fi
ninja -C "$BUILD" -j "${JOBS:-8}" \
 src/gallium/targets/osmesa/libOSMesa.8.dylib \
 src/gallium/drivers/panfrost/air_run \
 src/gallium/drivers/panfrost/libredwoodair.dylib \
 src/gallium/drivers/panfrost/air_metal_test \
 src/compiler/air/air2nir
cp "$BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib" "$B/out/"
cp "$BUILD/src/gallium/drivers/panfrost/"{air_run,libredwoodair.dylib,air_metal_test} "$B/out/"
cp "$BUILD/src/compiler/air/air2nir" "$B/out/"
install_name_tool -id /redwood/lib/libOSMesa.8.dylib "$B/out/libOSMesa.8.dylib"
install_name_tool -id /redwood/lib/libredwoodair.dylib "$B/out/libredwoodair.dylib"
cp "$S/src/mapi/glapi/registry/gl.xml" "$B/out/gl.xml"
nm -gU "$B/out/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3,2) }' > "$B/out/exported-gl.txt"
printf '%s\n' "$FINGERPRINT" > "$B/out/built-from"
(cd "$B/out"; sha256sum libOSMesa.8.dylib libredwoodair.dylib air_run air_metal_test air2nir > sha256.txt)
printf 'Built Lancelot Panfrost + softpipe in %s\n' "$B/out"
