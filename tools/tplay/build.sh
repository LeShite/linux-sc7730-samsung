#!/bin/bash
# static, no libc: runs under TWRP's 3.10 kernel
cd "$(dirname "$0")" && clang --target=armv7a-linux-gnueabi -mfloat-abi=soft -marm -O2 -ffreestanding \
	-fno-builtin -fno-stack-protector -nostdlib -static -fuse-ld=lld -Wl,-e,_start -o tplay tplay.c
