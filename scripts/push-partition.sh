#!/bin/bash
# Write a filesystem image to a raw eMMC partition through TWRP's adb, in
# verified chunks, so that the flaky micro-USB link can drop at any time.
# Every chunk is checked on the device first (sha256 of that region), so a
# re-run skips what is already in place and resumes where it stopped.
#
# Usage: push-partition.sh IMAGE PARTNAME   e.g. push-partition.sh boot.ext4 SYSTEM
set -u
img=$1 part=/dev/block/platform/sdio_emmc/by-name/$2
CHUNK_MB=32
size=$(stat -c %s "$img")
chunks=$(( (size + CHUNK_MB * 1048576 - 1) / (CHUNK_MB * 1048576) ))
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

devsh() { timeout 120 adb shell "$@" 2>/dev/null | tr -d '\r'; }

wait_device() {
	until [ "$(adb get-state 2>/dev/null)" = recovery ]; do
		echo "$(date +%T) esperando la tablet en TWRP (reconecta el cable)..."
		timeout 600 adb wait-for-recovery || return 1
	done
}

# Busybox dd in TWRP has a 32-bit off_t: stay below 2 GiB. Whole MiB only,
# so every chunk can be read back with plain dd (no head -c in old busybox)
[ "$size" -lt 2147483648 ] || { echo "imagen >= 2 GiB, no soportado"; exit 1; }
[ $((size % 1048576)) = 0 ] || { echo "el tamaño de la imagen debe ser múltiplo de 1 MiB"; exit 1; }

wait_device || exit 1
psize=$(devsh "cat /sys/class/block/\$(basename \$(readlink $part))/size")
psize=$(( ${psize:-0} * 512 ))
[ "$psize" -ge "$size" ] || { echo "la partición ($psize) es menor que la imagen ($size)"; exit 1; }
devsh "umount /system /data /cache 2>/dev/null; true" >/dev/null

i=0 tries=0
while [ $i -lt $chunks ]; do
	wait_device || exit 1
	tries=$((tries + 1))
	[ $tries -le 6 ] || { echo "el bloque $((i + 1)) no se pudo escribir tras 5 intentos"; exit 1; }
	dd if="$img" of="$work/c" bs=1M skip=$((i * CHUNK_MB)) count=$CHUNK_MB status=none
	n=$(stat -c %s "$work/c")
	want=$(sha256sum "$work/c" | cut -c1-64)
	have=$(devsh "dd if=$part bs=1M skip=$((i * CHUNK_MB)) count=$((n / 1048576)) 2>/dev/null | sha256sum" | cut -c1-64)
	if [ "$have" = "$want" ]; then
		printf '%s bloque %3d/%d ya está bien\n' "$(date +%T)" $((i + 1)) $chunks
		i=$((i + 1)) tries=0
		continue
	fi
	if ! adb push "$work/c" /tmp/chunk >/dev/null 2>&1; then
		echo "$(date +%T) se cortó el USB durante el envío, reintentando"
		continue
	fi
	devsh "dd if=/tmp/chunk of=$part bs=1M seek=$((i * CHUNK_MB)) conv=notrunc 2>/dev/null; sync; rm -f /tmp/chunk" >/dev/null
	# loop back to the verification of the same chunk
done
devsh "sync" >/dev/null
echo "$(date +%T) LISTO: $img escrita y verificada en $part ($chunks bloques)"
