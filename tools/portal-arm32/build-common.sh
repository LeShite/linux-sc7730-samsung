#!/usr/bin/env bash

portal_check_build_tools() {
	for tool in cmake clang clang++; do
		command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; return 1; }
	done
}

portal_write_armhf_toolchain() {
	local sysroot=$1 output=$2 gcc_lib="" linker_paths link_overlay
	link_overlay="${output%/*}/armhf-link-libs"
	[[ -f "$sysroot/usr/include/stdio.h" ]] || {
		echo "not an armhf sysroot: $sysroot (expected usr/include/stdio.h)" >&2
		return 1
	}
	[[ -e "$sysroot/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3" || -e "$sysroot/lib/ld-linux-armhf.so.3" ]] || {
		echo "not a runnable armhf sysroot: missing ld-linux-armhf.so.3 under $sysroot/lib" >&2
		return 1
	}
	[[ -e "$sysroot/lib/arm-linux-gnueabihf/libm.so.6" ]] || {
		echo "not a linkable armhf sysroot: missing lib/arm-linux-gnueabihf/libm.so.6" >&2
		return 1
	}
	# This sysroot's /usr/lib/.../libm.so is an absolute symlink. LLD follows
	# that outside the sysroot and falls back to libm.a, which leaves glibc's
	# internal __scalbn symbols unresolved. A direct dynamic-library symlink
	# ensures -lm selects the ARMHF shared libm.so.6.
	mkdir -p "$link_overlay"
	ln -sfn "$sysroot/lib/arm-linux-gnueabihf/libm.so.6" "$link_overlay/libm.so"
	if [[ -d "$sysroot/usr/lib/gcc/arm-linux-gnueabihf" ]]; then
		gcc_lib=$(find "$sysroot/usr/lib/gcc/arm-linux-gnueabihf" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)
	fi
	linker_paths="-fuse-ld=lld -L$link_overlay -L$sysroot/usr/lib/arm-linux-gnueabihf -L$sysroot/lib/arm-linux-gnueabihf"
	[[ -z "$gcc_lib" ]] || linker_paths+=" -L$gcc_lib"
	cat >"$output" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET arm-linux-gnueabihf)
set(CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN "$sysroot/usr")
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET arm-linux-gnueabihf)
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN "$sysroot/usr")
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET arm-linux-gnueabihf)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm")
set(CMAKE_EXE_LINKER_FLAGS "$linker_paths" CACHE STRING "ARMHF cross-linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "$linker_paths" CACHE STRING "ARMHF cross-linker flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "$linker_paths" CACHE STRING "ARMHF cross-linker flags" FORCE)
set(CMAKE_FIND_ROOT_PATH "$sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
}
