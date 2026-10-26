#!/usr/bin/env python3
"""GPU clock 384 <-> 512 MHz by hand (kernel CONFIG_DEVMEM), the way the vendor's gpu_dfs_func() does it:
pause the GPU users, close the gate of the clock now in use, switch the mux with the clock stopped,
open the new gate, resume. (The first try opened the 512 MHz gate first and switched the mux with both
clocks running: the bus hung.)   gpuclk.py 512 | 384
Every step is written to /boot/gpuclk.log and synced BEFORE it runs, so a hang shows where it was.

Second try: it hung at step 2, closing the clock of a GPU the lima driver still had an interrupt on
(any register read of a GPU with no clock hangs the CPU). So ES is stopped and lima unbound for the
switch, and both come back afterwards."""
import mmap
import os
import signal
import struct
import subprocess
import sys
import time

sys.path.insert(0, "/boot/touchpad")
from screen import card_holders           # noqa: E402

PMU_GATES = 0x402B011C          # bit 5: 384 MHz source, bit 7: 512 MHz source (bit 4: 460.8)
GPU_CLK = 0x60100004            # bits 2:0 source (3 = 512 MHz, 5 = 384 MHz), bits 5:4 divider - 1
SOURCES = {384: (0x20, 5), 512: (0x80, 3)}
LOG = "/boot/gpuclk.log"
LIMA = "/sys/bus/platform/drivers/lima"
GPU_DEV = "60000000.gpu"
ES_SERVICE = "/etc/init.d/S31emulationstation"


def sysfs_write(path, value):
    with open(path, "w") as f:
        f.write(value)


def log(msg):
    with open(LOG, "a") as f:
        f.write(f"{time.strftime('%T')} {msg}\n")
        f.flush()
        os.fsync(f.fileno())
    print(msg, flush=True)


class Regs:
    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.maps = {}

    def _page(self, addr):
        base = addr & ~0xFFF
        if base not in self.maps:
            self.maps[base] = mmap.mmap(self.fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
        return self.maps[base], addr - base

    def read(self, addr):
        mm, off = self._page(addr)
        return struct.unpack_from("<I", mm, off)[0]

    def write(self, addr, value):
        """A blind write: no read-back (reading a GPU register with a dead clock hangs the CPU)."""
        mm, off = self._page(addr)
        struct.pack_into("<I", mm, off, value & 0xFFFFFFFF)

    def update(self, addr, mask, value):
        mm, off = self._page(addr)
        old = struct.unpack_from("<I", mm, off)[0]
        struct.pack_into("<I", mm, off, (old & ~mask & 0xFFFFFFFF) | (value & mask))
        return old, struct.unpack_from("<I", mm, off)[0]


def main():
    target = int(sys.argv[1])
    if target not in SOURCES:
        sys.exit("512 or 384")
    new_gate, new_sel = SOURCES[target]
    r = Regs()
    clk = r.read(GPU_CLK)
    gates = r.read(PMU_GATES)
    cur_sel = clk & 7
    cur_gate = {5: 0x20, 3: 0x80, 6: 0x10}.get(cur_sel)
    log(f"== to {target} MHz: CLK_CTRL {clk:#x}, gates {gates:#x}")
    if cur_sel == new_sel:
        log("already there")
        return
    log("1 stopping EmulationStation (nobody uses the GPU)")
    subprocess.run([ES_SERVICE, "stop"], timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if not [p for p, _ in card_holders()]:
            break
        time.sleep(0.25)
    unbound = False
    try:
        log("1b unbinding lima: no interrupt handler left on the GPU")
        sysfs_write(f"{LIMA}/unbind", GPU_DEV)
        unbound = True
        time.sleep(0.2)
        # both clocks run during the switch (the GPU is idle: ES stopped, lima unbound); the vendor closes
        # the old one first, but that read-back hang points at the new clock being dead, so find out
        old_clk = clk                                      # read while the 384 MHz clock ran
        log(f"2 opening gate {new_gate:#x}")
        r.update(PMU_GATES, new_gate, new_gate)
        log(f"2b gates now {r.read(PMU_GATES):#x}")
        time.sleep(0.05)                                   # the PLL has had time to settle
        log(f"3 blind write CLK_CTRL {(old_clk & ~7) | new_sel:#x} (no read-back)")
        r.write(GPU_CLK, (old_clk & ~7) | new_sel)
        log("3b written")
        time.sleep(0.1)
        log("4 reading CLK_CTRL back (hangs here if the new clock is dead)")
        log(f"4b read {r.read(GPU_CLK):#x}")
        if cur_gate:
            log(f"5 closing the old gate {cur_gate:#x}")
            r.update(PMU_GATES, cur_gate, 0)
        log(f"6 done: CLK_CTRL {r.read(GPU_CLK):#x}, gates {r.read(PMU_GATES):#x}")
    finally:
        if unbound:
            log("7 binding lima again")
            sysfs_write(f"{LIMA}/bind", GPU_DEV)
        log("8 starting EmulationStation")
        subprocess.Popen([ES_SERVICE, "start"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)


main()
