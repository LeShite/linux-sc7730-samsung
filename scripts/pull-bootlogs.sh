#!/bin/bash
# From TWRP: pull the debug logs of the last Batocera boot (touchpad.log, trace.log,
# dmesg.txt from /boot, plus /data/system/logs) into ~/batocera-t280/logs/NAME.
# Installs nothing and leaves the tablet in TWRP.
#   pull-bootlogs.sh NAME
set -u
MNT=/tmp/bato
OUT=~/batocera-t280/logs/${1:-touch-last}

echo "esperando la tablet en TWRP..."
timeout 1800 adb wait-for-recovery </dev/null || { echo "no apareció en 30 min"; exit 1; }

adb shell "mkdir -p $MNT; grep -q ' $MNT ' /proc/mounts || mount -t ext4 /dev/block/mmcblk0p25 $MNT" </dev/null >/dev/null 2>&1
adb shell "grep -q ' $MNT ' /proc/mounts" </dev/null || { echo "no se pudo montar BATOCERA (p25)"; exit 1; }
adb shell 'grep -q " /data " /proc/mounts || mount /data' </dev/null >/dev/null 2>&1

mkdir -p "$OUT"
for f in touchpad.log trace.log dmesg.txt audiotest/audiotest.log; do
	for try in 1 2 3; do
		adb pull "$MNT/$f" "$OUT/" </dev/null >/dev/null 2>&1 && { echo "log  $f ($(wc -c <"$OUT/$(basename "$f")") bytes)"; break; }
		adb wait-for-recovery </dev/null
	done
done
adb pull "$MNT/lastgame" "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  lastgame (log del último juego)"
adb pull /data/system/logs "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  /data/system/logs"
adb shell "umount $MNT" </dev/null >/dev/null 2>&1
echo "listo: $OUT"
