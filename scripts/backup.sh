#!/bin/bash
# Resumable backup of the SM-T280's small partitions over adb (TWRP, root).
# Each image is verified against sha256sum computed on the device; verified
# names are recorded in VERIFIED so a re-run after a USB drop skips them.
B=~/batocera-t280/backup
cd "$B" || exit 1
touch VERIFIED

PARTS="SBOOT SBOOT2 wfixnv1 wfixnv2 tdfixnv1 tdfixnv2 WDSP MODEM MODEM2 TDDSP
FOTA_SIG wruntimenv1 wruntimenv2 tdruntimenv1 tdruntimenv2 PARAM efs prodnv
RESERVED2 KERNEL PERSISTENT persdata mmcblk0boot0 mmcblk0boot1"

echo "esperando la tablet en TWRP..."
timeout 600 adb wait-for-recovery || { echo "no apareció en 10 min"; exit 1; }

for n in $PARTS; do
	grep -qx "$n" VERIFIED && continue
	case $n in
	mmcblk0boot*) dev=/dev/block/$n ;;
	*) dev=/dev/block/platform/sdio_emmc/by-name/$n ;;
	esac
	adb pull "$dev" "$n.img" >/dev/null 2>&1 || { echo "FALLÓ $n (¿se cortó el USB?)"; exit 2; }
	remote=$(adb shell "sha256sum $dev" 2>/dev/null | tr -d '\r' | awk '{print $1}')
	local_=$(sha256sum "$n.img" | awk '{print $1}')
	if [ -n "$remote" ] && [ "$remote" = "$local_" ]; then
		echo "$n" >> VERIFIED
		printf "OK  %-14s %9s bytes\n" "$n" "$(stat -c %s "$n.img")"
	else
		echo "MAL $n: checksum distinto"; rm -f "$n.img"; exit 3
	fi
done
sha256sum *.img > SHA256SUMS
echo "RESPALDO COMPLETO: $(wc -l < VERIFIED) particiones verificadas"
