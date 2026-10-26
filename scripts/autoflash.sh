#!/bin/bash
# Wait for the tablet in Download mode, require a few seconds of stable USB,
# then flash. Meant for the flaky micro-USB port of this SM-T280.
# Usage: autoflash.sh PARTITION IMAGE [extra heimdall args]
part=$1 img=$2
shift 2
[ -f "$img" ] || { echo "no existe $img"; exit 1; }

deadline=$((SECONDS + 600))
while [ $SECONDS -lt $deadline ]; do
	stable=0
	while lsusb -d 04e8:685d >/dev/null 2>&1; do
		stable=$((stable + 1))
		[ $stable -ge 3 ] && break
		sleep 1
	done
	if [ $stable -ge 3 ]; then
		echo "$(date +%T) estable 3 s, flasheando $img en $part..."
		heimdall flash --"$part" "$img" "$@" 2>&1 | grep -E 'upload|ERROR|successful|Failed'
		[ "${PIPESTATUS[0]}" = 0 ] && { echo "$(date +%T) FLASHEO OK"; exit 0; }
		echo "$(date +%T) falló, reintentando cuando vuelva la conexión"
	fi
	sleep 1
done
echo "no se pudo flashear en 10 minutos"
exit 1
