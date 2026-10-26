#!/usr/bin/env python3
"""/dev/mem by hand (the kernel has CONFIG_DEVMEM, not strict):
  mem.py ADDR              read a 32-bit register
  mem.py ADDR VALUE        write it
  mem.py ADDR MASK VALUE   read-modify-write: (old & ~MASK) | (VALUE & MASK)
Addresses and values accept 0x. Prints what it read and, after a write, what it reads back."""
import mmap
import os
import struct
import sys

PAGE = 4096


def access(addr, value=None, mask=0xFFFFFFFF):
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    base = addr & ~(PAGE - 1)
    mm = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
    off = addr - base
    old = struct.unpack_from("<I", mm, off)[0]
    new = None
    if value is not None:
        new = (old & ~mask & 0xFFFFFFFF) | (value & mask)
        struct.pack_into("<I", mm, off, new)
        new = struct.unpack_from("<I", mm, off)[0]
    mm.close()
    os.close(fd)
    return old, new


if __name__ == "__main__":
    a = [int(x, 0) for x in sys.argv[1:]]
    if len(a) == 1:
        print("0x%08x = 0x%08x" % (a[0], access(a[0])[0]))
    elif len(a) == 2:
        old, new = access(a[0], a[1])
        print("0x%08x: 0x%08x -> 0x%08x" % (a[0], old, new))
    else:
        old, new = access(a[0], a[2], a[1])
        print("0x%08x: 0x%08x -> 0x%08x" % (a[0], old, new))
