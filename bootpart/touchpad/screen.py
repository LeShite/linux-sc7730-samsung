"""Draw a picture over whatever owns the screen (EmulationStation or a game), and give it back.

The display is simpledrm's single CRTC (800x1280, XRGB8888). Whoever runs on it holds DRM master, and
page flips need it, so this borrows that process's DRM file descriptor with pidfd_getfd (root only):
the borrowed fd *is* the master's. The owner is stopped while we draw (SIGSTOP: a game pauses, and
nothing flips behind our back), and on close the framebuffer it had on screen goes back and it is
continued. A detached watchdog continues it after WATCHDOG_S seconds whatever happens to us.

simpledrm copies the buffer to the panel at every commit, not continuously: two buffers, and each
new picture is written to the one that is not shown, then flipped to.
"""

import ctypes
import ctypes.util
import fcntl
import mmap
import os
import signal
import struct
import subprocess
import time

from PIL import Image

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
SYS_PIDFD_OPEN, SYS_PIDFD_GETFD = 434, 438           # ARM EABI
WATCHDOG_S = 90
FOURCC_XR24 = 0x34325258


def _nr(nr, size):
    return (3 << 30) | (size << 16) | (ord("d") << 8) | nr


def _drm(fd, nr, buf):
    b = ctypes.create_string_buffer(bytes(buf), len(buf))
    fcntl.ioctl(fd, _nr(nr, len(buf)), b)
    return bytes(b)


def card_holders():
    """[(pid, fd)] of every process with /dev/dri/card0 open."""
    out = []
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            for fd in os.listdir(f"/proc/{p}/fd"):
                if os.readlink(f"/proc/{p}/fd/{fd}") == "/dev/dri/card0":
                    out.append((int(p), int(fd)))
        except OSError:
            pass
    return out


def _borrow(pid, fd):
    pidfd = libc.syscall(SYS_PIDFD_OPEN, pid, 0)
    if pidfd < 0:
        raise OSError(ctypes.get_errno(), "pidfd_open")
    try:
        new = libc.syscall(SYS_PIDFD_GETFD, pidfd, fd, 0)
        if new < 0:
            raise OSError(ctypes.get_errno(), "pidfd_getfd")
        return new
    finally:
        os.close(pidfd)


class Screen:
    def __init__(self):
        self.fd = None
        self.stopped = []
        self.bufs = []            # [(handle, fb_id, mmap)]
        self.shown = -1
        self.crtc = self.conn = self.orig_fb = None
        self.mode = b""
        self.w = self.h = 0
        self.watchdog = None
        self.armed = 0.0

    # ---- ioctls -----------------------------------------------------------
    def _resources(self):
        res = _drm(self.fd, 0xA0, bytes(64))
        nfb, ncrtc, ncon, nenc = struct.unpack_from("4I", res, 32)
        arr = {k: (ctypes.c_uint32 * max(n, 1))() for k, n in (("fb", nfb), ("crtc", ncrtc), ("con", ncon), ("enc", nenc))}
        _drm(self.fd, 0xA0, struct.pack("4Q4I16x", *(ctypes.addressof(arr[k]) for k in ("fb", "crtc", "con", "enc")),
                                        nfb, ncrtc, ncon, nenc))
        return arr["crtc"][0], arr["con"][0]

    def _getcrtc(self):
        r = _drm(self.fd, 0xA1, struct.pack("QIIIIIII68s", 0, 0, self.crtc, 0, 0, 0, 0, 0, bytes(68)))
        return struct.unpack_from("I", r, 16)[0], r[36:104]

    def _flip(self, fb):
        for _ in range(200):
            try:
                _drm(self.fd, 0xB0, struct.pack("IIIIQ", self.crtc, fb, 0, 0, 0))     # PAGE_FLIP, no event
                return True
            except OSError as e:
                if e.errno != 16:       # EBUSY: a flip is still in flight
                    raise
                time.sleep(0.005)
        return False

    def _make_buffer(self):
        h = _drm(self.fd, 0xB2, struct.pack("6IQ", self.h, self.w, 32, 0, 0, 0, 0))
        handle, pitch = struct.unpack_from("II", h, 16)
        size = struct.unpack_from("Q", h, 24)[0]
        off = struct.unpack_from("Q", _drm(self.fd, 0xB3, struct.pack("IIQ", handle, 0, 0)), 8)[0]
        # struct drm_mode_fb_cmd2: 17 u32 (fb_id out, size, format, flags, 4 handles, 4 pitches, 4 offsets),
        # 4 bytes of padding, 4 u64 modifiers: 104 bytes
        cmd = struct.pack("<5I4I4I4I", 0, self.w, self.h, FOURCC_XR24, 0, handle, 0, 0, 0, pitch, 0, 0, 0, 0, 0, 0, 0) + bytes(36)
        fb = struct.unpack_from("<I", _drm(self.fd, 0xB8, cmd), 0)[0]
        mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
        return handle, fb, mm

    # ---- lifecycle ----------------------------------------------------------
    def _arm(self):
        if self.watchdog is not None:
            self.watchdog.kill()
            self.watchdog.wait()
        self.watchdog = subprocess.Popen(
            ["sh", "-c", f"sleep {WATCHDOG_S}; kill -CONT {' '.join(map(str, self._pids))} 2>/dev/null"],
            start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        self.armed = time.monotonic()

    def keepalive(self):
        """Still in use: push the watchdog's deadline back (at most every 10 s)."""
        if self.watchdog is not None and time.monotonic() - self.armed > 10:
            self._arm()

    def open(self):
        holders = card_holders()
        if not holders:
            raise RuntimeError("nobody has the display open")
        pids = sorted({p for p, _ in holders})
        self._pids = pids
        self._arm()         # the failsafe first: if we die the owners are continued by somebody else
        for pid in pids:
            try:
                os.kill(pid, signal.SIGSTOP)
                self.stopped.append(pid)
            except OSError:
                pass
        try:
            for pid, fd in holders:               # the master is the one whose flips are allowed
                try:
                    self.fd = _borrow(pid, fd)
                    self.crtc, self.conn = self._resources()
                    self.orig_fb, self.mode = self._getcrtc()
                    self.w, self.h = struct.unpack_from("HH", self.mode, 4)[0], struct.unpack_from("H", self.mode, 14)[0]
                    if not self.orig_fb:
                        raise OSError(0, "nothing on screen")
                    self.bufs = [self._make_buffer(), self._make_buffer()]
                    if not self._flip(self.bufs[0][1]):
                        raise OSError(16, "flip busy")
                    self.shown = 0
                    return
                except OSError:
                    self._release_buffers()
                    if self.fd is not None:
                        os.close(self.fd)
                        self.fd = None
            raise RuntimeError("no DRM master among the holders")
        except BaseException:
            self.close()
            raise

    def _release_buffers(self):
        for handle, fb, mm in self.bufs:
            try:
                mm.close()
                _drm(self.fd, 0xAF, struct.pack("I", fb))           # RMFB
                _drm(self.fd, 0xB4, struct.pack("II", handle, 0))    # DESTROY_DUMB
            except OSError:
                pass
        self.bufs = []

    def grab(self):
        """What the owner had on screen (panel orientation, RGB), or None if it cannot be read."""
        handle = None
        try:
            g = _drm(self.fd, 0xCE, struct.pack("I", self.orig_fb) + bytes(100))        # GETFB2: a new handle
            handle, pitch = struct.unpack_from("I", g, 20)[0], struct.unpack_from("I", g, 36)[0]
            off = struct.unpack_from("Q", _drm(self.fd, 0xB3, struct.pack("IIQ", handle, 0, 0)), 8)[0]
            mm = mmap.mmap(self.fd, pitch * self.h, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
            try:
                data = bytes(mm)
            finally:
                mm.close()
            return Image.frombuffer("RGBX", (self.w, self.h), data, "raw", "BGRX", pitch, 1).convert("RGB")
        except (OSError, ValueError):
            return None
        finally:
            if handle:
                try:
                    _drm(self.fd, 0x09, struct.pack("II", handle, 0))                    # GEM_CLOSE
                except OSError:
                    pass

    def show(self, img):
        """img: a PIL image already in the panel's orientation (w x h)."""
        if img.size != (self.w, self.h):
            raise ValueError(f"picture is {img.size}, the panel {(self.w, self.h)}")
        nxt = 1 - self.shown
        self.bufs[nxt][2][:] = img.convert("RGB").tobytes("raw", "BGRX")
        if self._flip(self.bufs[nxt][1]):
            self.shown = nxt

    def close(self):
        if self.fd is not None:
            try:
                if self.orig_fb:
                    self._flip(self.orig_fb)
            except OSError:
                pass
            self._release_buffers()
            os.close(self.fd)
            self.fd = None
        for pid in self.stopped:
            try:
                os.kill(pid, signal.SIGCONT)
            except OSError:
                pass
        self.stopped = []
        if self.watchdog is not None:       # the owners run again: no failsafe needed
            self.watchdog.kill()
            self.watchdog.wait()
            self.watchdog = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()
