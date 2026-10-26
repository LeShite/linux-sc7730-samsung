#!/usr/bin/env python3
"""Touchscreen gamepad for Batocera on the Galaxy Tab A 7.0 (SM-T280).

Reads the Melfas MIP4 touchscreen and presents a virtual "Microsoft X-Box 360
pad" through uinput, so Batocera configures it like a real one. The layout is
for landscape (display.rotate in batocera.conf), handheld style:

    L1                    HOTKEY                    R1
    L2                                              R2
                                                     Y
      D-PAD                                       X     B
                                                     A
                     SELECT     START

Several fingers at once are fine (D-pad and a button, for example). The
touchscreen is grabbed, so nothing else sees the raw touches.
"""

import signal
import sys

# Outlive the boot scripts that start us, and say so in the log right away
signal.signal(signal.SIGHUP, signal.SIG_IGN)
print("touchpad: python arrancado", flush=True)

import collections
import math
import os
import select
import shutil
import subprocess
import threading
import time

import evdev
from evdev import AbsInfo, UInput, ecodes as e

try:
    from menu import Menu, toast  # the Home button's quick menu and the banners (need PIL and /dev/dri)
except Exception as exc:          # the gamepad must work even without it
    Menu = toast = None
    print(f"touchpad: sin menu rapido: {exc!r}", flush=True)

W, H = 1280, 800          # landscape screen the zones are laid out on

# button zones: name -> (centre x, centre y, radius)
BUTTONS = {
    e.BTN_SOUTH:  (1110, 650, 75),     # A
    e.BTN_EAST:   (1210, 545, 75),     # B
    e.BTN_NORTH:  (1010, 545, 75),     # X (an Xbox pad's X reports BTN_X = BTN_NORTH)
    e.BTN_WEST:   (1110, 440, 75),     # Y (and its Y reports BTN_Y = BTN_WEST)
    e.BTN_SELECT: (540, 725, 85),      # a bit up from the bezel and roomy: it is hard to hit
    e.BTN_START:  (740, 725, 85),
    e.BTN_MODE:   (640, 55, 70),       # hotkey
}
# shoulder zones: (x0, y0, x1, y1)
SHOULDERS = {
    e.BTN_TL: (0, 0, 260, 140),
    e.BTN_TR: (W - 260, 0, W, 140),
}
TRIGGERS = {                          # analog triggers, pressed = full
    e.ABS_Z: (0, 150, 260, 290),
    e.ABS_RZ: (W - 260, 150, W, 290),
}
DPAD = (190, 545, 175, 35)            # centre x, centre y, radius, dead zone


PAD_NAMES = {e.BTN_SOUTH: "A", e.BTN_EAST: "B", e.BTN_NORTH: "X", e.BTN_WEST: "Y", e.BTN_TL: "L1",
             e.BTN_TR: "R1", e.BTN_SELECT: "SELECT", e.BTN_START: "START", e.BTN_MODE: "HOTKEY",
             e.ABS_Z: "L2", e.ABS_RZ: "R2"}

EXIT_COMBO = {e.BTN_MODE, e.BTN_START}   # hold Hotkey + Start...
EXIT_HOLD = 1.5                           # ...this many seconds to close the emulator

USERDATA_LOG = "/userdata/system/logs/touchpad.log"
UDC_STATE = "/sys/class/udc/20200000.usb/state"   # "configured" only once a PC has enumerated the tablet
START = time.monotonic()
SLEEP_FLAG = "/var/run/tablet-sleep"     # /boot/powerkey.sh: the tablet sleeps, touches do nothing


class State:
    """Shared by the touch loop and the watcher thread."""
    emulator = False          # a game is running
    touch_budget = 30         # raw touch lines still allowed in the log (refilled when a game starts/ends)
    edge_budget = 600         # button press/release lines still allowed
    combo_since = None        # when Hotkey+Start went down
    sleeping = False          # SLEEP_FLAG exists (checked by the watcher thread)
    menu_open = False         # the Home-button menu is on screen: touches belong to it
    screen_lock = threading.Lock()      # whoever draws over the game (menu, banner) holds it
    usb_up = None             # the PC has the USB gadget configured
    usb_down_since = 0.0
    fired = False             # emulator already asked to close for this press


def emulator_running():
    """Batocera's own test (batocera-es-swissknife --emupid): emulatorlauncher is alive."""
    for name in os.listdir("/proc"):
        if name.isdigit():
            try:
                with open(f"/proc/{name}/cmdline", "rb") as f:
                    if b"emulatorlauncher" in f.read(200):
                        return True
            except OSError:
                pass
    return False


GAME_LOG_DIR = "/boot/lastgame"

# CPU use per process, sampled every 2 s into memory (a few microseconds each) and written with the
# other game logs when a game starts or ends: how much CPU does the audio path really take?
CPU_WATCH = ("pipewire", "wireplumber", "retroarch", "emulationstation", "python3", "emulatorlaunch")
cpu_log = collections.deque(maxlen=300)
cpu_prev = {}


def cpu_sample():
    """One line: total CPU % (all cores = 100) and the % of one core of each watched process."""
    with open("/proc/stat") as f:
        fields = [int(v) for v in f.readline().split()[1:8]]
    total, idle = sum(fields), fields[3] + fields[4]
    ticks = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/stat") as f:
                data = f.read()
            comm = data[data.index("(") + 1:data.rindex(")")]
            rest = data[data.rindex(")") + 2:].split()
            for w in CPU_WATCH:
                if comm.startswith(w[:15]):
                    ticks[w] = ticks.get(w, 0) + int(rest[11]) + int(rest[12])
        except (OSError, ValueError):
            pass
    if cpu_prev:
        dt = total - cpu_prev["total"]
        if dt > 0:
            busy = 100.0 * (1 - (idle - cpu_prev["idle"]) / dt)
            ncpu = 4
            per = " ".join(f"{w}={100.0 * ncpu * (ticks.get(w, 0) - cpu_prev.get(w, 0)) / dt:.0f}"
                           for w in CPU_WATCH)
            cpu_log.append(f"{time.strftime('%H:%M:%S')} up={float(open('/proc/uptime').read().split()[0]):.0f} "
                           f"cpu={busy:.0f}% [{per}]")
    cpu_prev.clear()
    cpu_prev.update(ticks, total=total, idle=idle)
GAME_LOG_FILES = ("/userdata/system/logs/retroarch.log", "/userdata/system/logs/es_launch_stdout.log",
                  "/userdata/system/logs/es_launch_stderr.log")


def save_game_logs():
    """Keep the running game's logs on /boot, flushed to disk: when a game freezes the
    tablet is powered off by force and whatever still sits in RAM is lost."""
    try:
        os.makedirs(GAME_LOG_DIR, exist_ok=True)
        out = subprocess.run(["dmesg"], capture_output=True, timeout=5).stdout[-30000:]
        blobs = {"dmesg.txt": out, "cpu.log": "\n".join(cpu_log).encode()}
        for src in GAME_LOG_FILES:
            if os.path.exists(src):
                with open(src, "rb") as f:
                    blobs[os.path.basename(src)] = f.read()
        for name, data in blobs.items():
            with open(os.path.join(GAME_LOG_DIR, name), "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
    except (OSError, subprocess.SubprocessError):
        pass


def usb_toast():
    """Tell the user, on screen, that a PC just connected (a banner over the game or ES)."""
    if toast is None or State.menu_open or State.sleeping or not State.screen_lock.acquire(False):
        return
    try:
        print("touchpad: aviso en pantalla: conectada a una PC", flush=True)
        toast(["Conectada a una PC", "SSH:  root@10.42.7.2"])
    except Exception as exc:
        print(f"touchpad: error del aviso: {exc!r}", flush=True)
    finally:
        State.screen_lock.release()


def usb_poll():
    """Once a second: a PC appearing on the USB cable (not a wall charger: it never enumerates)."""
    now = time.monotonic()
    try:
        with open(UDC_STATE) as f:
            up = f.read().strip() == "configured"
    except OSError:
        return
    if State.usb_up is None:
        State.usb_up, State.usb_down_since = up, now
    elif up and not State.usb_up:
        State.usb_up = True
        # not at boot, and not for a blink of the cable
        if now - State.usb_down_since >= 3 and now - START >= 45:
            threading.Thread(target=usb_toast, daemon=True).start()
    elif not up and State.usb_up:
        State.usb_up, State.usb_down_since = False, now


def watcher():
    """While a game runs the virtual keyboard must be silent (RetroArch reads it too:
    Esc quits, Space is fast-forward, Enter is Start, and the console echoes the keys).
    Holding Hotkey+Start closes the game the way Batocera does, even if RetroArch
    itself does not see both buttons at once."""
    running, last_scan = False, 0
    while True:
        time.sleep(0.25)
        State.sleeping = os.path.exists(SLEEP_FLAG)
        # /proc is scanned once a second: the periodic debug copying and scanning stalled the audio
        if time.monotonic() - last_scan >= 1.0:
            last_scan = time.monotonic()
            running = emulator_running()
            usb_poll()
            if int(last_scan) % 4 == 0:
                try:
                    cpu_sample()
                except (OSError, ValueError):
                    pass
        if running != State.emulator:
            State.emulator = running
            State.touch_budget = 40
            print("touchpad: emulador en marcha, teclado virtual apagado" if running
                  else "touchpad: emulador cerrado, teclado virtual encendido", flush=True)
            save_game_logs()
        since = State.combo_since
        if running and since is not None and not State.fired and time.monotonic() - since >= EXIT_HOLD:
            State.fired = True
            print("touchpad: Hotkey+Start sostenidos, cierro el emulador", flush=True)
            subprocess.Popen(["batocera-es-swissknife", "--emukill"], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)


def mirror_log():
    """Started from /boot before /userdata is mounted: our stdout goes to
    /tmp/touchpad.log, copy it where TWRP can read it once /userdata shows up."""
    while True:
        time.sleep(5)
        try:
            if os.path.isdir(os.path.dirname(USERDATA_LOG)):
                shutil.copyfile("/tmp/touchpad.log", USERDATA_LOG)
        except OSError:
            pass


def wait_for_userdata(timeout=90):
    """batocera.conf (display.rotate) lives in /userdata, mounted later."""
    for _ in range(timeout):
        if os.path.exists("/userdata/system/batocera.conf"):
            return True
        time.sleep(1)
    return False


def find_touchscreen():
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        if "MELFAS" in dev.name or "mip4" in dev.name.lower():
            return dev
    return None


def display_rotation():
    if not wait_for_userdata():
        print("touchpad: /userdata no aparecio, uso display.rotate=1", flush=True)
        return 1
    try:
        out = subprocess.run(["batocera-settings-get-master", "display.rotate"],
                             capture_output=True, text=True, timeout=5)
        return int(out.stdout.strip() or 1)
    except (OSError, ValueError, subprocess.SubprocessError):
        return 1


def make_mapper(dev, rotation):
    """Return f(raw_x, raw_y) -> (x, y) on the landscape W x H screen."""
    ax = dev.absinfo(e.ABS_MT_POSITION_X)
    ay = dev.absinfo(e.ABS_MT_POSITION_Y)
    pw, ph = ax.max - ax.min + 1, ay.max - ay.min + 1

    def norm(x, y):
        # The touch panel is mounted 180 degrees from the display (found on the tablet: the
        # zones came out upside down), so count from the opposite corner: the position on
        # the panel as the display shows it.
        return 1 - (x - ax.min) / pw, 1 - (y - ay.min) / ph

    if rotation == 3:
        # panel top on the right of the landscape picture
        return lambda x, y: ((1 - norm(x, y)[1]) * W, norm(x, y)[0] * H)
    if rotation == 2:
        return lambda x, y: ((1 - norm(x, y)[0]) * W, (1 - norm(x, y)[1]) * H)
    if rotation == 0:
        return lambda x, y: (norm(x, y)[0] * W, norm(x, y)[1] * H)
    # rotation 1: display.rotate=1, the way ES and the games are shown
    return lambda x, y: (norm(x, y)[1] * W, (1 - norm(x, y)[0]) * H)


def make_pad():
    stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
    trigger = AbsInfo(0, 0, 255, 0, 0, 0)
    hat = AbsInfo(0, -1, 1, 0, 0, 0)
    caps = {
        e.EV_KEY: [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST,
                   e.BTN_TL, e.BTN_TR, e.BTN_SELECT, e.BTN_START,
                   e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_Z, trigger),
                   (e.ABS_RX, stick), (e.ABS_RY, stick), (e.ABS_RZ, trigger),
                   (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
    }
    # the ids and name of a wired Xbox 360 pad, which Batocera knows
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045e,
                  product=0x028e, version=0x0114, bustype=e.BUS_USB)


# EmulationStation always accepts its default keyboard (Batocera's es_input.cfg)
# but ignores a joystick it has no config for, so mirror the pad as keys too:
# arrows move, Enter confirms ("b"), Esc goes back ("a"), Space is start,
# Backspace select, "=" x and "-" y.
KEYS_FOR_BUTTON = {
    e.BTN_SOUTH: e.KEY_ENTER, e.BTN_EAST: e.KEY_ESC, e.BTN_WEST: e.KEY_EQUAL,
    e.BTN_NORTH: e.KEY_MINUS, e.BTN_START: e.KEY_SPACE,
    e.BTN_SELECT: e.KEY_BACKSPACE,
}


def make_keyboard():
    # udev only tags ID_INPUT_KEYBOARD (and SDL only listens to it) when the
    # device has a full set of keys, so declare every code below the buttons
    return UInput({e.EV_KEY: list(range(1, 256))}, name="touchpad keyboard")


def keyboard_keys(keys, axes):
    out = {KEYS_FOR_BUTTON[k] for k in keys if k in KEYS_FOR_BUTTON}
    hx, hy = axes[e.ABS_HAT0X], axes[e.ABS_HAT0Y]
    if hx < 0:
        out.add(e.KEY_LEFT)
    if hx > 0:
        out.add(e.KEY_RIGHT)
    if hy < 0:
        out.add(e.KEY_UP)
    if hy > 0:
        out.add(e.KEY_DOWN)
    return out


def zone_state(points):
    """Pressed keys, trigger values and hat direction for touch points."""
    keys, axes = set(), {e.ABS_Z: 0, e.ABS_RZ: 0}
    hx = hy = 0
    cx, cy, radius, dead = DPAD
    for x, y in points:
        dx, dy = x - cx, y - cy
        dist = math.hypot(dx, dy)
        if dead < dist <= radius * 1.3:
            ang = math.degrees(math.atan2(dy, dx)) % 360
            # 8 sectors of 45 degrees, diagonals included
            if ang < 67.5 or ang >= 292.5:
                hx = 1
            if 112.5 <= ang < 247.5:
                hx = -1
            if 22.5 <= ang < 157.5:
                hy = 1
            if 202.5 <= ang < 337.5:
                hy = -1
            continue
        for code, (bx, by, br) in BUTTONS.items():
            if math.hypot(x - bx, y - by) <= br:
                keys.add(code)
        for code, (x0, y0, x1, y1) in SHOULDERS.items():
            if x0 <= x < x1 and y0 <= y < y1:
                keys.add(code)
        for code, (x0, y0, x1, y1) in TRIGGERS.items():
            if x0 <= x < x1 and y0 <= y < y1:
                axes[code] = 255
    axes[e.ABS_HAT0X], axes[e.ABS_HAT0Y] = hx, hy
    return keys, axes


def find_home_key():
    """The physical buttons (kernel sc8830-keys): Home opens the quick menu."""
    for path in evdev.list_devices():
        d = evdev.InputDevice(path)
        if d.name == "sc8830-keys":
            return d
    return None


def main():
    threading.Thread(target=mirror_log, daemon=True).start()
    threading.Thread(target=watcher, daemon=True).start()
    print(f"touchpad: dispositivos {evdev.list_devices()}", flush=True)
    dev = None
    for _ in range(30):          # the touchscreen may show up late
        dev = find_touchscreen()
        if dev:
            break
        time.sleep(1)
    if not dev:
        sys.exit("touchpad: no touchscreen found")

    rotation = display_rotation()
    to_screen = make_mapper(dev, rotation)
    print(f"touchpad: {dev.path} '{dev.name}' x={dev.absinfo(e.ABS_MT_POSITION_X)} "
          f"y={dev.absinfo(e.ABS_MT_POSITION_Y)} rotate={rotation}", flush=True)
    pad = make_pad()
    kbd = make_keyboard()
    dev.grab()
    print(f"touchpad: pad {pad.device.path}, keyboard {kbd.device.path}", flush=True)
    kbd_now = set()

    slots, slot = {}, 0           # slot -> [raw x, raw y]
    pressed, axes_now = set(), {}

    home = find_home_key()
    menu = Menu(lambda: State.emulator) if Menu and home else None
    print(f"touchpad: boton Home {'-> menu rapido' if menu else 'no disponible'}", flush=True)

    def close_menu():
        State.menu_open = False
        try:
            menu.close()
        except Exception as exc:
            print(f"touchpad: error al cerrar el menu: {exc!r}", flush=True)
        finally:
            State.screen_lock.release()
        print("touchpad: menu cerrado", flush=True)

    def menu_call(fn, *args):
        """Run a menu method; True while the menu should stay open. Any error closes it."""
        try:
            return fn(*args)
        except Exception as exc:
            print(f"touchpad: error del menu: {exc!r}", flush=True)
            return False

    def toggle_menu():
        nonlocal pressed, axes_now, kbd_now
        if State.menu_open:
            close_menu()
            return
        if State.sleeping or menu is None:
            return
        # let go of every pad button and key first, the menu owns the touches now
        for code in pressed:
            pad.write(e.EV_KEY, code, 0)
        for code, val in axes_now.items():
            if val:
                pad.write(e.EV_ABS, code, 0)
        pad.syn()
        pressed, axes_now = set(), {}
        State.combo_since, State.fired = None, False
        State.screen_lock.acquire()           # waits for a banner that is on screen
        print("touchpad: menu abierto (Home)", flush=True)
        if menu_call(menu.open) is False:
            State.screen_lock.release()
            return
        State.menu_open = True

    def stream():
        """(device, event) of the touchscreen and the buttons; (None, None) once a second while the menu is open."""
        devs = [dev] + ([home] if home else [])
        while True:
            ready, _, _ = select.select(devs, [], [], 1.0 if State.menu_open else None)
            if not ready:
                yield None, None
                continue
            for d in ready:
                for event in d.read():
                    yield d, event

    for src, ev in stream():
        if ev is None:
            if State.menu_open and not menu_call(menu.tick):
                close_menu()
            continue
        if src is home:
            if ev.type == e.EV_KEY and ev.code == e.KEY_HOMEPAGE and ev.value == 1:
                toggle_menu()
            continue
        if ev.type == e.EV_ABS:
            if ev.code == e.ABS_MT_SLOT:
                slot = ev.value
            elif ev.code == e.ABS_MT_TRACKING_ID:
                if ev.value < 0:
                    slots.pop(slot, None)
                else:
                    slots[slot] = [0, 0]
            elif ev.code == e.ABS_MT_POSITION_X:
                slots.setdefault(slot, [0, 0])[0] = ev.value
            elif ev.code == e.ABS_MT_POSITION_Y:
                slots.setdefault(slot, [0, 0])[1] = ev.value
        elif ev.type == e.EV_SYN and ev.code == e.SYN_REPORT:
            if State.menu_open:
                if not menu_call(menu.touch, [to_screen(x, y) for x, y in slots.values()]):
                    close_menu()
                continue
            keys, axes = zone_state([] if State.sleeping else
                                    [to_screen(x, y) for x, y in slots.values()])
            for code in keys - pressed:
                pad.write(e.EV_KEY, code, 1)
            for code in pressed - keys:
                pad.write(e.EV_KEY, code, 0)
            for code, val in axes.items():
                if axes_now.get(code) != val:
                    pad.write(e.EV_ABS, code, val)
            changes = [("+" if c in keys else "-") + PAD_NAMES[c] for c in keys ^ pressed]
            changes += [("+" if axes[c] else "-") + PAD_NAMES[c] for c in axes
                        if c in PAD_NAMES and bool(axes[c]) != bool(axes_now.get(c))]
            if changes and State.edge_budget > 0:
                State.edge_budget -= 1
                pts = [tuple(round(v) for v in to_screen(x, y)) for x, y in slots.values()]
                print(f"pad {'[juego]' if State.emulator else '[menu] '} {' '.join(sorted(changes))} "
                      f"dedos={pts}", flush=True)
            pressed, axes_now = keys, axes
            pad.syn()
            if EXIT_COMBO <= keys:
                if State.combo_since is None:
                    State.combo_since = time.monotonic()
            else:
                State.combo_since, State.fired = None, False
            # /boot/kbd_off: the pad alone drives EmulationStation (with both, each tap moved it twice)
            kbd_keys = set() if State.emulator or os.path.exists("/boot/kbd_off") else keyboard_keys(keys, axes)
            if slots and State.touch_budget > 0:
                State.touch_budget -= 1
                pts = [tuple(round(v) for v in to_screen(x, y)) for x, y in slots.values()]
                print(f"touch raw={list(slots.values())} screen={pts} keys={sorted(kbd_keys)}",
                      flush=True)
            for code in kbd_keys - kbd_now:
                kbd.write(e.EV_KEY, code, 1)
            for code in kbd_now - kbd_keys:
                kbd.write(e.EV_KEY, code, 0)
            kbd_now = kbd_keys
            kbd.syn()


if __name__ == "__main__":
    main()
