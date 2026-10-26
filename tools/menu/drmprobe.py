#!/usr/bin/env python3
"""Read-only probe of the display: borrow the DRM fd of the process that owns the screen
(pidfd_getfd) and print the CRTC, connector and the framebuffer on scan-out.
  drmprobe.py [pid]      default: whoever has /dev/dri/card0 open (emulationstation or retroarch)
"""
import ctypes
import ctypes.util
import fcntl
import os
import struct
import sys

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
SYS_PIDFD_OPEN, SYS_PIDFD_GETFD = 434, 438          # ARM EABI


def ioctl_nr(nr, size):
    return (3 << 30) | (size << 16) | (ord("d") << 8) | nr


def find_owner():
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            for fd in os.listdir(f"/proc/{p}/fd"):
                if os.readlink(f"/proc/{p}/fd/{fd}") == "/dev/dri/card0":
                    return int(p), int(fd)
        except OSError:
            pass
    sys.exit("nobody has /dev/dri/card0 open")


def borrow(pid, fd):
    pidfd = libc.syscall(SYS_PIDFD_OPEN, pid, 0)
    if pidfd < 0:
        raise OSError(ctypes.get_errno(), "pidfd_open")
    new = libc.syscall(SYS_PIDFD_GETFD, pidfd, fd, 0)
    if new < 0:
        raise OSError(ctypes.get_errno(), "pidfd_getfd")
    os.close(pidfd)
    return new


def drm(fd, nr, buf):
    buf = ctypes.create_string_buffer(bytes(buf), len(buf))
    fcntl.ioctl(fd, ioctl_nr(nr, len(buf)), buf)
    return bytes(buf)


def main():
    pid, fd = find_owner() if len(sys.argv) < 2 else (int(sys.argv[1]), None)
    print(f"owner pid {pid} ({open(f'/proc/{pid}/comm').read().strip()})")
    for f in sorted(os.listdir(f"/proc/{pid}/fd"), key=int):
        if os.readlink(f"/proc/{pid}/fd/{f}") == "/dev/dri/card0":
            mine = borrow(pid, int(f))
            res = drm(mine, 0xA0, bytes(64))
            nfb, ncrtc, ncon, nenc = struct.unpack_from("4I", res, 32)
            print(f"fd {f}: resources: {nfb} fbs, {ncrtc} crtcs, {ncon} connectors, {nenc} encoders")
            arrays = {k: (ctypes.c_uint32 * max(n, 1))() for k, n in
                      (("fb", nfb), ("crtc", ncrtc), ("con", ncon), ("enc", nenc))}
            req = struct.pack("4Q4I4I", *(ctypes.addressof(arrays[k]) for k in ("fb", "crtc", "con", "enc")),
                              nfb, ncrtc, ncon, nenc, 0, 0, 0, 0)
            drm(mine, 0xA0, req)
            crtcs = list(arrays["crtc"])[:ncrtc]
            cons = list(arrays["con"])[:ncon]
            print("  crtc ids", crtcs, "connector ids", cons, "fb ids", list(arrays["fb"])[:nfb])
            for c in crtcs:
                r = drm(mine, 0xA1, struct.pack("QIIIIIII68s", 0, 0, c, 0, 0, 0, 0, 0, bytes(68)))
                _, ncn, cid, fbid, x, y, gamma, valid = struct.unpack_from("QIIIIIII", r, 0)
                mode = r[36:36 + 68]
                w, h = struct.unpack_from("H", mode, 4)[0], struct.unpack_from("H", mode, 14)[0]
                print(f"  crtc {cid}: fb {fbid} at {x},{y}, mode_valid {valid}, mode {w}x{h}")
                if fbid:
                    try:
                        g = drm(mine, 0xCE, struct.pack("I", fbid) + bytes(100))
                        fid, fw, fh, fmt, flags = struct.unpack_from("5I", g, 0)
                        handles, pitches = struct.unpack_from("4I", g, 20), struct.unpack_from("4I", g, 36)
                        print(f"    fb {fbid}: {fw}x{fh} format {struct.pack('<I', fmt)!r} handles {handles} pitches {pitches}")
                    except OSError as e:
                        print("    GETFB2 failed:", e)
            os.close(mine)


if __name__ == "__main__":
    main()
