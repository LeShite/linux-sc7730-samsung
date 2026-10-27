"""Load sc2331_fw.bin into the Marlin over SDIO channel 3 (frames of Samsung's `download`, SDIO mode).
Header = 4 little-endian halfwords: h0 length, h1 type (MIDST also carries len>>16 in the high byte),
h2 checksum of the payload, h3 checksum of the first 6 header bytes; checksum = one's-complement sum16."""
import os, struct, sys, time
import bsl

def csum(b):
    s = 0
    for i in range(0, len(b) - 1, 2):
        s += b[i] | b[i + 1] << 8
    if len(b) & 1:
        s += b[-1]
    s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff

def hdr(h0, h1, h2):
    h = struct.pack("<HHH", h0, h1, h2)
    return h + struct.pack("<H", csum(h))

def f_connect():
    return hdr(0, 0, 0xffff) + b"\0\0\0\0"

def f_start(addr, size):
    p = struct.pack(">II", addr, size)
    return hdr(8, 1, csum(p)) + p

def f_midst_header(total):
    return hdr(total & 0xffff, ((total >> 16) & 0xff) << 8 | 2, 0)

def f_end():
    return hdr(4, 3, 0xffff) + b"\0\0\0\0"

def f_exec(addr):
    p = struct.pack(">I", addr)
    return hdr(4, 4, csum(p)) + p

def chn_len(n):
    for l in bsl.status().splitlines():
        if l.startswith("chn %2d:" % n):
            return int(l.split(":")[1].split()[0])

def wait_ack(what, timeout=5):
    chn, data = bsl.recv()
    ok = isinstance(data, bytes) and len(data) >= 2 and ((data[0] << 8 | data[1]) & ~0x80) == 0
    print(what, "->", chn, data.hex() if isinstance(data, bytes) else data, "ACK" if ok else "NO ACK")
    return ok
