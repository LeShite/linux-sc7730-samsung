#!/usr/bin/env python3
"""Pull the ramoops area of our kernel out of RAM through TWRP and decode it.

After a hang or crash, force a reboot straight into TWRP (the DRAM keeps its
contents across the warm reset) and run this. TWRP's kernel has /dev/mem
without STRICT_DEVMEM and never touches the vendor ION carveout we use.

Usage: read-ramoops.py [dump.bin]   (reads a saved dump instead of adb)
"""

import struct
import subprocess
import sys

BASE = 0x9F600000      # must match ramoops@9f600000 in the board DTS
SIZE = 0x100000
SIG = b"DBGC"          # persistent_ram_buffer signature (0x43474244 LE)


MEMDUMP = "/home/nigga/batocera-t280/build/initramfs/memdump"


def dump():
    # busybox dd in TWRP overflows its 32-bit off_t above 2 GiB, so use our
    # own memdump (initramfs/memdump.c)
    subprocess.run(["adb", "push", MEMDUMP, "/tmp/memdump"], capture_output=True)
    cmd = f"chmod 755 /tmp/memdump && /tmp/memdump {BASE:#x} {SIZE:#x}"
    data = subprocess.run(["adb", "exec-out", cmd], capture_output=True).stdout
    if len(data) != SIZE:
        sys.exit(f"got {len(data)} bytes over adb, expected {SIZE} (USB dropped?)")
    return data


def zones(data):
    """Yield (offset, text) of every ramoops ring buffer found."""
    off = data.find(SIG)
    while off != -1:
        start, size = struct.unpack("<II", data[off + 4:off + 12])
        end = data.find(SIG, off + 4)
        cap = (end if end != -1 else len(data)) - off - 12
        ring = data[off + 12:off + 12 + cap]
        if 0 < size <= cap and start <= size:
            # oldest data begins at start when the ring has wrapped
            text = ring[start:size] + ring[:start] if size == cap else ring[:size]
            yield off, text.decode("utf-8", "replace")
        off = end


def main():
    data = open(sys.argv[1], "rb").read() if len(sys.argv) > 1 else dump()
    if len(sys.argv) == 1:
        with open("ramoops-dump.bin", "wb") as f:
            f.write(data)
    found = False
    for off, text in zones(data):
        if text.strip("\0").strip():
            found = True
            print(f"===== zone at +{off:#x} ({len(text)} bytes) =====")
            print(text)
    if not found:
        print("no ramoops data found (kernel died before pstore started, or RAM was cleared)")


if __name__ == "__main__":
    main()
