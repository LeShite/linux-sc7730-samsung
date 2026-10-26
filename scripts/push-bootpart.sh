#!/bin/bash
# From TWRP: first pull the logs of whatever ran last on the tablet, then copy
# ~/batocera-t280/bootpart/* to the root of the BATOCERA partition
# (mmcblk0p25, Batocera's /boot), verify, and reboot into Batocera.
#   push-bootpart.sh [NAME]     logs go to ~/batocera-t280/logs/NAME (default touch-last)
set -u
SRC=~/batocera-t280/bootpart
MNT=/tmp/bato
OUT=~/batocera-t280/logs/${1:-touch-last}

echo "esperando la tablet en TWRP..."
timeout 1800 adb wait-for-recovery </dev/null || { echo "no apareció en 30 min"; exit 1; }

adb shell "mkdir -p $MNT; grep -q ' $MNT ' /proc/mounts || mount -t ext4 /dev/block/mmcblk0p25 $MNT" </dev/null >/dev/null 2>&1
adb shell "grep -q ' $MNT ' /proc/mounts" </dev/null || { echo "no se pudo montar BATOCERA (p25)"; exit 1; }
adb shell 'grep -q " /data " /proc/mounts || mount /data' </dev/null >/dev/null 2>&1

# Logs first: they are the point of the previous boot
mkdir -p "$OUT"
for f in touchpad.log trace.log dmesg.txt; do
	adb pull "$MNT/$f" "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  $f ($(wc -c <"$OUT/$f") bytes)"
done
adb pull "$MNT/lastgame" "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  lastgame (log del último juego)"
adb pull /data/system/logs "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  /data/system/logs"
echo "logs en $OUT"

cd "$SRC" || exit 1
find . -type f | while read -r f; do
	dst=$MNT/${f#./}
	for try in 1 2 3; do
		adb shell "mkdir -p '$(dirname "$dst")'" </dev/null >/dev/null 2>&1 &&
		adb push "$f" "$dst" </dev/null >/dev/null 2>&1 &&
		[ "$(adb shell "sha256sum '$dst'" </dev/null 2>/dev/null | cut -c1-64)" = "$(sha256sum "$f" | cut -c1-64)" ] &&
		{ echo "OK  $dst"; break; }
		echo "reintentando $dst"; adb wait-for-recovery </dev/null
	done
done

# The old /userdata launcher would start a second copy if S99 ever ran
adb shell 'rm -f /data/system/custom.sh' </dev/null >/dev/null 2>&1 &&
	echo "OK  quitado /data/system/custom.sh"

adb shell "sync; umount $MNT" </dev/null
[ -n "${NOREBOOT:-}" ] && exit 0
echo "reiniciando en Batocera..."
adb reboot </dev/null
