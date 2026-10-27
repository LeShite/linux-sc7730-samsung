"""Raw talk to the Marlin's UART0 through /dev/marlin-uart (kernel v70), to find how its Bluetooth answers.
Run on the tablet with Bluetooth OFF. Sends an HCI Reset (01 03 0c 00) under different line settings."""
import mmap, os, struct, sys, time

SYS = "/sys/devices/virtual/misc/marlin-uart/"
HCI_RESET = bytes([0x01, 0x03, 0x0c, 0x00])
READ_LOCAL_VERSION = bytes([0x01, 0x01, 0x10, 0x00])

def sysw(name, val):
    with open(SYS + name, "w") as f:
        f.write(str(val))

def gpio(pin, val=None):
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x40280000)
    b, t = divmod(pin, 16)
    base = b * 0x80
    rd = lambda o: struct.unpack_from("<H", m, o)[0]
    wr = lambda o, v: struct.pack_into("<H", m, o, v)
    wr(base + 4, rd(base + 4) | (1 << t))
    if val is not None:
        wr(base + 8, rd(base + 8) | (1 << t))
        wr(base, (rd(base) | (1 << t)) if val else (rd(base) & ~(1 << t)))
    return (rd(base) >> t) & 1

def drain(fd):
    out = b""
    while True:
        d = os.read(fd, 4096)
        if not d:
            return out
        out += d

def try_(fd, baud, flow, pkt=HCI_RESET, label=""):
    sysw("raw", 0); sysw("baud", baud); sysw("flow", 1 if flow else 0); sysw("raw", 1)
    time.sleep(0.05)
    os.write(fd, pkt)
    got = drain(fd)
    print("%-10s baud %-8d flow %d -> %s" % (label, baud, flow, got.hex() or "(nada)"), flush=True)
    return got

if __name__ == "__main__":
    print("wake_out=1 ->", gpio(46, 1), " ack:", gpio(148))
    time.sleep(0.2)
    print("ack after wake:", gpio(148))
    fd = os.open("/dev/marlin-uart", os.O_RDWR)
    for baud in (3000000, 115200, 921600, 1500000, 1000000, 460800):
        for flow in (False, True):
            try_(fd, baud, flow, HCI_RESET, "reset")
    sysw("raw", 0)
