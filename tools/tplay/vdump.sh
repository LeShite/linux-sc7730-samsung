#!/sbin/sh
# On TWRP: vdump.sh speaker|headphone -> play dual.wav with Android's route, dump registers mid-play
T=/tmp/tplay
s() { $T 2 set "$1" "$2" >/dev/null; }
s "HPL Playback Volume" 8; s "HPR Playback Volume" 8
s "HPL CG Playback Volume1" 63; s "HPR CG Playback Volume1" 63
s "HPL CG Playback Volume2" 7; s "HPR CG Playback Volume2" 7
s "Inter PA Config" 304; s "Inter HP PA Config" 7489
if [ "$1" = speaker ]; then
	s "HeadPhone Function" 0; s "HPL Mixer DACLHPL Switch" 0; s "HPR Mixer DACRHPR Switch" 0
	s "SPKL Playback Volume" 10; s "DACL Playback Volume" 5; s "DACR Playback Volume" 5
	s "SPKL Mixer DACLSPKL Switch" 1; s "SPKL Mixer DACRSPKL Switch" 1; s "Speaker Function" 1
else
	s "Speaker Function" 0; s "SPKL Mixer DACLSPKL Switch" 0; s "SPKL Mixer DACRSPKL Switch" 0
	s "DACL Playback Volume" 7; s "DACR Playback Volume" 7
	s "HPL Mixer DACLHPL Switch" 1; s "HPR Mixer DACRHPR Switch" 1; s "HeadPhone Function" 1
fi
(sleep 3; for f in sprd-codec vbc aud-glb; do echo "== $f"; cat /proc/asound/sprdphone/$f; done >/tmp/dump.txt) &
$T 2 play 0 /tmp/dual.wav
sleep 1
cat /tmp/dump.txt
