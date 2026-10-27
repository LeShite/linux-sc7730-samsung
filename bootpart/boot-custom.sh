#!/bin/bash
# Batocera runs this first thing at boot (S00bootcustom), with "start".
# SM-T280: start the touchscreen gamepad from here, because custom.sh in
# /userdata (S99userservices) never runs: a service before it hangs.
# While debugging, log to /boot (the BATOCERA partition), which TWRP can read:
#   touchpad.log  the gamepad program      trace.log  what runs, every 3 s
#   dmesg.txt     kernel log, refreshed every 30 s

LOG=/boot/touchpad.log
TRACE=/boot/trace.log

case "$1" in
start)
	mount -o remount,rw /boot
	# before S06audio starts PipeWire: pin it to 44.1 kHz (see /boot/pipewire/10-rate.conf)
	mkdir -p /etc/pipewire/pipewire.conf.d && cp /boot/pipewire/10-rate.conf /etc/pipewire/pipewire.conf.d/
	# S12populateshare deletes ~/.asoundrc and /boot/asoundrc at every boot, so the ALSA config goes into
	# /etc/asound.conf instead (now Batocera's own, stereo, for the headphones)
	cp /boot/asound.conf /etc/asound.conf
	# CPU speed (kernel sc8830-cpuclk sets voltage and bus too); chosen in the Home menu, 1482 MHz by default
	[ -w /sys/devices/platform/soc/40030000.cpuclk/freq_mhz ] &&
		echo "$(cat /boot/cpu_mhz 2>/dev/null || echo 1482)" >/sys/devices/platform/soc/40030000.cpuclk/freq_mhz
	echo "$(date '+%F %T') boot-custom.sh start, python3=$(command -v python3)" >>"$LOG"
	# Detached: whatever stays in the boot scripts' session is killed later
	nohup setsid python3 -u /boot/touchpad/touchpad.py </dev/null >>"$LOG" 2>&1 &
	echo "$(date '+%F %T') lanzado, pid $!" >>"$LOG"
	nohup setsid bash /boot/boot-custom.sh trace </dev/null >/dev/null 2>&1 &
	# power off cleanly before the battery runs flat (kernel sc2723-fgu)
	nohup setsid bash /boot/batwatch.sh </dev/null >/dev/null 2>&1 &
	# USB network to the PC: 10.42.7.2 on usb0 (kernel dwc2 + g_ether), ssh root@10.42.7.2
	nohup setsid bash /boot/usbnet.sh </dev/null >/dev/null 2>&1 &
	# WiFi chip: firmware + calibration over SDIO once /dev/marlin shows up (see /boot/marlin/marlin-up.sh)
	nohup setsid bash /boot/marlin/marlin-up.sh </dev/null >/dev/null 2>&1 &
	[ -e /boot/audiotest/run ] && nohup setsid bash /boot/audiotest/audio-test.sh </dev/null >/dev/null 2>&1 &
	;;
trace)
	: >"$TRACE"
	for i in $(seq 1 12); do
		{
			echo "== $(date '+%T') up=$(cut -d' ' -f1 /proc/uptime)"
			ps -eo pid,stat,etime,args | grep -E 'init.d|rcS|touchpad|emulationstation' | grep -v grep
			echo "input: $(ls /dev/input 2>&1 | tr '\n' ' ') uinput: $(ls /dev/uinput 2>&1)"
		} >>"$TRACE"
		[ $((i % 6)) = 1 ] && dmesg >/boot/dmesg.txt
		sleep 5
	done
	dmesg >/boot/dmesg.txt
	sync
	# then every 30 s for 30 minutes: what happened on a cable pull/plug is readable from TWRP
	for i in $(seq 1 60); do
		sleep 30
		dmesg >/boot/dmesg.txt
		sync
	done
	;;
stop)
	pkill -f touchpad/touchpad.py
	;;
esac
