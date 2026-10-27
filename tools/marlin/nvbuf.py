"""Build the 4096-byte config+calibration buffer that Samsung's `download` writes to channel 0.
Layout (bin/download .data tables): config table at 0x90a8 (name ptr, offset, element size) fills buf[0:0x268],
calibration table at 0x9468 fills buf[0x268:]. Elements per key = (next offset - offset) / element size
(the last of each table ends at 0x25f / 0xbb4)."""
import re, struct, sys

D = "/home/nigga/batocera-t280/android-fw/sys/tree/"
BIN = open(D + "bin/download", "rb").read()

def tables():
    # PT_LOAD #2: vaddr 0x8dfc at file 0x7dfc  => file = vaddr - 0x1000 for .data
    def u32(v):
        return struct.unpack_from("<I", BIN, v - 0x1000)[0]
    def cstr(o):
        return BIN[o:BIN.index(b"\0", o)].decode()
    out = []
    for start, end in ((0x90a8, 0x9468), (0x9468, 0x96e4 + 4)):
        ents = []
        v = start
        while v + 12 <= end:
            n, off, sz = u32(v), u32(v + 4), u32(v + 8)
            if not (0x4e08 <= n < 0x73ec):
                break
            ents.append((cstr(n), off, sz))
            v += 12
        out.append(ents)
    return out

def parse_ini(path):
    vals = {}
    for line in open(path, errors="replace"):
        line = line.split("#")[0].strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        nums = []
        for x in v.split(","):
            x = x.strip()
            if x:
                nums.append(int(x, 0))
        vals[k.strip().lower()] = nums
    return vals

def build(conf=D + "etc/connectivity_configure.ini", cali=D + "etc/connectivity_calibration.ini"):
    buf = bytearray(0x1000)
    (ct, kt), ini = tables(), (parse_ini(conf), parse_ini(cali))
    for base, ents, vals, end in ((0, ct, ini[0], 0x25f), (0x268, kt, ini[1], 0xbb4)):
        for i, (name, off, sz) in enumerate(ents):
            nxt = ents[i + 1][1] if i + 1 < len(ents) else end
            cnt = max(1, (nxt - off) // sz)
            nums = vals.get(name.lower())
            if nums is None:
                print("falta", name, file=sys.stderr)
                continue
            fmt = {1: "B", 2: "H", 4: "I"}[sz]
            for j in range(cnt):
                n = nums[j] if j < len(nums) else 0
                struct.pack_into("<" + fmt, buf, base + off + j * sz, n & ((1 << (8 * sz)) - 1))
    return bytes(buf)

if __name__ == "__main__":
    b = build()
    open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/nvbuf.bin", "wb").write(b)
    ct, kt = tables()
    print(len(ct), "config keys,", len(kt), "cali keys;", "first bytes:", b[:16].hex(), "cali:", b[0x268:0x278].hex())
