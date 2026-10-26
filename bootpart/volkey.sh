#!/bin/bash
# Volume buttons (triggerhappy): one step of the hardware gain of the output in use.
#   volkey.sh up|down
# Speaker: "Speaker Playback Volume" 0-8 (3 dB steps). Headphones plugged: "Aux Gain" 0-15.
ctl='Speaker Playback Volume'
[ "$(amixer -c 0 cget name='Aux Jack Flag' 2>/dev/null | sed -n 's/.*: values=//p')" = on ] && ctl='Aux Gain'
cur=$(amixer -c 0 cget name="$ctl" 2>/dev/null | sed -n 's/.*: values=//p')
max=$(amixer -c 0 cget name="$ctl" 2>/dev/null | sed -n 's/.*max=\([0-9]*\).*/\1/p')
[ -n "$cur" ] || exit 0
case $1 in
up)   new=$((cur + 1)); [ $new -gt "${max:-8}" ] && new=${max:-8} ;;
down) new=$((cur - 1)); [ $new -lt 0 ] && new=0 ;;
*)    exit 1 ;;
esac
amixer -c 0 -q cset name="$ctl" $new
