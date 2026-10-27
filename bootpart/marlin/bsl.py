"""BSL frames (Spreadtrum) over /dev/marlin: write [chn]+frame, read [chn]+data. Run on the tablet."""
import os, struct, sys, time, select

def crc16(d):
    c = 0
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c

def frame(typ, data=b""):
    body = struct.pack(">HH", typ, len(data)) + data
    body += struct.pack(">H", crc16(body))
    out = bytearray([0x7e])
    for b in body:
        if b in (0x7d, 0x7e):
            out += bytes([0x7d, b ^ 0x20])
        else:
            out.append(b)
    out.append(0x7e)
    return bytes(out)

def unframe(raw):
    raw = raw.strip(b"\x7e")
    out, esc = bytearray(), False
    for b in raw:
        if esc:
            out.append(b ^ 0x20); esc = False
        elif b == 0x7d:
            esc = True
        else:
            out.append(b)
    return bytes(out)

fd = os.open("/dev/marlin", os.O_RDWR)

def send(chn, data):
    return os.write(fd, bytes([chn]) + data)

def recv(timeout=6):
    try:
        r = os.read(fd, 70000)
    except OSError as e:
        return None, str(e)
    return r[0], r[1:]

def status():
    return open("/sys/devices/virtual/misc/marlin/status").read()
