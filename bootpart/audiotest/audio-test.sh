#!/bin/bash
# One-shot monitor, started from boot-custom.sh when /boot/audiotest/run exists and removed here
# so it runs once per push. v43 went silent once 2048 started although the DMA still carried the
# game's sound (level meter 25-32 %): something after the DMA turned it down. For 15 minutes,
# every 5 s, log what changed: the card's controls (amixer) and the registers that decide the
# speaker's level (sysfs "adi": PMU0 PA_EN, CDC6 route, CDC10/11 gains, VBC DAHPCTL/ALC).
# Log: /boot/audiotest/audiotest.log
D=/boot/audiotest
[ -e $D/run ] || exit 0
rm -f $D/run
exec >>$D/audiotest.log 2>&1
up() { cut -d' ' -f1 /proc/uptime; }
echo "== $(date) monitor (controls + speaker registers), up=$(up)"

for i in $(seq 1 240); do [ -e /dev/snd/pcmC0D0p ] && break; sleep 0.5; done
ADI=$(ls /sys/bus/platform/devices/*.audio/adi 2>/dev/null | head -1)
last_c= last_r=
for i in $(seq 1 180); do
	c=$(amixer -c 0 contents 2>/dev/null | awk -F"'" '/name=/{n=$2} /: values=/{sub(/.*values=/,""); printf "%s=%s; ", n, $0}')
	r=$(grep -E '^(600|620|640)=|^0x0040 \||^powered' "$ADI" 2>/dev/null | tr '\n' ' ')
	emu=$(pgrep -f '[e]mulatorlauncher' >/dev/null && echo game || echo menu)
	if [ "$c" != "$last_c" ]; then echo "up=$(up) [$emu] controls: $c"; last_c=$c; fi
	if [ "$r" != "$last_r" ]; then echo "up=$(up) [$emu] regs: $r"; last_r=$r; fi
	sync
	sleep 5
done
echo "== $(date) monitor done, up=$(up)"; sync
