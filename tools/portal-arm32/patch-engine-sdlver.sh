#!/usr/bin/env bash
# Portal's engine.so asks SDL_GetWindowWMInfo with an SDL_version of 2.0.14; the KMSDRM video driver of
# Batocera's SDL 2.28 refuses anything older than 2.0.15 ("Version must be 2.0.15 or newer") and the engine
# then dies with "Unable to get window info from SDL". Raise the patch level it announces to 16.
# Run on the tablet (or anywhere python3 exists). Keeps engine.so.orig; safe to run twice.
#   usage: patch-engine-sdlver.sh [path/to/engine.so]
set -euo pipefail
ENGINE=${1:-/userdata/roms/ports/.data/Portal_Linux/game/bin/engine.so}
[[ -f "$ENGINE" ]] || { echo "not found: $ENGINE" >&2; exit 1; }
[[ -f "$ENGINE.orig" ]] || cp -- "$ENGINE" "$ENGINE.orig"
python3 - "$ENGINE" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
# mov byte ptr [ebp-0x52], 0x0e ; mov [ebp-0x54], ax ; lea eax, [ebp-0x54] ; push eax
old = bytes.fromhex("c645ae0e668945ac8d45ac50")
new = bytes.fromhex("c645ae10668945ac8d45ac50")
if d.count(new) == 1 and d.count(old) == 0:
    print("already patched")
    sys.exit(0)
if d.count(old) != 1:
    sys.exit("expected exactly one match, found %d: different engine.so build?" % d.count(old))
d[d.index(old):d.index(old) + len(old)] = new
open(p, "wb").write(d)
print("patched")
PY
