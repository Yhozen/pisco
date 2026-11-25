# SPDX-License-Identifier: GPL-2.0-or-later
# usage: audio-compare.py OURS.wav [WINE.wav] [--window S]
# How loud a recording is, and how it compares with the oracle's. A WAV of 16-bit PCM or 32-bit float, any rate,
# mono or stereo (ours: SW_AUDIO_WAV, rt/mixer.c; Wine's: oracle.sh's wine-audio.wav). Only Python's standard library.
# For one file: its length, mean and peak level (dB of full scale, as ffmpeg's volumedetect gives them) and a
# checksum of its samples (two runs of a session on the virtual clock must give the same one).
# For two: each one's levels, and their loudness envelopes (RMS level per window, default 1 s) set side by side.
# The two runs do not keep time alike (Wine's clock is the wall's, and it runs while the game loads; ours is
# virtual and does not), so the envelopes are first aligned by cross-correlation, then the mean difference of the
# overlapping windows and the correlation of the two envelopes are reported.
import math, struct, sys, hashlib

def read(path):
    d = open(path, 'rb').read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE': sys.exit(f'{path}: not a WAV file')
    at, fmt = 12, None
    while at + 8 <= len(d):
        cid, n = d[at:at + 4], struct.unpack('<I', d[at + 4:at + 8])[0]; body = d[at + 8:at + 8 + n]
        if cid == b'fmt ':
            fmt = list(struct.unpack('<HHIIHH', body[:16]))
            if fmt[0] == 0xFFFE and len(body) >= 26: fmt[0] = struct.unpack('<H', body[24:26])[0]   # WAVE_FORMAT_EXTENSIBLE: its sub-format
        if cid == b'data' and fmt:
            tag, ch, rate, _, _, bits = fmt
            count = len(body) // (bits // 8)
            if tag == 3 and bits == 32: s = struct.unpack(f'<{count}f', body[:count * 4])
            elif tag == 1 and bits == 16: s = [v / 32768 for v in struct.unpack(f'<{count}h', body[:count * 2])]
            else: sys.exit(f'{path}: format {tag} with {bits} bits is not read here')
            return rate, ch, s, hashlib.sha256(body).hexdigest()[:16]
        at += 8 + n + (n & 1)
    sys.exit(f'{path}: no data')

def db(x): return 20 * math.log10(x) if x > 0 else -120.0

def levels(rate, ch, s):
    frames = len(s) // ch
    mean = math.sqrt(sum(v * v for v in s) / max(len(s), 1)); peak = max((abs(v) for v in s), default=0)
    return frames / rate, db(mean), db(peak)

def envelope(rate, ch, s, window):
    n = int(rate * window) * ch; out = []
    for i in range(0, len(s) - n + 1, n):
        part = s[i:i + n]; out.append(db(math.sqrt(sum(v * v for v in part) / n)))
    return out

args = [a for a in sys.argv[1:] if not a.startswith('--')]
window = float(sys.argv[sys.argv.index('--window') + 1]) if '--window' in sys.argv else 1.0
if '--window' in sys.argv: args.remove(sys.argv[sys.argv.index('--window') + 1])
files = [read(p) for p in args]
for p, (rate, ch, s, digest) in zip(args, files):
    length, mean, peak = levels(rate, ch, s)
    print(f'{p}: {length:.1f} s at {rate} Hz, {ch} channels; mean {mean:.1f} dB, peak {peak:.1f} dB; samples sha256 {digest}')
if len(files) == 2:
    a, b = (envelope(f[0], f[1], f[2], window) for f in files)
    # the shift of b against a that makes the envelopes agree best (silence counts too: levels are clipped at -60 dB)
    clip = lambda e: [max(v, -60) for v in e]; a, b = clip(a), clip(b)
    def corr(x, y):
        n = len(x); mx, my = sum(x) / n, sum(y) / n
        sx = math.sqrt(sum((v - mx) ** 2 for v in x)); sy = math.sqrt(sum((v - my) ** 2 for v in y))
        return sum((u - mx) * (v - my) for u, v in zip(x, y)) / (sx * sy) if sx and sy else 0
    def overlap(k):  # b shifted by k windows against a: the two stretches that cover the same time
        x, y = a[max(0, -k):], b[max(0, k):]; n = min(len(x), len(y)); return x[:n], y[:n]
    best = max(((corr(*overlap(k)), k) for k in range(-len(b) + 10, len(a) - 10)), default=(0, 0))
    r, k = best
    x, y = overlap(k); n = len(x)
    loud = [(u, v) for u, v in zip(x, y) if u > -60 and v > -60]
    diff = sum(v - u for u, v in loud) / len(loud) if loud else 0
    print(f'envelopes ({window:g} s windows): aligned with the second {k * window:+.0f} s against the first; {n} windows overlap, '
          f'{len(loud)} of them sound on both sides; correlation {r:.2f}; the second is {diff:+.1f} dB against the first where both sound')
