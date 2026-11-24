#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sets a screenshot of ours against the oracle's of the same checkpoint, in numbers.

  shot-compare.py <ours.png> <wine.png> [--threshold N] [--diff out.png] [--box x0,y0,x1,y1]
  shot-compare.py --table <our folder> <wine's folder> [--threshold N] [--diff <folder>]

One pair prints one line: the mean absolute difference of each channel (0 to 255), the share of pixels where
any channel differs by more than the threshold (default 16), and the largest difference. --table does it for
every <name>.png of our folder that the oracle's folder has too, in the order ours were written (a session's
order). --diff writes the differences as pictures (four times amplified). --box compares only the pixels of
that rectangle (x1, y1 excluded): the text of a checkpoint, say. Only the standard library: PNGs are
read with zlib (8-bit RGB or RGBA, not interlaced: what play.py and tools/serve.mjs write, and more).
"""
import os, struct, sys, zlib


def read_png(path):
    """-> (width, height, rows of RGB bytes joined)"""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n': raise SystemExit(f'{path}: not a PNG')
    at, idat, head = 8, [], None
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8]); body = data[at + 8:at + 8 + n]; at += 12 + n
        if kind == b'IHDR': head = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT': idat.append(body)
    w, h, depth, color, _, _, interlace = head
    if depth != 8 or color not in (2, 6) or interlace: raise SystemExit(f'{path}: only 8-bit RGB / RGBA PNGs without interlacing are read')
    bpp = 3 if color == 2 else 4; stride = w * bpp
    raw = zlib.decompress(b''.join(idat)); out = bytearray(); prev = bytes(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]; row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): row[i] = (row[i] + row[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): row[i] = (row[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): row[i] = (row[i] + (((row[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif f: raise SystemExit(f'{path}: filter {f}')
        prev = bytes(row)
        out += row if bpp == 3 else bytes(v for i, v in enumerate(row) if i % 4 != 3)
    return w, h, bytes(out)


def write_png(path, w, h, rgb):
    rows = b''.join(b'\0' + rgb[y * 3 * w:(y + 1) * 3 * w] for y in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def crop(w, h, rgb, box):
    x0, y0, x1, y1 = box
    return x1 - x0, y1 - y0, b''.join(rgb[3 * (y * w + x0):3 * (y * w + x1)] for y in range(y0, y1))


def compare(ours, wine, threshold, diff=None, box=None):
    """-> None if the sizes differ, else (mean R, mean G, mean B, share of pixels over the threshold, largest difference)"""
    (w, h, a), (w2, h2, b) = read_png(ours), read_png(wine)
    if (w, h) != (w2, h2): return None
    if box: (w, h, a), (_, _, b) = crop(w, h, a, box), crop(w2, h2, b, box)
    d = bytes(abs(x - y) for x, y in zip(a, b))
    n = w * h
    means = [sum(d[c::3]) / n for c in range(3)]
    worst = bytes(map(max, d[0::3], d[1::3], d[2::3]))
    over = sum(1 for v in worst if v > threshold) / n
    if diff: write_png(diff, w, h, bytes(min(255, 4 * v) for v in d))
    return (*means, over, max(d))


def line(r): return 'sizes differ' if r is None else f'{r[0]:6.2f} {r[1]:6.2f} {r[2]:6.2f}   {100 * r[3]:6.2f} %   {r[4]:3d}'


def main():
    args = sys.argv[1:]; threshold, diff, table, box = 16, None, False, None
    for flag in ('--threshold', '--diff', '--box'):
        if flag in args:
            i = args.index(flag); value = args[i + 1]; del args[i:i + 2]
            if flag == '--threshold': threshold = int(value)
            elif flag == '--box': box = tuple(int(v) for v in value.split(','))
            else: diff = value
    if '--table' in args: args.remove('--table'); table = True
    if len(args) != 2: raise SystemExit(__doc__)
    head = f'mean |difference| R, G, B     pixels over {threshold}   largest'
    if not table:
        print(head); print(line(compare(args[0], args[1], threshold, diff, box))); return
    names = [f[:-4] for f in sorted(os.listdir(args[0]), key=lambda f: os.path.getmtime(os.path.join(args[0], f))) if f.endswith('.png') and os.path.exists(os.path.join(args[1], f))]
    if not names: raise SystemExit(f'no <name>.png of {args[0]} is in {args[1]} too')
    if diff: os.makedirs(diff, exist_ok=True)
    width = max(len(n) for n in names)
    print(f'{"checkpoint":{width}}   {head}')
    for n in names:
        print(f'{n:{width}}   {line(compare(os.path.join(args[0], n + ".png"), os.path.join(args[1], n + ".png"), threshold, diff and os.path.join(diff, n + ".png")))}')


if __name__ == '__main__': main()
