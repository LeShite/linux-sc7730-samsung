#!/usr/bin/env python3
"""Pack a boot image that the SM-T280 (gtexswifi) SBOOT accepts.

Layout, reverse engineered from a TWRP image that boots on this tablet:

  DHTB header (512 bytes): b"DHTB", u32 1, sha256(payload), u32 len at 48
  payload:
    Android boot image v0, page size 2048, the "dt_size" in header word 9
    kernel | ramdisk | dt.img, each padded to a page
    b"SEANDROIDENFORCE" + 0xffffffff

dt.img ("SPRD" container): b"SPRD", u32 version 1, u32 count, then per entry
(u32 platform 8830, u32 board rev, u32 0x20000, u32 offset, u32 size), a zero
entry, and the page-aligned DTBs. SBOOT picks the entry matching hw_revision.

Usage:
  mkgteximg.py --kernel zImage-dtb --dtb board.dtb [--ramdisk rd.cpio.gz] -o boot.img
  mkgteximg.py --selftest twrp-recovery.img   # rebuild a known-good image
"""

import argparse
import hashlib
import struct
import sys

PAGE = 2048
BOOT_MAGIC = b"ANDROID!"
TRAILER = b"SEANDROIDENFORCE" + b"\xff\xff\xff\xff"
BOARD_REVS = (0, 2, 3, 4, 5)
SPRD_PLATFORM = 8830
SPRD_SUBTYPE = 0x20000
# Free space left in each DTB so SBOOT can insert bootargs and memory info
DTB_SLACK = 8192


def pad(data, align=PAGE):
    return data + b"\0" * (-len(data) % align)


def grow_fdt(dtb, slack):
    if dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("not a flattened device tree")
    total = struct.unpack(">I", dtb[4:8])[0]
    new_total = total + slack
    return dtb[:4] + struct.pack(">I", new_total) + dtb[8:total] + b"\0" * slack


def make_dtimg(dtbs):
    """dtbs: list of (rev, dtb bytes)."""
    header_len = 12 + 20 * (len(dtbs) + 1)
    offset = len(pad(b"\0" * header_len))
    table, blobs = b"", b""
    for rev, dtb in dtbs:
        blob = pad(dtb)
        table += struct.pack("<5I", SPRD_PLATFORM, rev, SPRD_SUBTYPE, offset, len(blob))
        blobs += blob
        offset += len(blob)
    table += b"\0" * 20
    head = struct.pack("<4sII", b"SPRD", 1, len(dtbs)) + table
    return pad(head) + blobs


def make_bootimg(kernel, ramdisk, dt, cmdline=b"console=ttyS1,115200n8"):
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", dt):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    header = struct.pack(
        "<8s10I16s512s32s1024s",
        BOOT_MAGIC,
        len(kernel), 0x00008000,
        len(ramdisk), 0x01000000,
        0, 0x00f00000,
        0x00000100,
        PAGE,
        len(dt),
        0,
        b"",
        cmdline,
        sha.digest(),
        b"",
    )
    return pad(header) + pad(kernel) + pad(ramdisk) + pad(dt) + TRAILER


def wrap_dhtb(payload):
    head = b"DHTB" + struct.pack("<I", 1) + hashlib.sha256(payload).digest()
    head = head.ljust(48, b"\0") + struct.pack("<I", len(payload))
    return head.ljust(512, b"\0") + payload


def split_bootimg(img):
    """Return (kernel, ramdisk, dt, cmdline) from a DHTB-wrapped image."""
    if img[:4] != b"DHTB":
        sys.exit("missing DHTB header")
    size = struct.unpack("<I", img[48:52])[0]
    p = img[512:512 + size]
    (magic, ks, _, rs, _, ss, _, _, page, ds, _, _, cmd, _, _) = struct.unpack(
        "<8s10I16s512s32s1024s", p[:1632])
    if magic != BOOT_MAGIC or page != PAGE or ss:
        sys.exit("unexpected boot image layout")
    off = PAGE
    parts = []
    for n in (ks, rs, ds):
        parts.append(p[off:off + n])
        off += len(pad(b"\0" * n))
    return parts[0], parts[1], parts[2], cmd.rstrip(b"\0")


def selftest(path):
    orig = open(path, "rb").read()
    size = struct.unpack("<I", orig[48:52])[0]
    kernel, ramdisk, dt, cmd = split_bootimg(orig)
    rebuilt = wrap_dhtb(make_bootimg(kernel, ramdisk, dt, cmd))
    ok = rebuilt == orig[:512 + size]
    print("selftest:", "OK, byte-identical" if ok else "MISMATCH")
    if not ok:
        for i, (a, b) in enumerate(zip(rebuilt, orig)):
            if a != b:
                print("first difference at offset", i)
                break
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kernel")
    ap.add_argument("--dtb")
    ap.add_argument("--ramdisk")
    ap.add_argument("-o", "--output")
    ap.add_argument("--selftest", metavar="IMG")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest(a.selftest) else 1)
    if not (a.kernel and a.dtb and a.output):
        ap.error("--kernel, --dtb and -o are required")

    kernel = open(a.kernel, "rb").read()
    dtb = grow_fdt(open(a.dtb, "rb").read(), DTB_SLACK)
    # An empty newc archive keeps SBOOT happy when no ramdisk is given
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else b"070701" + b"0" * 48 + \
        b"0000000B" + b"0" * 8 + b"TRAILER!!!\0" + b"\0" * 3
    dt = make_dtimg([(rev, dtb) for rev in BOARD_REVS])
    img = wrap_dhtb(make_bootimg(kernel, ramdisk, dt))

    if len(img) > 16 * 1024 * 1024:
        sys.exit(f"image is {len(img)} bytes, KERNEL/RECOVERY hold 16 MiB")
    with open(a.output, "wb") as f:
        f.write(img)
    print(f"{a.output}: {len(img)} bytes (kernel {len(kernel)}, dt.img {len(dt)})")


if __name__ == "__main__":
    main()
