#!/bin/sh
# n64test.sh NAME CORE : launch Mario Kart 64 with the given mupen64plus video plugin, look at it, close it
NAME=$1; CORE=$2; OUT=/tmp/n64-$NAME; mkdir -p $OUT
batocera-settings-set n64.core $CORE
ROM="/userdata/roms/n64/Mario Kart 64 (USA).z64"
curl -s -m 5 -X POST -d "$ROM" http://localhost:1234/launch >/dev/null
sleep 25
echo "== $NAME ($CORE): $(curl -s -m 3 http://localhost:1234/runningGame | head -c 200)"
PID=$(pgrep -n mupen64plus)
echo "pid mupen64plus: $PID"
cd /boot/touchpad && python3 shot.py $OUT/shot-25s.png landscape
python3 shot.py fps
a=$(awk '{print $14+$15}' /proc/$PID/stat); b=$(head -1 /proc/stat | awk '{print $2+$3+$4+$5+$6+$7+$8}'); sleep 4; c=$(awk '{print $14+$15}' /proc/$PID/stat); d=$(head -1 /proc/stat | awk '{print $2+$3+$4+$5+$6+$7+$8}')
echo "CPU de mupen64plus: $(( (c-a)*100/(d-b) ))% de TODA la CPU (4 nucleos=100%)"
sleep 15
python3 shot.py $OUT/shot-40s.png landscape
python3 shot.py fps
a=$(awk '{print $14+$15}' /proc/$PID/stat); b=$(head -1 /proc/stat | awk '{print $2+$3+$4+$5+$6+$7+$8}'); sleep 4; c=$(awk '{print $14+$15}' /proc/$PID/stat); d=$(head -1 /proc/stat | awk '{print $2+$3+$4+$5+$6+$7+$8}')
echo "CPU de mupen64plus: $(( (c-a)*100/(d-b) ))% de TODA la CPU (4 nucleos=100%)"
cat /sys/class/power_supply/battery/current_now
tail -25 /userdata/system/logs/es_launch_stdout.log > $OUT/stdout.txt
batocera-es-swissknife --emukill >/dev/null 2>&1
sleep 8
echo "after kill: $(curl -s -m 3 http://localhost:1234/runningGame | head -c 80)"
