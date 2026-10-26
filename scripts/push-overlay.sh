#!/bin/bash
# From TWRP: install ~/batocera-t280/overlay.tar (made by make-overlay.py) into Batocera's
# /userdata (SHARE, /data in TWRP), then reboot into Batocera. One file over the flaky USB
#   push-overlay.sh [LOGDIR]   logs of the previous run go to ~/batocera-t280/logs/LOGDIR
# link, verified by sha256. Existing RetroArch per-system configs are saved first.
set -u
TAR=~/batocera-t280/overlay.tar
DST=/data/overlay.tar
[ -f "$TAR" ] || { echo "falta $TAR: corre scripts/make-overlay.py"; exit 1; }

echo "esperando la tablet en TWRP..."
timeout 1800 adb wait-for-recovery </dev/null || { echo "no apareció en 30 min"; exit 1; }
adb shell 'grep -q " /data " /proc/mounts || mount /data' </dev/null >/dev/null 2>&1
adb shell 'grep -q " /data " /proc/mounts' </dev/null || { echo "no se pudo montar /data (SHARE)"; exit 1; }

# Evidence of the previous run first: RetroArch's log, the config Batocera generated, launch logs
OUT=~/batocera-t280/logs/${1:-overlay-last}
mkdir -p "$OUT"
adb pull /data/system/logs "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  /data/system/logs -> $OUT"
for f in retroarchcustom.cfg overlay.cfg; do
	adb pull /data/system/configs/retroarch/$f "$OUT/" </dev/null >/dev/null 2>&1 && echo "log  $f"
done

# Keep whatever per-system RetroArch configs the user already had
adb shell 'ls /data/system/configs/retroarch/*.cfg 2>/dev/null | head -50' </dev/null | tr -d '\r' >~/batocera-t280/logs/retroarch-cfgs-before.txt
adb shell 'if [ -d /data/system/configs/retroarch ] && [ ! -d /data/system/configs/retroarch.bak-overlay ]; then cp -a /data/system/configs/retroarch /data/system/configs/retroarch.bak-overlay; fi' </dev/null >/dev/null 2>&1

want=$(sha256sum "$TAR" | cut -c1-64)
for try in 1 2 3 4 5; do
	adb push "$TAR" "$DST" </dev/null >/dev/null 2>&1 &&
	[ "$(adb shell "sha256sum $DST" </dev/null 2>/dev/null | cut -c1-64)" = "$want" ] && { echo "OK  overlay.tar verificado"; break; }
	echo "reintentando ($try)"; adb wait-for-recovery </dev/null
	[ $try = 5 ] && { echo "no se pudo copiar el overlay"; exit 1; }
done

adb shell "cd /data && tar -xf $DST && rm -f $DST" </dev/null

# EmulationStation's theme: carbon-touch (carbon with the pad drawn on top, from overlay.tar)
ES=/data/system/configs/emulationstation/es_settings.cfg
adb shell "[ -f $ES ] && [ ! -f $ES.bak-overlay ] && cp $ES $ES.bak-overlay
	if grep -q 'name=\"ThemeSet\"' $ES; then
		sed -i 's|<string name=\"ThemeSet\" value=\"[^\"]*\" */>|<string name=\"ThemeSet\" value=\"carbon-touch\" />|' $ES
	elif [ -f $ES ]; then
		sed -i 's|</config>|	<string name=\"ThemeSet\" value=\"carbon-touch\" />\n</config>|' $ES
	fi; grep ThemeSet $ES" </dev/null
n=$(adb shell 'ls /data/system/configs/retroarch/*.cfg 2>/dev/null | wc -l' </dev/null | tr -d '\r')
adb shell 'ls -la /data/system/touchpad/overlay/' </dev/null
echo "configs de RetroArch en la tablet: $n"
adb shell sync </dev/null
[ -n "${NOREBOOT:-}" ] && exit 0
echo "reiniciando en Batocera..."
adb reboot </dev/null
