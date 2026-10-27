"""Whole bring-up after boot: firmware over SDIO channel 3, then the calibration buffer on channel 0.
Run once per boot, right after /dev/marlin appears."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bsl, fwload

def snap(tag):
    s = bsl.status().splitlines()
    print(tag, s[0], [l.strip() for l in s if "bytes" in l and not l.endswith(" 0 bytes")], flush=True)

def load_fw(path=HERE + "/sc2331_fw.bin"):
    d = open(path, "rb").read()
    CH = 0x20000
    n = (len(d) + CH - 1) // CH
    d += b"\xff" * (n * CH - len(d))
    snap("inicio")
    bsl.send(3, fwload.f_start(0x100000, n * CH))
    if not fwload.wait_ack("START"):
        return False
    for i in range(n):
        frame = fwload.f_midst_header(CH) + d[i * CH:(i + 1) * CH]
        for off in range(0, len(frame), 65536):
            bsl.send(3, frame[off:off + 65536])
        if not fwload.wait_ack("MIDST %d" % i):
            return False
    bsl.send(3, fwload.f_end())
    if not fwload.wait_ack("END"):
        return False
    bsl.send(3, fwload.f_exec(0x100000))
    return fwload.wait_ack("EXEC")

if __name__ == "__main__":
    if not load_fw():
        sys.exit("descarga fallida")
    delay = int(os.environ.get("DELAY", "5"))
    for i in range(delay * 2):
        time.sleep(0.5)
        s = bsl.status().splitlines()
        act = [l.strip() for l in s if "bytes" in l and not l.endswith(" 0 bytes")]
        if i % 4 == 3 or s[0] != "status 0x0000 irq 0" or len(act) > 1:
            print("t+%.1fs" % ((i + 1) * 0.5), s[0], act, flush=True)
    buf = open(HERE + "/nvbuf.bin", "rb").read()
    print("calibracion: escribo", len(buf), "bytes en el canal 0", flush=True)
    for off in range(0, len(buf), 4096):
        bsl.send(0, buf[off:off + 4096])
    chn, data = bsl.recv()
    if chn is None:
        sys.exit("sin respuesta de calibracion")
    print("calibracion OK: canal", chn, len(data), "bytes", flush=True)
    open(HERE + "/cali-response.bin", "wb").write(data)
    snap("final")
