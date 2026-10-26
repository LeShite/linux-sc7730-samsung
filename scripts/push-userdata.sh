#!/bin/bash
# Copy ~/batocera-t280/userdata/* into Batocera's /userdata (the SHARE
# partition, mounted as /data in TWRP), set the batocera.conf keys given as
# arguments, then reboot into Batocera. Small files only: the USB link is flaky.
#
# Usage: push-userdata.sh [key=value ...]   e.g. push-userdata.sh display.rotate=1
set -u
SRC=~/batocera-t280/userdata
CONF=/data/system/batocera.conf

echo "esperando la tablet en TWRP..."
timeout 600 adb wait-for-recovery || { echo "no apareció en 10 min"; exit 1; }

adb shell 'grep -q " /data " /proc/mounts || mount /data' >/dev/null 2>&1
adb shell 'grep -q " /data " /proc/mounts' || { echo "no se pudo montar /data (SHARE)"; exit 1; }

cd "$SRC" || exit 1
# adb shell reads stdin, so it must not see the file list: feed it /dev/null
find . -type f | while read -r f; do
	dst=/data/${f#./}
	for try in 1 2 3; do
		adb shell "mkdir -p '$(dirname "$dst")'" </dev/null >/dev/null 2>&1 &&
		adb push "$f" "$dst" </dev/null >/dev/null 2>&1 &&
		[ "$(adb shell "sha256sum '$dst'" </dev/null 2>/dev/null | cut -c1-64)" = "$(sha256sum "$f" | cut -c1-64)" ] &&
		{ echo "OK  $dst"; break; }
		echo "reintentando $dst"; adb wait-for-recovery </dev/null
	done
done
adb shell 'chmod 755 /data/system/custom.sh' >/dev/null 2>&1

for kv in "$@"; do
	key=${kv%%=*}
	adb shell "touch $CONF; grep -v '^$key=' $CONF > $CONF.new; echo '$kv' >> $CONF.new; mv $CONF.new $CONF" &&
		echo "OK  batocera.conf: $kv"
done

adb shell sync
echo "reiniciando en Batocera..."
adb reboot
