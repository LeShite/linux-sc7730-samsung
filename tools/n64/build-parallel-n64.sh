#!/bin/bash
# Cross-compiles the libretro core parallel_n64 (https://github.com/libretro/parallel-n64) for the SM-T280:
# 32-bit ARM, Cortex-A7 with NEON, GLES2 renderer, ARM dynarec, against a Debian bookworm armhf sysroot
# (glibc 2.36, older than the tablet's 2.37 so the .so runs there).
#   usage: build-parallel-n64.sh SRC_DIR SYSROOT_DIR      ->  SRC_DIR/parallel_n64_libretro.so
# The Makefile's own LDFLAGS (-shared, version script, -lpthread...) must stay: the linker options go in CC/CXX.
set -euo pipefail
SRC=${1:?directory of the parallel-n64 clone}
S=${2:?sysroot with the Debian armhf packages unpacked}

FL="--target=arm-linux-gnueabihf --sysroot=$S --gcc-toolchain=$S/usr -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm"
LD="-fuse-ld=lld -L$S/usr/lib/arm-linux-gnueabihf -L$S/lib/arm-linux-gnueabihf -L$S/usr/lib/gcc/arm-linux-gnueabihf/12"
INC="-I$S/usr/include/arm-linux-gnueabihf/c++/12 -I$S/usr/include/c++/12"

cd "$SRC"
make -j"$(nproc)" platform=armv-neon-gles HAVE_PARALLEL=0 HAVE_PARALLEL_RSP=0 \
	CC="clang $FL $LD" CXX="clang++ $FL $LD $INC"
file parallel_n64_libretro.so
