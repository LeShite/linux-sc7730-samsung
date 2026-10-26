#!/usr/bin/env python3
"""Make the speaker test files: bootpart/audiotest/*.wav (mono, 16 bit, 44.1 kHz).

music.wav: 8 s (peak -3 dBFS) of NES-like music (square lead arpeggio over a triangle bass), the kind of
rich-in-harmonics sound where a defect in the audio path shows. blip.wav: a short 2 kHz blip
that announces each variant (1 blip = first ...).
"""
import math
import struct
import wave
from pathlib import Path

OUT = Path.home() / "batocera-t280/bootpart/audiotest"
RATE = 44100


def write(name, samples):
    with wave.open(str(OUT / name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v)))) for v in samples))


def env(i, n, fade=0.01):
    f = int(RATE * fade)
    return min(1, i / f, (n - i) / f)


def square(freq, i, duty=0.5):
    return 1.0 if (freq * i / RATE) % 1.0 < duty else -1.0


def tri(freq, i):
    x = (freq * i / RATE) % 1.0
    return 4 * abs(x - 0.5) - 1


for old in OUT.glob("*.wav"):
    old.unlink()
OUT.mkdir(parents=True, exist_ok=True)

# C major, A minor, F major, G major arpeggios, 0.125 s per note
chords = [(261.63, 329.63, 392.0), (220.0, 261.63, 329.63), (174.61, 220.0, 261.63), (196.0, 246.94, 293.66)]
bass = [65.41, 55.0, 43.65, 49.0]
note = int(RATE * 0.125)
amp_lead, amp_bass = 32767 * 10 ** (-12 / 20), 32767 * 10 ** (-12 / 20)
out = []
for rep in range(2):
    for c, chord in enumerate(chords):
        for k in range(8):                       # 8 notes per chord = 1 s
            f = chord[[0, 1, 2, 1][k % 4]] * (2 if k >= 4 else 1)
            for i in range(note):
                t = (rep * 4 + c) * 8 * note + k * note + i
                out.append(amp_lead * 0.6 * square(f, t, 0.25) * env(i, note)
                           + amp_bass * 0.4 * tri(bass[c], t))
# loud, like real music: scale to a -3 dBFS peak (an earlier -12 dBFS test sounded clean and misled us)
peak = max(abs(v) for v in out)
out = [v * 32767 * 10 ** (-3 / 20) / peak for v in out]
write("music.wav", out)

# the same music at 48 kHz (PipeWire's default graph rate): is the DAC clean at that rate too?
RATE48 = 48000
with wave.open(str(OUT / "music48.wav"), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE48)
    n48 = int(len(out) * RATE48 / RATE)
    w.writeframes(b"".join(struct.pack("<h", int(out[min(len(out) - 1, int(i * RATE / RATE48))])) for i in range(n48)))
blip = [32767 * 10 ** (-24 / 20) * math.sin(2 * math.pi * 2000 * i / RATE) * env(i, int(RATE * 0.15), 0.01)
        for i in range(int(RATE * 0.15))]
write("blip.wav", blip)

# stereo check: lead only on the left, bass only on the right (same -3 dBFS scaling as music.wav)
left, right = [], []
for rep in range(2):
    for c, chord in enumerate(chords):
        for k in range(8):
            f = chord[[0, 1, 2, 1][k % 4]] * (2 if k >= 4 else 1)
            for i in range(note):
                t = (rep * 4 + c) * 8 * note + k * note + i
                left.append(amp_lead * square(f, t, 0.25) * env(i, note))
                right.append(amp_bass * tri(bass[c], t))
scale = 32767 * 10 ** (-3 / 20) / max(max(abs(v) for v in left), max(abs(v) for v in right))
with wave.open(str(OUT / "stereo.wav"), "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<hh", int(a * scale), int(b * scale)) for a, b in zip(left, right)))
print(sorted(p.name for p in OUT.iterdir()))
