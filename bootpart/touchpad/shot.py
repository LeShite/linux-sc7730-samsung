#!/usr/bin/env python3
"""Screenshot of what is on the panel, without pausing anything: shot.py out.png [landscape]
(borrows the display owner's DRM fd, reads the framebuffer on scan-out; may show a torn frame)"""
import sys

from PIL import Image

import screen

out = sys.argv[1]
s = screen.Screen()
pid, fd = screen.card_holders()[0]
for pid, fd in screen.card_holders():
    try:
        s.fd = screen._borrow(pid, fd)
        s.crtc, s.conn = s._resources()
        s.orig_fb, s.mode = s._getcrtc()
        break
    except OSError:
        continue
import struct
import time
s.w, s.h = struct.unpack_from("HH", s.mode, 4)[0], struct.unpack_from("H", s.mode, 14)[0]
if out == "fps":               # how many different frames reach the panel per second, over 4 s
    seen, last, t0 = 0, None, time.monotonic()
    while time.monotonic() - t0 < 4:
        fb, _ = s._getcrtc()
        if fb != last:
            seen += last is not None
            last = fb
    print("fps (cambios de imagen en pantalla): %.1f" % (seen / 4))
    sys.exit(0)
img = s.grab()
if img is None:
    sys.exit("could not read the framebuffer")
if len(sys.argv) > 2:           # show it the way the user sees it (landscape)
    img = img.transpose(Image.ROTATE_90)
img.save(out)
print("saved", out, img.size)
