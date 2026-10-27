#!/bin/bash
# Build the mainline kernel for the SM-T280 and pack it into a flashable
# boot image: ~/batocera-t280/out/boot-gtex.img
set -euo pipefail

K=~/batocera-t280/src/linux-6.18.58
O=~/batocera-t280/build/kernel
OUT=~/batocera-t280/out
DTB=$O/arch/arm/boot/dts/unisoc/sc7730-samsung-gtexswifi.dtb
mkdir -p "$O" "$OUT"

if command -v ld.lld >/dev/null; then
	TC=(LLVM=1)
elif command -v arm-none-eabi-gcc >/dev/null; then
	TC=(CROSS_COMPILE=arm-none-eabi-)
else
	echo "falta el compilador: instala lld o arm-none-eabi-gcc" >&2
	exit 1
fi
MK=(make -C "$K" O="$O" ARCH=arm "${TC[@]}")

# The built-in initramfs (initramfs/initramfs.list) needs our /init first
mkdir -p ~/batocera-t280/build/initramfs
clang --target=armv7a-linux-gnueabi -marm -mcpu=cortex-a7 -mfloat-abi=soft -O2 -Wall \
	-nostdlib -ffreestanding -fno-builtin -fno-stack-protector -static \
	-fuse-ld=lld -Wl,-e,_start -Wl,--build-id=none \
	-o ~/batocera-t280/build/initramfs/init ~/batocera-t280/initramfs/init.c

"${MK[@]}" allnoconfig KCONFIG_ALLCONFIG="$K/arch/arm/configs/gtexswifi.config" >/dev/null

# allnoconfig silently drops options whose dependencies are missing
missing=0
for sym in MMU KUSER_HELPERS SND SND_PROC_FS SND_ARM SND_SC8830 ARCH_SPRD ARM_GIC SPRD_SC8830_INTC SPRD_SC8830_TIMER COMMON_CLK SMP HIGHMEM ARM_THUMB \
	ARM_APPENDED_DTB CMDLINE_FORCE DRM_SIMPLEDRM DRM_FBDEV_EMULATION \
	VT_CONSOLE FRAMEBUFFER_CONSOLE PSTORE_RAM PSTORE_CONSOLE KERNEL_XZ DRM_PANIC \
	BLK_DEV_INITRD COMPAT_32BIT_TIME MMC_BLOCK MMC_SDHCI_SPRD_SC8830 EFI_PARTITION \
	BLK_DEV_LOOP EXT4_FS VFAT_FS SQUASHFS SQUASHFS_XZ SQUASHFS_ZSTD OVERLAY_FS TMPFS_XATTR \
	INOTIFY_USER UNIX INPUT_EVDEV INPUT_UINPUT PM PM_GENERIC_DOMAINS SPRD_SC8830_GPU_PD \
	DRM_LIMA GPIO_SPRD I2C_SPRD_SC8830 REGULATOR_SC2723 TOUCHSCREEN_MELFAS_MIP4 KEYBOARD_SC8830 BACKLIGHT_SC8830_DSI \
	PWRSEQ_SIMPLE BATTERY_SC2723_FGU USB_DWC2 USB_DWC2_PERIPHERAL USB_ETH PHY_SC8830_USB I2C_GPIO CHARGER_SM5701 THERMAL SC8830_THERMAL SC8830_CPUCLK DEVMEM SERIAL_SPRD SERIAL_DEV_BUS FW_LOADER SPRD_SC2331_MARLIN SPRD_MARLIN_SDIO CFG80211 SPRD_SC2331_WLAN; do
	grep -qx "CONFIG_$sym=y" "$O/.config" || { echo "FALTA CONFIG_$sym" >&2; missing=1; }
done
[ $missing = 0 ] || exit 1

"${MK[@]}" -j"$(nproc)" zImage dtbs

# Fresh 32-byte /chosen/rng-seed per build. The kernel credits it at boot;
# with only a 32 kHz clocksource its jitter entropy never succeeds, so
# getrandom() would block forever (it stalled Batocera's syslog-ng).
cp "$DTB" "$OUT/board.dtb"
fdtput -t x "$OUT/board.dtb" /chosen rng-seed $(od -An -tx4 -N32 /dev/urandom)

cat "$O/arch/arm/boot/zImage" "$OUT/board.dtb" > "$OUT/zImage-dtb"
python3 -I ~/batocera-t280/scripts/mkgteximg.py \
	--kernel "$OUT/zImage-dtb" --dtb "$OUT/board.dtb" -o "$OUT/boot-gtex.img"
