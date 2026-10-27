#!/bin/bash
# Marlin WiFi/BT chip, once per boot: wait for /dev/marlin (kernel sprd-marlin-sdio, ~30 s after boot: it appears
# when FDL1 has run and the SDIO card enumerated), then load the firmware and calibrate it (marlin_up.py).
# Nothing may write to SDIO channel 3 before this: a stray byte makes the chip stop answering until the next boot.
L=/boot/marlin/marlin.log
{
	echo "== $(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) esperando /dev/marlin"
	for i in $(seq 1 240); do [ -e /dev/marlin ] && break; sleep 0.5; done
	if [ ! -e /dev/marlin ]; then echo "no aparecio /dev/marlin"; exit 1; fi
	cd /boot/marlin && python3 -u marlin_up.py
	echo "== fin rc=$? up=$(cut -d' ' -f1 /proc/uptime)"
} >>"$L" 2>&1
