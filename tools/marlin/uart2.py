import os, sys, time
sys.path.insert(0, "/tmp")
import uarttest as u
pk = open("/tmp/pskey.bin", "rb").read()
PSKEY = bytes([0x01, 0xa0, 0xfc, len(pk)]) + pk
u.gpio(46, 1); time.sleep(0.2)
fd = os.open("/dev/marlin-uart", os.O_RDWR)
print("pskey command", len(PSKEY), "bytes; ack", u.gpio(148))
for baud in (3000000, 115200, 921600, 1500000, 1000000, 460800, 2000000, 4000000):
    for flow in (False, True):
        u.try_(fd, baud, flow, PSKEY, "pskey")
u.sysw("raw", 0)
