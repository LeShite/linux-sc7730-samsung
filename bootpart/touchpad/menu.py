"""Quick menu (Home button): battery, system and sliders for brightness and volume, plus buttons.

Drawn in landscape (1280x800, the coordinates touchpad.py's touches already use) and turned to the
panel by screen.py, over whatever owns the screen, which is paused while the menu is open.
"""

import os
import subprocess
import time

from PIL import Image, ImageDraw, ImageFont

from screen import Screen

W, H = 1280, 800
FONT = "/usr/share/fonts/dejavu/DejaVuSans.ttf"
FONT_B = "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
BATT = "/sys/class/power_supply/battery"
CHG = "/sys/class/power_supply/charger"
BL = "/sys/class/backlight/panel"
CPUCLK = "/sys/devices/platform/soc/40030000.cpuclk/freq_mhz"   # kernel sc8830-cpuclk (sets voltage and bus itself)
CPU_SAVED = "/boot/cpu_mhz"                                      # chosen speed, applied at boot by boot-custom.sh
CPU_STEPS = [26 * n for n in range(50, 58)]                      # 1300 ... 1482 MHz
IDLE_CLOSE_S = 60
CONFIRM_S = 4

BG, CARD, CARD2 = (22, 24, 30), (42, 45, 54), (58, 62, 74)
TEXT, SUB = (240, 242, 248), (150, 156, 172)
ACC, GOOD, WARN, BAD = (0, 190, 225), (80, 210, 120), (240, 170, 40), (235, 70, 70)


def _read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def _num(path, default=0):
    try:
        return int(_read(path))
    except ValueError:
        return default


def _run(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def _amixer_get(name):
    for line in _run("amixer", "-c", "0", "cget", f"name={name}").splitlines():
        if ": values=" in line:
            return line.split("values=")[1].strip()
    return None


def toast(lines, secs=2.5):
    """A banner at the top of the screen over what is showing (it pauses for that moment)."""
    with Screen() as scr:
        bg = scr.grab()
        land = bg.transpose(Image.ROTATE_90) if bg else Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(land)
        big, small = ImageFont.truetype(FONT_B, 40), ImageFont.truetype(FONT, 28)
        x0, x1, y0, y1 = 190, W - 190, 36, 36 + 70 + 44 * (len(lines) - 1) + 40
        d.rounded_rectangle((x0 + 6, y0 + 8, x1 + 6, y1 + 8), 26, fill=(0, 0, 0))
        d.rounded_rectangle((x0, y0, x1, y1), 26, fill=(30, 34, 44), outline=ACC, width=4)
        for i, line in enumerate(lines):
            d.text(((x0 + x1) // 2, y0 + 50 + 46 * i), line, fill=TEXT if i == 0 else SUB,
                   font=big if i == 0 else small, anchor="mm")
        scr.show(land.transpose(Image.ROTATE_270))
        time.sleep(secs)


class Menu:
    def __init__(self, game_running):
        self.game_running = game_running          # callable -> bool
        self.screen = Screen()
        self.fonts = {}
        self.last_cpu = None
        self.cpu_pct = 0
        self.rects = {}
        self.press = None                         # name of what the finger went down on
        self.confirm = None                       # (button, deadline)
        self.last_touch = time.monotonic()
        self.dirty = True
        self.brightness_start = 0
        self.cpu_mhz = 0                          # slider position (MHz) while dragging
        self.after = None                         # what to run once the menu is closed

    # ---- lifecycle ---------------------------------------------------------
    def open(self):
        self.screen.open()
        self.bl_max = max(1, _num(f"{BL}/max_brightness", 255))
        self.bright = max(5, round(100 * _num(f"{BL}/brightness") / self.bl_max))
        self.brightness_start = self.bright
        self.jack = _amixer_get("Aux Jack Flag") == "on"
        if self.jack:
            self.vol_ctl, self.vol_max = "Aux Gain", 15
        else:
            self.vol_ctl, self.vol_max = "Speaker Playback Volume", 8
        try:
            self.vol = int(_amixer_get(self.vol_ctl) or 0)
        except ValueError:
            self.vol = 0
        self.last_touch = time.monotonic()
        self.confirm = None
        self.press = None
        self.after = None
        self.last_cpu = None
        self.stats()
        self.cpu_mhz = self.s["freq"] or 0
        self.render()

    def close(self):
        try:
            self.screen.close()
        finally:
            if self.bright != self.brightness_start:
                subprocess.Popen(["batocera-settings-set", "display.brightness", str(self.bright)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if self.after:
                subprocess.Popen(self.after, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)

    # ---- data ---------------------------------------------------------------
    def stats(self):
        s = {}
        s["cap"] = _num(f"{BATT}/capacity")
        s["mv"] = _num(f"{BATT}/voltage_now") // 1000
        s["ma"] = _num(f"{BATT}/current_now") // 1000
        s["status"] = _read(f"{BATT}/status", "?")
        s["plugged"] = _read(f"{CHG}/online") == "1"
        s["in_ma"] = _num(f"{CHG}/input_current_limit") // 1000
        s["chg_ma"] = _num(f"{CHG}/constant_charge_current") // 1000
        s["full_mah"] = _num(f"{BATT}/charge_full_design", 3950000) // 1000
        # CPU use between two samples
        try:
            f = [int(v) for v in _read("/proc/stat").splitlines()[0].split()[1:8]]
            total, idle = sum(f), f[3] + f[4]
            if self.last_cpu and total > self.last_cpu[0]:
                self.cpu_pct = round(100 * (1 - (idle - self.last_cpu[1]) / (total - self.last_cpu[0])))
            self.last_cpu = (total, idle)
        except (ValueError, IndexError):
            pass
        s["cpu"] = self.cpu_pct
        mem = {}
        for line in _read("/proc/meminfo").splitlines():
            k, _, v = line.partition(":")
            mem[k] = int(v.split()[0]) if v.split() else 0
        s["mem_used"] = (mem.get("MemTotal", 0) - mem.get("MemAvailable", 0)) // 1024
        s["mem_total"] = mem.get("MemTotal", 0) // 1024
        # temperature and frequency appear once their drivers exist
        temps = []
        for i in range(8):
            t = _read(f"/sys/class/thermal/thermal_zone{i}/temp")
            if t.lstrip("-").isdigit():
                temps.append(int(t) / 1000)
        s["temp"] = max(temps) if temps else None
        freq = _read(CPUCLK)
        s["freq"] = int(freq) if freq.isdigit() and int(freq) else None
        try:
            s["up"] = int(float(_read("/proc/uptime").split()[0]))
        except (ValueError, IndexError):
            s["up"] = 0
        self.s = s

    def remaining(self):
        s = self.s
        if s["status"] != "Discharging" or s["ma"] > -120:
            return None
        h = s["cap"] / 100 * s["full_mah"] / -s["ma"]
        return f"~ {int(h)} h {int(h % 1 * 60):02d} min" if h < 48 else None

    # ---- drawing ---------------------------------------------------------------
    def font(self, size, bold=False):
        key = (size, bold)
        if key not in self.fonts:
            self.fonts[key] = ImageFont.truetype(FONT_B if bold else FONT, size)
        return self.fonts[key]

    def render(self):
        s = self.s
        img = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(img)
        txt = lambda xy, t, size=28, col=TEXT, bold=False, anchor="la": d.text(xy, t, fill=col, font=self.font(size, bold), anchor=anchor)

        # top bar
        txt((40, 22), "Menú rápido", 40, TEXT, True)
        txt((W - 40, 30), "Home: cerrar", 26, SUB, False, "ra")

        # battery card
        d.rounded_rectangle((40, 100, 620, 430), 22, fill=CARD)
        txt((70, 112), "Batería", 28, SUB)
        cap = s["cap"]
        col = BAD if cap <= 15 else WARN if cap <= 30 else GOOD
        txt((70, 150), f"{cap} %", 96, col, True)
        if s["plugged"]:
            state = "Cargando" if s["status"] in ("Charging", "Full") else "Conectada"
            cable = f"{state} · {s['in_ma']} mA de entrada"
        else:
            state = "Descargando"
            cable = state
        txt((70, 262), cable, 28, TEXT)
        d.rounded_rectangle((70, 308, 590, 336), 12, fill=CARD2)
        d.rounded_rectangle((70, 308, 70 + max(24, 520 * cap // 100), 336), 12, fill=col)
        txt((70, 352), f"{s['mv']} mV    {s['ma']:+d} mA", 28, TEXT)
        rem = self.remaining()
        txt((70, 392), f"Autonomía {rem}" if rem else "Autonomía —", 26, SUB)

        # system card
        self.rects = {}
        d.rounded_rectangle((40, 450, 620, 770), 22, fill=CARD)
        txt((70, 462), "Sistema", 28, SUB)
        txt((70, 506), f"CPU  {s['cpu']} %", 30, TEXT)
        d.rounded_rectangle((300, 516, 590, 540), 10, fill=CARD2)
        d.rounded_rectangle((300, 516, 300 + max(14, 290 * s["cpu"] // 100), 540), 10, fill=ACC)
        txt((70, 558), f"Memoria  {s['mem_used']} / {s['mem_total']} MB", 30, TEXT)
        txt((70, 610), "Temperatura  " + (f"{s['temp']:.0f} °C" if s["temp"] is not None else "—  (sin sensor)"), 30,
            TEXT if s["temp"] is not None else SUB)
        up = s["up"]
        txt((590, 466), f"encendida {up // 3600} h {up % 3600 // 60:02d} min", 24, SUB, False, "ra")
        if s["freq"]:
            shown = self.cpu_mhz if self.press == "cpu" else s["freq"]
            txt((70, 662), f"Velocidad CPU  {shown} MHz", 30, TEXT)
            frac = (min(max(shown, CPU_STEPS[0]), CPU_STEPS[-1]) - CPU_STEPS[0]) / (CPU_STEPS[-1] - CPU_STEPS[0])
            x0, x1, ty = 100, 560, 730
            d.rounded_rectangle((x0, ty - 6, x1, ty + 6), 6, fill=CARD2)
            d.rounded_rectangle((x0, ty - 6, x0 + int((x1 - x0) * frac), ty + 6), 6, fill=ACC)
            kx = x0 + int((x1 - x0) * frac)
            d.ellipse((kx - 20, ty - 20, kx + 20, ty + 20), fill=TEXT)
            self.rects["cpu"] = (x0 - 30, ty - 30, x1 + 30, ty + 30)
        else:
            txt((70, 662), "Frecuencia  —  (fija)", 30, SUB)

        # sliders card
        d.rounded_rectangle((660, 100, 1240, 380), 22, fill=CARD)
        for i, (name, label, frac, shown) in enumerate((
                ("bright", "Brillo", (self.bright - 5) / 95, f"{self.bright} %"),
                ("vol", "Volumen", self.vol / self.vol_max, f"{round(100 * self.vol / self.vol_max)} %"))):
            y = 120 + i * 120
            txt((690, y), label + (" (audífonos)" if name == "vol" and self.jack else ""), 28, SUB)
            txt((1210, y), shown, 28, TEXT, True, "ra")
            x0, x1, ty = 700, 1200, y + 62
            d.rounded_rectangle((x0, ty - 7, x1, ty + 7), 7, fill=CARD2)
            d.rounded_rectangle((x0, ty - 7, x0 + int((x1 - x0) * frac), ty + 7), 7, fill=ACC)
            kx = x0 + int((x1 - x0) * frac)
            d.ellipse((kx - 22, ty - 22, kx + 22, ty + 22), fill=TEXT)
            self.rects[name] = (x0 - 30, ty - 40, x1 + 30, ty + 40)

        # buttons
        game = self.game_running()
        buttons = (("sleep", "Reposo", (660, 400, 940, 520), True),
                   ("kill", "Cerrar juego", (960, 400, 1240, 520), game),
                   ("reboot", "Reiniciar", (660, 540, 940, 660), True),
                   ("off", "Apagar", (960, 540, 1240, 660), True),
                   ("back", "Volver", (660, 680, 1240, 770), True))
        now = time.monotonic()
        for name, label, rect, enabled in buttons:
            asking = self.confirm and self.confirm[0] == name and now < self.confirm[1]
            fill = BAD if asking else (ACC if name == "back" else CARD) if enabled else (30, 32, 38)
            d.rounded_rectangle(rect, 22, fill=fill)
            if self.press == name and enabled:
                d.rounded_rectangle(rect, 22, outline=TEXT, width=5)
            cx, cy = (rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2
            txt((cx, cy), "¿Seguro? Toca otra vez" if asking else label, 28 if asking else 34,
                TEXT if enabled else (90, 94, 106), True, "mm")
            if enabled:
                self.rects[name] = rect

        self.screen.show(img.transpose(Image.ROTATE_270))
        self.dirty = False

    # ---- input ----------------------------------------------------------------
    def hit(self, p):
        for name, (x0, y0, x1, y1) in self.rects.items():
            if x0 <= p[0] < x1 and y0 <= p[1] < y1:
                return name
        return None

    def set_slider(self, name, x):
        if name == "cpu":
            frac = min(1.0, max(0.0, (x - 100) / (560 - 100)))
            self.cpu_mhz = CPU_STEPS[round(frac * (len(CPU_STEPS) - 1))]
            self.dirty = True
            return
        x0, x1 = 700, 1200
        frac = min(1.0, max(0.0, (x - x0) / (x1 - x0)))
        if name == "bright":
            self.bright = 5 + round(95 * frac)
            try:
                with open(f"{BL}/brightness", "w") as f:
                    f.write(str(round(self.bright * self.bl_max / 100)))
            except OSError:
                pass
        else:
            v = round(frac * self.vol_max)
            if v != self.vol:
                self.vol = v
                _run("amixer", "-c", "0", "-q", "cset", f"name={self.vol_ctl}", str(v))
        self.dirty = True

    def touch(self, points):
        """points: the fingers' positions on the 1280x800 landscape screen (empty = all lifted).
        Returns True while the menu stays open."""
        self.screen.keepalive()
        now = time.monotonic()
        if points:
            self.last_touch = now
            p = points[0]
            if self.press is None:
                self.press = self.hit(p) or ""
                if self.press in ("bright", "vol", "cpu"):
                    self.set_slider(self.press, p[0])
                self.dirty = True
            elif self.press in ("bright", "vol", "cpu"):
                self.set_slider(self.press, p[0])
        elif self.press is not None:
            name, self.press = self.press, None
            self.dirty = True
            if name == "cpu":
                self.apply_cpu()
            if name and name not in ("bright", "vol", "cpu") and self.hit(self.last_point) == name:
                return self.activate(name)
        if points:
            self.last_point = points[0]
        if self.dirty:
            self.render()
        return True

    last_point = (0, 0)

    def apply_cpu(self):
        """Finger lifted from the speed slider: set it now and keep it for the next boot."""
        try:
            with open(CPUCLK, "w") as f:
                f.write(str(self.cpu_mhz))
            with open(CPU_SAVED, "w") as f:
                f.write(f"{self.cpu_mhz}\n")
        except OSError:
            pass
        self.stats()

    def activate(self, name):
        if name == "back":
            return False
        if name in ("kill", "reboot", "off"):
            if not (self.confirm and self.confirm[0] == name and time.monotonic() < self.confirm[1]):
                self.confirm = (name, time.monotonic() + CONFIRM_S)
                self.render()
                return True
        self.after = {"sleep": ["bash", "/boot/powerkey.sh", "sleep"],
                      "kill": ["batocera-es-swissknife", "--emukill"],
                      "reboot": ["/sbin/reboot"],
                      "off": ["bash", "/boot/powerkey.sh", "off"]}[name]
        return False

    def tick(self):
        """About once a second while open. Returns True while the menu stays open."""
        now = time.monotonic()
        if now - self.last_touch > IDLE_CLOSE_S:
            return False
        if self.confirm and now > self.confirm[1]:
            self.confirm = None
        self.stats()
        self.render()
        return True
