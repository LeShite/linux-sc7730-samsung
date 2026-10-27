#!/usr/bin/env bash
# Optional fallback: Batocera v38 already ships /usr/lib/gl4es/libGL.so.1.
# Usage: build-gl4es-armv7.sh GL4ES_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/build-common.sh"

SRC=${1:?usage: $0 GL4ES_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]}
SYSROOT=${2:?usage: $0 GL4ES_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]}
OUT=${3:-"$HOME/batocera-t280/out/portal-arm32/gl4es"}
[[ -f "$SRC/CMakeLists.txt" ]] || { echo "not a GL4ES source tree: $SRC" >&2; exit 1; }
portal_check_build_tools
mkdir -p "$OUT"
TOOLCHAIN="$OUT/armhf-toolchain.cmake"
portal_write_armhf_toolchain "$SYSROOT" "$TOOLCHAIN"

cmake -S "$SRC" -B "$OUT/build" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="-Wno-error=return-mismatch -Wno-return-mismatch -Wno-int-conversion -Wno-incompatible-pointer-types" \
	-DNOX11=ON -DNOEGL=OFF -DGBM=OFF -DGLX_STUBS=ON -DEGL_WRAPPER=ON \
	-DDEFAULT_ES=2 \
	-DDEFAULT_EGL=/usr/lib/libEGL.so.1 \
	-DDEFAULT_GLES=/usr/lib/libGLESv2.so.2
cmake --build "$OUT/build" --parallel "${JOBS:-2}"

LIB="$SRC/lib/libGL.so.1"
[[ -f "$LIB" ]] || { echo "GL4ES build did not produce $LIB" >&2; exit 1; }
mkdir -p "$OUT/package/gl4es"
install -m 0755 "$LIB" "$OUT/package/gl4es/libGL.so.1"
tar -czf "$OUT/gl4es-armv7.tar.gz" -C "$OUT/package" gl4es
file "$OUT/package/gl4es/libGL.so.1"
echo "Package: $OUT/gl4es-armv7.tar.gz"
