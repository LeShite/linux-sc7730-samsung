#!/bin/bash
# Battery watch (started from boot-custom.sh): power off cleanly before the battery runs flat.
# The kernel's sc2723-fgu driver is /sys/class/power_supply/battery. Every minute, logs a line to
# /boot/battery.log every 10 minutes, and powers off at <= 3 % while discharging, only if the voltage
# agrees (< 3600 mV): a wrong percentage alone never shuts the tablet down.
B=/sys/class/power_supply/battery
LOG=/boot/battery.log
LOW=3
[ -d $B ] || exit 0
n=0
while true; do
	cap=$(cat $B/capacity 2>/dev/null); st=$(cat $B/status 2>/dev/null)
	mv=$(( $(cat $B/voltage_now 2>/dev/null || echo 0) / 1000 )); ma=$(( $(cat $B/current_now 2>/dev/null || echo 0) / 1000 ))
	[ $((n % 10)) = 0 ] && echo "$(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) ${cap}% $st ${mv} mV ${ma} mA" >>$LOG
	if [ "$st" = Discharging ] && [ -n "$cap" ] && [ "$cap" -le $LOW ] && [ "$mv" -gt 0 ] && [ "$mv" -lt 3600 ]; then
		echo "$(date '+%F %T') battery ${cap}% (${mv} mV): poweroff" >>$LOG
		sync
		/sbin/poweroff
		exit 0
	fi
	n=$((n + 1))
	sleep 60
done
