import os, sys, time
sys.path.insert(0, "/tmp")
import uarttest as u
while not os.path.exists("/dev/marlin"):
    time.sleep(0.2)
t0 = time.time()
u.sysw("baud", 3000000); u.sysw("flow", 0); u.sysw("raw", 1)
fd = os.open("/dev/marlin-uart", os.O_RDWR)
n = 0
while time.time() - t0 < 100:
    os.write(fd, bytes([0x01, 0x03, 0x0c, 0x00]))
    d = b""
    try:
        import select
        r, _, _ = select.select([fd], [], [], 0.25)
        if r: d = os.read(fd, 4096)
    except Exception as e:
        d = b""
    n += 1
    if d:
        print("t+%.1fs RX %s" % (time.time() - t0, d.hex()), flush=True)
print("fin, %d intentos" % n, flush=True)
u.sysw("raw", 0)
