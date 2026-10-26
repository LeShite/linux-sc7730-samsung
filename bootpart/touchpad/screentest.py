#!/usr/bin/env python3
"""Test of screen.py: pause whoever has the display, show a test picture 5 s (gradient bars and
text, drawn in landscape and turned to the panel), put everything back.  screentest.py [seconds]"""
import sys
import time

from PIL import Image, ImageDraw, ImageFont

from screen import Screen

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
W, H = 1280, 800
land = Image.new("RGB", (W, H), (20, 24, 40))
d = ImageDraw.Draw(land)
font = ImageFont.truetype("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", 48)
d.rectangle((60, 60, W - 60, H - 60), outline=(255, 255, 255), width=6)
d.text((100, 100), "MENU DE PRUEBA", fill=(255, 255, 255), font=font)
for i, c in enumerate(((230, 60, 60), (60, 200, 90), (70, 120, 240), (240, 200, 60))):
    d.rectangle((100 + i * 270, 300, 340 + i * 270, 500), fill=c)
t0 = time.time()
with Screen() as scr:
    print("screen", scr.w, "x", scr.h, "original fb", scr.orig_fb, flush=True)
    while time.time() - t0 < secs:
        img = land.copy()
        ImageDraw.Draw(img).text((100, 600), "%.1f s" % (time.time() - t0), fill=(255, 255, 0), font=font)
        scr.show(img.transpose(Image.ROTATE_270))
        time.sleep(0.2)
print("restored", flush=True)
