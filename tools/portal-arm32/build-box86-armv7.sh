#!/usr/bin/env bash
# Cross-build Box86 for the tablet's ARMv7 hard-float ABI.
# Usage: build-box86-armv7.sh BOX86_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/build-common.sh"

SRC=${1:?usage: $0 BOX86_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]}
SYSROOT=${2:?usage: $0 BOX86_SOURCE ARMHF_SYSROOT [OUTPUT_DIR]}
OUT=${3:-"$HOME/batocera-t280/out/portal-arm32/box86"}
REPO_ROOT=$(cd -- "$HERE/../.." && pwd)
[[ -f "$SRC/CMakeLists.txt" ]] || { echo "not a Box86 source tree: $SRC" >&2; exit 1; }
portal_check_build_tools
mkdir -p "$OUT"
TOOLCHAIN="$OUT/armhf-toolchain.cmake"
portal_write_armhf_toolchain "$SYSROOT" "$TOOLCHAIN"

cmake -S "$SRC" -B "$OUT/build" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
	-DCMAKE_BUILD_TYPE=Release \
	-DARM_DYNAREC=1
cmake --build "$OUT/build" --parallel "${JOBS:-2}"

BOX86_BIN=$(find "$OUT/build" -type f -name box86 -perm -u+x -print -quit)
[[ -n "$BOX86_BIN" ]] || { echo "Box86 executable not found under $OUT/build" >&2; exit 1; }
mkdir -p "$OUT/package/box86/bin"
install -m 0755 "$BOX86_BIN" "$OUT/package/box86/bin/box86"
install -m 0755 "$HERE/run_portal.sh" "$OUT/package/box86/run_portal.sh"
install -m 0755 "$HERE/portal-rotation-hook.sh" "$OUT/package/box86/portal-rotation-hook.sh"
tar -czf "$OUT/box86-armv7.tar.gz" -C "$OUT/package" box86
file "$OUT/package/box86/bin/box86"
echo "Package: $OUT/box86-armv7.tar.gz"
install -D -m 0644 "$OUT/box86-armv7.tar.gz" "$REPO_ROOT/userdata/system/box86-armv7.tar.gz"
install -D -m 0755 "$HERE/install-box86-userdata.sh" "$REPO_ROOT/userdata/system/install-box86-userdata.sh"
echo "Staged for scripts/push-userdata.sh under $REPO_ROOT/userdata/system/"
