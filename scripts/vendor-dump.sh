#!/bin/bash
# From TWRP (Samsung's 3.10 kernel): play dual.wav through the vendor's sound card with Android's
# routes (speaker, then headphones) and dump every audio register while it plays, into
# ~/batocera-t280/logs/NAME. Uses tools/tplay (no libc). Leaves the tablet in TWRP.
#   vendor-dump.sh NAME
set -u
R=~/batocera-t280
OUT=$R/logs/${1:-vendor-dump}
echo "esperando la tablet en TWRP..."
timeout 1800 adb wait-for-recovery </dev/null || { echo "no apareció en 30 min"; exit 1; }
mkdir -p "$OUT"
adb push $R/tools/tplay/tplay /tmp/tplay </dev/null >/dev/null && adb push $R/bootpart/audiotest/dual.wav /tmp/dual.wav </dev/null >/dev/null || exit 1
dump() {	# dump NAME ROUTE...: set the controls, play, dump in the middle of it
	local name=$1; shift
	adb shell "T=/tmp/tplay; chmod 755 \$T; for c in $*; do \$T 2 set \"\${c%:*}\" \"\${c##*:}\" >/dev/null; done
		(sleep 3; for f in sprd-codec vbc aud-glb; do echo \"== \$f\"; cat /proc/asound/sprdphone/\$f; done >/tmp/dump.txt) &
		\$T 2 play 0 /tmp/dual.wav; sleep 1; cat /tmp/dump.txt" </dev/null >"$OUT/$name.txt" 2>&1
	echo "dump $name: $(wc -l <"$OUT/$name.txt") líneas"
}
COMMON="'HPL Playback Volume:8' 'HPR Playback Volume:8' 'HPL CG Playback Volume1:63' 'HPR CG Playback Volume1:63' 'HPL CG Playback Volume2:7' 'HPR CG Playback Volume2:7' 'Inter PA Config:304' 'Inter HP PA Config:7489'"
dump speaker "$COMMON" "'HeadPhone Function:0'" "'HPL Mixer DACLHPL Switch:0'" "'HPR Mixer DACRHPR Switch:0'" "'SPKL Playback Volume:10'" "'DACL Playback Volume:5'" "'DACR Playback Volume:5'" "'SPKL Mixer DACLSPKL Switch:1'" "'SPKL Mixer DACRSPKL Switch:1'" "'Speaker Function:1'"
dump headphone "$COMMON" "'Speaker Function:0'" "'SPKL Mixer DACLSPKL Switch:0'" "'SPKL Mixer DACRSPKL Switch:0'" "'DACL Playback Volume:7'" "'DACR Playback Volume:7'" "'HPL Mixer DACLHPL Switch:1'" "'HPR Mixer DACRHPR Switch:1'" "'HeadPhone Function:1'"
adb shell '/tmp/tplay 2 list' </dev/null >"$OUT/controls.txt" 2>&1
echo "listo: $OUT (la tablet sigue en TWRP)"
