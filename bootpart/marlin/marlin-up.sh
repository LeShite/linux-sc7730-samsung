#!/bin/bash
# Marlin WiFi/BT chip, once per boot: wait for /dev/marlin (kernel sprd-marlin-sdio, ~30 s after boot: it appears
# when FDL1 has run and the SDIO card enumerated), then load the firmware and calibrate it (marlin_up.py).
# Nothing may write to SDIO channel 3 before this: a stray byte makes the chip stop answering until the next boot.
L=/boot/marlin/marlin.log
{
	echo "== $(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) esperando /dev/marlin"
	for i in $(seq 1 240); do [ -e /dev/marlin ] && break; sleep 0.5; done
	if [ ! -e /dev/marlin ]; then echo "no aparecio /dev/marlin"; exit 1; fi
	# TEST: wake_out (GPIO 46) must be LOW while the firmware boots, as in the vendor driver
	python3 - <<'PY'
import mmap, os, struct
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x40280000)
b, t = divmod(46, 16)
d = struct.unpack_from("<H", m, b * 0x80)[0]
struct.pack_into("<H", m, b * 0x80, d & ~(1 << t))
PY
	cd /boot/marlin && python3 -u marlin_up.py || { echo "marlin_up fallo"; exit 1; }
	# firmware running and calibrated: register wlan0 (kernel sprd-sc2331-wlan)
	# a MAC of our own, made once and kept (locally administered, so it cannot clash with a real one)
	if ! grep -qE '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' /boot/marlin/mac 2>/dev/null; then
		od -An -N5 -tx1 /dev/urandom | awk '{printf "02:%s:%s:%s:%s:%s\n", $1, $2, $3, $4, $5}' >/boot/marlin/mac
	fi
	echo "1 $(cat /boot/marlin/mac)" >/sys/devices/virtual/misc/marlin/wlan && echo "wlan0 registrada, MAC $(cat /boot/marlin/mac)"
	# Batocera's connman started before wlan0 existed and left WiFi switched off: switch it on now,
	# if the user has WiFi enabled in the Batocera menu (wifi.enabled=1 in batocera.conf)
	if [ "$(batocera-settings-get -f /userdata/system/batocera.conf wifi.enabled 2>/dev/null)" = "1" ]; then
		for i in $(seq 1 20); do connmanctl state >/dev/null 2>&1 && break; sleep 1; done
		connmanctl enable wifi 2>&1
		connmanctl scan wifi 2>&1
		echo "connman: WiFi activado"
	fi
	echo "== fin rc=$? up=$(cut -d' ' -f1 /proc/uptime)"
} >>"$L" 2>&1
