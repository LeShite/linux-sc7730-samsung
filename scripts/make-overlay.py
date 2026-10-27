#!/usr/bin/env python3
"""Draw the RetroArch overlay that shows where the touchscreen gamepad zones are.

The zones come straight from bootpart/touchpad/touchpad.py (BUTTONS, SHOULDERS,
TRIGGERS, DPAD), so the picture cannot drift from what actually answers touches.

RetroArch draws overlays in the display's own orientation (it does not apply
video_rotation to them), and the panel is 800x1280 portrait, so the landscape
drawing is turned 90 degrees clockwise to come out the right way up with
display.rotate=1. That matches the game picture (checked on the tablet). The touch
panel, in turn, is mounted 180 degrees from the display; touchpad.py accounts for it.

Writes ~/batocera-t280/overlay/ (gamepad.png, gamepad-landscape.png, gamepad.cfg,
retroarch/<system>.cfg) and overlay.tar, ready for scripts/push-overlay.sh.
  make-overlay.py [opacity]      default 0.8
"""

import importlib.util
import io
import sys
import tarfile
import types
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path.home() / "batocera-t280"
OUT = ROOT / "overlay"
AUDIO_FILTERS = OUT / "audiofilter"      # iir.so, panning.so: Batocera's ARM builds from /usr/share/audio_filters
SS = 2                                    # supersampling
W, H = 1280, 800                          # landscape picture the zones are laid out on
FONT = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"

# Directories of /roms in Batocera: RetroArch reads <system>.cfg for the one launched
SYSTEMS = """abuse amiga1200 amiga500 amigacd32 amigacdtv amstradcpc apple2 apple2gs arduboy atari2600
atari5200 atari7800 atari800 atarist atomiswave c128 c20 c64 cannonball cavestory cdogs cgenius channelf
colecovision cplus4 daphne devilutionx dos dreamcast easyrpg fbneo fds flash gameandwatch gamegear gb
gb2players gba gbc gbc2players gx4000 hcl hurrican intellivision lowresnx lynx macintosh mame mastersystem
megadrive megaduck moonlight mrboom msu-md msx1 msx2 msx2+ msxturbor multivision n64 n64dd naomi neogeo
neogeocd nes ngp ngpc o2em odcommander openbor pc88 pc98 pcengine pcenginecd pet pico pico8 pokemini ports
prboom psp psx quake3 samcoupe satellaview scummvm scv sdlpop sega32x segacd sg1000 sgb snes snes-msu1
solarus sonicretro sufami superbroswar supergrafx supervision thextech thomson tic80 tyrian tyrquake
vectrex videopacplus virtualboy wswan wswanc x1 x68000 xash3d_fwgs xrick zc210 zx81 zxspectrum""".split()


class _Codes:
    """Stand-in for evdev.ecodes: every code is its own name."""
    def __getattr__(self, name):
        return name


def load_zones():
    sys.dont_write_bytecode = True       # no __pycache__ inside bootpart/, it gets pushed
    stub = types.ModuleType("evdev")
    stub.AbsInfo = stub.UInput = object
    stub.ecodes = _Codes()
    sys.modules["evdev"] = stub
    spec = importlib.util.spec_from_file_location("touchpad", ROOT / "bootpart/touchpad/touchpad.py")
    tp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tp)
    return tp


def font(size):
    return ImageFont.truetype(FONT, size * SS)


# what each touch zone is called and coloured: the Xbox letters, or the N64's (RetroArch's parallel_n64 with
# its "independent C buttons" option: south = A, west-code BTN_NORTH (left) = B, east = C-down, top = C-up,
# L1 = C-left, R1 = C-right, L2 = Z, R2 = R, SELECT = L)
LABELS_XBOX = {"BTN_SOUTH": ((76, 175, 80), "A"), "BTN_EAST": ((244, 67, 54), "B"),
               "BTN_NORTH": ((33, 150, 243), "X"), "BTN_WEST": ((255, 193, 7), "Y"),
               "BTN_SELECT": "SELECT", "BTN_START": "START", "BTN_MODE": "HOTKEY",
               "BTN_TL": "L1", "BTN_TR": "R1", "ABS_Z": "L2", "ABS_RZ": "R2"}
LABELS_N64 = {"BTN_SOUTH": ((33, 120, 243), "A"), "BTN_NORTH": ((76, 175, 80), "B"),
              "BTN_EAST": ((255, 193, 7), "C\u2193"), "BTN_WEST": ((255, 193, 7), "C\u2191"),
              "BTN_SELECT": "L", "BTN_START": "START", "BTN_MODE": "HOTKEY",
              "BTN_TL": "C\u2190", "BTN_TR": "C\u2192", "ABS_Z": "Z", "ABS_RZ": "R"}


def draw_landscape(tp, names=None):
    names = names or LABELS_XBOX
    s = lambda v: int(round(v * SS))
    canvas = Image.new("RGBA", (s(W), s(H)), (0, 0, 0, 0))

    def layer(draw_fn):
        nonlocal canvas
        lay = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        draw_fn(ImageDraw.Draw(lay))
        canvas = Image.alpha_composite(canvas, lay)

    def label(d, x, y, text, size, fill=(255, 255, 255, 255)):
        d.text((s(x), s(y)), text, font=font(size), fill=fill, anchor="mm",
               stroke_width=SS * 2, stroke_fill=(0, 0, 0, 200))

    def circle(d, cx, cy, r, color, text, size):
        d.ellipse((s(cx - r), s(cy - r), s(cx + r), s(cy + r)),
                  fill=color + (110,), outline=(255, 255, 255, 230), width=s(4))
        label(d, cx, cy, text, size)

    def pill(d, box, text, size, color=(210, 210, 210)):
        x0, y0, x1, y1 = box
        d.rounded_rectangle((s(x0), s(y0), s(x1), s(y1)), radius=s((y1 - y0) / 2),
                            fill=color + (90,), outline=(255, 255, 255, 230), width=s(4))
        label(d, (x0 + x1) / 2, (y0 + y1) / 2, text, size)

    # D-pad: the touch ring, the cross, and arrows pointing out
    cx, cy, r, dead = tp.DPAD
    def dpad(d):
        d.ellipse((s(cx - r), s(cy - r), s(cx + r), s(cy + r)),
                  fill=(255, 255, 255, 25), outline=(255, 255, 255, 150), width=s(3))
        arm, thick = r * 0.80, 62
        d.rounded_rectangle((s(cx - arm), s(cy - thick / 2), s(cx + arm), s(cy + thick / 2)),
                            radius=s(16), fill=(210, 210, 210, 100), outline=(255, 255, 255, 230), width=s(4))
        d.rounded_rectangle((s(cx - thick / 2), s(cy - arm), s(cx + thick / 2), s(cy + arm)),
                            radius=s(16), fill=(210, 210, 210, 100), outline=(255, 255, 255, 230), width=s(4))
        d.ellipse((s(cx - dead), s(cy - dead), s(cx + dead), s(cy + dead)),
                  fill=(0, 0, 0, 90), outline=(255, 255, 255, 200), width=s(3))
        t, a = r * 0.80 - 22, 20             # arrow tip distance, half width
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            tip = (cx + dx * t, cy + dy * t)
            base = (cx + dx * (t - 34), cy + dy * (t - 34))
            px, py = -dy * a, dx * a
            d.polygon([(s(tip[0]), s(tip[1])), (s(base[0] + px), s(base[1] + py)),
                       (s(base[0] - px), s(base[1] - py))], fill=(255, 255, 255, 235))
    layer(dpad)

    # Xbox letters: X is left (BTN_NORTH code), Y is top (BTN_WEST code)
    colors = {k: names[k] for k in ("BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST")}
    for code, (bx, by, br) in tp.BUTTONS.items():
        if code in colors:
            col, text = colors[code]
            # drawn a bit smaller than the touch zone so neighbours do not overlap
            layer(lambda d, bx=bx, by=by, br=br, col=col, text=text: circle(d, bx, by, br - 8, col, text, 64))
        else:
            text = names[{"BTN_SELECT": "BTN_SELECT", "BTN_START": "BTN_START", "BTN_MODE": "BTN_MODE"}[code]]
            layer(lambda d, bx=bx, by=by, text=text: pill(d, (bx - 62, by - 26, bx + 62, by + 26), text, 24))

    layer(lambda d: label(d, W / 2, 668, f"Mantén HOTKEY + START {tp.EXIT_HOLD:g} s para salir", 22,
                          fill=(255, 255, 255, 235)))

    for code, (x0, y0, x1, y1) in {**tp.SHOULDERS, **tp.TRIGGERS}.items():
        text = names[code]
        layer(lambda d, x0=x0, y0=y0, x1=x1, y1=y1, text=text:
              pill(d, (x0 + 14, y0 + 14, x1 - 14, y1 - 14), text, 40))

    return canvas.resize((W, H), Image.LANCZOS)


def to_panel(land):
    """Landscape picture -> the 800x1280 panel, turned 90 degrees clockwise."""
    return Image.fromarray(np.rot90(np.asarray(land), -1).copy())


def check_mapping(tp, panel):
    """Find the B button (red) in the panel picture, work out where a finger on it reports in
    raw touch coordinates (the touch panel is mounted 180 degrees from the display, so raw =
    (799 - x, 1279 - y)) and run that through touchpad.py's own mapping: it must land on B."""
    a = np.asarray(panel).astype(int)
    red = (a[..., 0] > 200) & (a[..., 1] < 120) & (a[..., 3] > 60)
    ys, xs = np.nonzero(red)
    px, py = xs.mean(), ys.mean()
    rx, ry = 799 - px, 1279 - py

    class Info:                       # what the MIP4 reports: x 0-800, y 0-1280
        def __init__(self, mx): self.min, self.max = 0, mx

    class Dev:
        def absinfo(self, axis): return Info(800 if axis == "ABS_MT_POSITION_X" else 1280)

    x, y = tp.make_mapper(Dev(), 1)(rx, ry)
    bx, by, br = tp.BUTTONS["BTN_EAST"]
    ok = ((x - bx) ** 2 + (y - by) ** 2) ** 0.5 < br * 0.5
    print(f"comprobación: el botón B dibujado en el panel ({px:.0f},{py:.0f}) = toque crudo ({rx:.0f},{ry:.0f}) "
          f"-> ({x:.0f},{y:.0f}) en el plano horizontal; zona B = ({bx},{by}) -> {'OK' if ok else 'MAL'}")
    return ok


def main():
    opacity = sys.argv[1] if len(sys.argv) > 1 else "0.8"
    tp = load_zones()
    land = draw_landscape(tp)
    panel = to_panel(land)
    if not check_mapping(tp, panel):
        sys.exit("el overlay no coincide con las zonas del mando")

    OUT.mkdir(exist_ok=True)
    (OUT / "retroarch").mkdir(exist_ok=True)
    panel.save(OUT / "gamepad.png")
    land.save(OUT / "gamepad-landscape.png")
    land64 = draw_landscape(tp, LABELS_N64)
    to_panel(land64).save(OUT / "gamepad-n64.png")
    (OUT / "gamepad-n64.cfg").write_text(
        'overlays = 1\n'
        'overlay0_overlay = "gamepad-n64.png"\n'
        'overlay0_full_screen = true\n'
        'overlay0_descs = 0\n')
    (OUT / "gamepad.cfg").write_text(
        'overlays = 1\n'
        'overlay0_overlay = "gamepad.png"\n'
        'overlay0_full_screen = true\n'
        'overlay0_descs = 0\n')
    cfg = ('input_overlay_enable = "true"\n'
           'input_overlay = "/userdata/system/touchpad/overlay/gamepad.cfg"\n'
           f'input_overlay_opacity = "{opacity}"\n'
           'input_overlay_scale = "1.0"\n'
           'input_overlay_auto_scale = "false"\n'
           # the virtual pad counts as a connected gamepad: never hide for it
           'input_overlay_hide_when_gamepad_connected = "false"\n'
           'input_overlay_hide_in_menu = "true"\n'
           'audio_dsp_plugin = "/userdata/system/audiofilter/speaker.dsp"\n'
           # debugging: RetroArch's own log, to read from TWRP
           'log_verbosity = "true"\n'
           'log_to_file = "true"\n'
           'log_to_file_timestamp = "false"\n'
           'log_dir = "/userdata/system/logs"\n')
    for name in SYSTEMS:
        extra = ""
        if name == "n64":	# no analog stick on the touch pad: the D-pad drives the N64 stick (core option alt-map: C buttons on the face buttons)
            extra = '# no analog stick on the touch pad: the D-pad drives the N64 stick\ninput_player1_analog_dpad_mode = "1"\n'
        text = cfg + extra
        if name == "n64":
            text = text.replace("overlay/gamepad.cfg", "overlay/gamepad-n64.cfg")
        (OUT / "retroarch" / f"{name}.cfg").write_text(text)

    # PipeWire's graph runs at 48 kHz by default while the apps mostly use 44.1: pin it to CD rate.
    # Drop-in for both places a user config may live ($HOME/.config and $XDG_CONFIG_HOME)
    (OUT / "pipewire").mkdir(exist_ok=True)
    (OUT / "pipewire" / "10-rate.conf").write_text(
        "context.properties = {\n    default.clock.rate = 44100\n"
        "    default.clock.allowed-rates = [ 44100 ]\n}\n")

    # (The ALSA downmix lives in bootpart/asound.conf: S12populateshare deletes any ~/.asoundrc at boot.)

    # Body for the small speaker, in RetroArch games only: a low shelf (iir BBOOST) and a gain below 1 so
    # the boost does not clip. The filter plugins (iir.so, panning.so) must sit next to the .dsp file;
    # they are the ARM builds that Batocera ships in /usr/share/audio_filters (copied by push-overlay).
    (OUT / "audiofilter").mkdir(exist_ok=True)
    (OUT / "audiofilter" / "body.dsp").write_text(
        "filters = 2\nfilter0 = iir\nfilter1 = panning\n\n"
        "iir_gain = 6.0\niir_type = BBOOST\niir_frequency = 220.0\n\n"
        '# -3 dB overall: headroom for the boost (the left output is what reaches the speaker)\n'
        'panning_left_mix = "0.7 0.0"\npanning_right_mix = "0.0 0.7"\n')

    # Samsung's own speaker equalizer (Android's /system/etc/audio_para, EQ_Handsfree, preset 1: one band
    # on, a -60 x 0.1 dB cut at 1500 Hz with q = 1024 / 1024 = 1): the little speaker resonates there.
    # Same biquad here as RetroArch's iir filter, type PEQ. Games only: ES's menu does not use the filter.
    (OUT / "audiofilter" / "speaker.dsp").write_text(
        "# Samsung's speaker EQ for the SM-T280 (Android audio_para, EQ_Handsfree preset 1)\n"
        "filters = 1\nfilter0 = iir\n\n"
        "iir_type = PEQ\niir_frequency = 1500.0\niir_quality = 1.0\niir_gain = -6.0\n")

    # The pad in EmulationStation's menus too. ES has no overlay of its own, but a theme's "screen"
    # view draws extra images over everything (the carbon theme's CRT shader does it). The theme
    # carbon-touch is Batocera's carbon (its theme.xml, copied from the tablet's squashfs, plus one
    # include; everything else symlinked) with the landscape drawing on top. push-overlay selects it.
    carbon_xml = (OUT / "es-theme" / "carbon-theme.xml").read_text(encoding="utf-8")
    assert carbon_xml.count("</theme>") == 1
    theme_xml = carbon_xml.replace("</theme>", "  <!-- SM-T280: the touchscreen pad -->\n"
                                   "  <include>./touchpad.xml</include>\n</theme>")
    (OUT / "es-theme" / "theme.xml").write_text(theme_xml, encoding="utf-8")
    (OUT / "es-theme" / "touchpad.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<theme>\n  <formatVersion>7</formatVersion>\n'
        '  <view name="screen">\n    <image name="touchpad" extra="true">\n'
        '      <pos>0 0</pos>\n      <size>1 1</size>\n      <path>./touchpad.png</path>\n'
        f'      <color>FFFFFF{round(float(opacity) * 255):02X}</color>\n'
        '      <zIndex>100</zIndex>\n    </image>\n  </view>\n</theme>\n')
    CARBON = "/usr/share/emulationstation/themes/es-theme-carbon"
    CARBON_ENTRIES = ("_splash.xml", "art", "games", "gamesplash.xml", "lang", "layouts", "readme.txt",
                      "resources", "subsets", "views")

    # The buttons (kernel driver sc8830-keys) through Batocera's triggerhappy: S50triggerhappy prefers
    # this file over /etc/triggerhappy/triggers.d/multimedia_keys.conf. Power: hold 2 s to shut down
    # (the stock batocera-shutdown shuts down at once on press when nothing is configured). Volume:
    # the hardware gain of the output in use (scripts in /boot, from bootpart/).
    (OUT / "multimedia_keys.conf").write_text(
        "# SM-T280: power and volume buttons\n"
        "KEY_VOLUMEUP    1    bash /boot/volkey.sh up\n"
        "KEY_VOLUMEDOWN  1    bash /boot/volkey.sh down\n"
        "KEY_POWER       1    bash /boot/powerkey.sh 1\n"
        "KEY_POWER       0    bash /boot/powerkey.sh 0\n"
        "KEY_SYSRQ       1    batocera-screenshot\n")

    # One tarball: far fewer adb round trips over the flaky USB link
    with tarfile.open(ROOT / "overlay.tar", "w") as tar:
        tar.add(OUT / "multimedia_keys.conf", "system/configs/multimedia_keys.conf")
        for f in ("theme.xml", "touchpad.xml"):
            tar.add(OUT / "es-theme" / f, f"themes/carbon-touch/{f}")
        tar.add(OUT / "gamepad-landscape.png", "themes/carbon-touch/touchpad.png")
        for entry in CARBON_ENTRIES:
            link = tarfile.TarInfo(f"themes/carbon-touch/{entry}")
            link.type, link.linkname = tarfile.SYMTYPE, f"{CARBON}/{entry}"
            tar.addfile(link)
        for dsp in ("body.dsp", "speaker.dsp"):
            tar.add(OUT / "audiofilter" / dsp, f"system/audiofilter/{dsp}")
        for so in ("iir.so", "panning.so"):
            tar.add(AUDIO_FILTERS / so, f"system/audiofilter/{so}")
        for base in ("system/configs", "system/.config"):
            tar.add(OUT / "pipewire" / "10-rate.conf", f"{base}/pipewire/pipewire.conf.d/10-rate.conf")
        for f in ("gamepad.png", "gamepad-landscape.png", "gamepad.cfg"):
            tar.add(OUT / f, f"system/touchpad/overlay/{f}")
        for name in SYSTEMS:
            tar.add(OUT / "retroarch" / f"{name}.cfg", f"system/configs/retroarch/{name}.cfg")
    print(f"overlay.tar: {len(SYSTEMS)} sistemas, opacidad {opacity}, "
          f"{(ROOT / 'overlay.tar').stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
