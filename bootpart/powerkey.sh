#!/bin/bash
# Power button (triggerhappy, /userdata/system/configs/multimedia_keys.conf):
#   short press: sleep / wake (screen off, speaker muted, the running game frozen, touches ignored)
#   hold 2 s:    power off
#   powerkey.sh 1 (pressed) | 0 (released)
# poweroff ends in the kernel's SC2723 power-off (every LDO/DC-DC down). If a service hangs on the
# way down, force it after 25 s.
FLAG=/var/run/powerkey.down
SLEEP=/var/run/tablet-sleep		# touchpad.py ignores touches while it exists
BL=/sys/class/backlight/panel/brightness
HOLD=2
LOG=/boot/touchpad.log
EMUS='retroarch|ppsspp|mupen64plus|flycast|dosbox|scummvm|prboom|duckstation|pcsx2'

emu_pids() { pgrep -f "$EMUS" 2>/dev/null; }

go_sleep() {
	cat $BL >$SLEEP.brightness 2>/dev/null
	amixer -c 0 cget name='Speaker Playback Switch' | sed -n 's/.*: values=//p' >$SLEEP.speaker
	touch $SLEEP
	echo 0 >$BL
	amixer -c 0 -q cset name='Speaker Playback Switch' off
	emu_pids >$SLEEP.pids
	[ -s $SLEEP.pids ] && kill -STOP $(cat $SLEEP.pids)
	echo "$(date '+%F %T') sleep (game pids: $(tr '\n' ' ' <$SLEEP.pids))" >>$LOG
}

wake_up() {
	[ -s $SLEEP.pids ] && kill -CONT $(cat $SLEEP.pids) 2>/dev/null
	amixer -c 0 -q cset name='Speaker Playback Switch' "$(cat $SLEEP.speaker 2>/dev/null || echo on)"
	b=$(cat $SLEEP.brightness 2>/dev/null); [ "${b:-0}" -gt 0 ] || b=143
	echo $b >$BL
	rm -f $SLEEP $SLEEP.pids
	echo "$(date '+%F %T') wake" >>$LOG
}

power_off() {
	echo "$(date '+%F %T') poweroff ($1)" >>$LOG
	[ -e $SLEEP ] && wake_up
	sync
	( sleep 25; echo "$(date '+%F %T') poweroff stuck: forcing" >>$LOG; sync; /sbin/poweroff -f ) &
	/sbin/poweroff
}

# from the Home menu (touchpad.py): no button to time
case "$1" in
sleep) if [ -e $SLEEP ]; then wake_up; else go_sleep; fi; exit 0 ;;
off)   power_off menu; exit 0 ;;
esac

if [ "$1" = 1 ]; then
	touch "$FLAG"
	sleep $HOLD
	# still held: whoever removes the flag acts
	if [ -e "$FLAG" ] && rm "$FLAG" 2>/dev/null; then
		power_off "held ${HOLD} s"
	fi
else
	# released before the hold time: a short press
	if [ -e "$FLAG" ] && rm "$FLAG" 2>/dev/null; then
		if [ -e $SLEEP ]; then wake_up; else go_sleep; fi
	fi
fi
