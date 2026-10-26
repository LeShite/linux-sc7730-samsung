#!/bin/bash
# One TWRP session: pull the logs of the last run, install the overlay and /boot files, and
# optionally flash a kernel through Download mode (heimdall reboots the tablet afterwards).
#   install-all.sh NAME [KERNEL_IMAGE]      logs go to ~/batocera-t280/logs/NAME
set -u
D=~/batocera-t280/scripts
NAME=${1:-last}
IMG=${2:-}
[ -z "$IMG" ] || [ -f "$IMG" ] || { echo "no existe $IMG"; exit 1; }

NOREBOOT=1 "$D/push-overlay.sh" "$NAME" || exit 1
NOREBOOT=1 "$D/push-bootpart.sh" "$NAME" || exit 1
if [ -n "$IMG" ]; then
	echo "kernel: reiniciando en modo Download..."
	adb reboot download </dev/null
	"$D/autoflash.sh" KERNEL "$IMG"
else
	echo "reiniciando en Batocera..."
	adb reboot </dev/null
fi
