#!/bin/bash
# USB network (kernel: dwc2 + g_ether, CDC ECM): the tablet is 10.42.7.2 on usb0, the PC 10.42.7.1.
# SSH: ssh root@10.42.7.2 (Batocera's dropbear). Keeps the address in place every few seconds.
#
# Cable pulls: the board gives dwc2 no VBUS sense, so after a pull and a new plug the controller
# never answers the PC again. The vendor re-initialised the PHY and the controller on every plug;
# here, when the gadget has not been "configured" by a host for HEAL seconds, unbind and bind dwc2
# (a full PHY power cycle and core reset), then wait and try again. Harmless with no cable.
# Log: /boot/usbnet.log
IP=10.42.7.2/24
LOG=/boot/usbnet.log
UDC=20200000.usb
DRV=/sys/bus/platform/drivers/dwc2
HEAL=8
last= bad=0 heals=0
log() { echo "$(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) $*" >>$LOG; }

while true; do
	if [ -d /sys/class/net/usb0 ]; then
		ip link set usb0 up 2>/dev/null
		ip -4 addr show dev usb0 | grep -q "${IP%/*}" || ip addr add $IP dev usb0 2>/dev/null
	fi
	st=$(cat /sys/class/udc/$UDC/state 2>/dev/null || echo "no udc")
	now="udc $st, usb0 $(cat /sys/class/net/usb0/operstate 2>/dev/null || echo none)"
	[ "$now" != "$last" ] && { log "$now"; last=$now; }

	if [ "$st" = configured ]; then
		bad=0 heals=0
	else
		bad=$((bad + 2))
		# after three tries (no cable, most likely) only once a minute, and quietly
		if [ $bad -ge $HEAL ] && { [ $heals -lt 3 ] || [ $bad -ge 60 ]; }; then
			[ $heals -lt 3 ] && log "not configured for ${bad} s: rebinding dwc2"
			echo $UDC >$DRV/unbind 2>/dev/null
			sleep 1
			echo $UDC >$DRV/bind 2>/dev/null
			heals=$((heals + 1))
			bad=-6		# give the host time to enumerate before trying again
		fi
	fi
	sleep 2
done
