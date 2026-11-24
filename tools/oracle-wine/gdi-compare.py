#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# gdi-compare.py <wine's gdiprobe.txt> <ours: tests/gdi/fonts.c's output> [--verbose]
# Sets our fonts' answers against Wine's GDI: every measurement line (metrics, kerning, ranges, glyph indices, ABC
# widths, advances, placements) value by value, then the pixels: each D3DX glyph (antialiased, the blue channel)
# and each of the game's 256 characters (one bit a pixel). Prints a summary; exit status 1 if a measurement that
# D3DX or the game reads differs (pixels are reported, not judged: hinting moves Wine's edges).
import sys

def parse(path):
    lines, blocks, cur = {}, {}, None
    for raw in open(path, encoding="latin-1"):
        line = raw.rstrip("\n")
        if line.startswith("#"):
            continue
        if line.startswith("  ") and cur is not None:
            blocks[cur].append(line[2:])
            continue
        words = line.split()
        if not words:
            continue
        if words[0] == "glyph":
            cur = ("glyph", int(words[1]), int(words[2])); blocks[cur] = []
            lines[("glyph_abc", words[1], words[2])] = " ".join(words[3:7])
        elif words[0] == "char":
            cur = ("char", 0, int(words[1])); blocks[cur] = []
        else:
            cur = None
            key = (words[0], words[1]) if len(words) > 1 else (words[0],)
            if words[0] == "placement":
                key = tuple(words[:6])
            lines[key] = line
    return lines, blocks

def main():
    wine, ours = parse(sys.argv[1]), parse(sys.argv[2])
    verbose = "--verbose" in sys.argv
    bad = 0
    # what D3DX and the game read: everything except GetCharABCWidthsW, which neither calls
    for key in sorted(wine[0]):
        a, b = wine[0][key], ours[0].get(key)
        if b is None:
            print(f"missing in ours: {' '.join(key)}"); bad += 1; continue
        if a == b:
            continue
        wa, wb = a.split(), b.split()
        diff = [i for i in range(max(len(wa), len(wb))) if i >= len(wa) or i >= len(wb) or wa[i] != wb[i]]
        judged = key[0] != "abc_w"
        print(f"{'DIFFERS' if judged else 'differs (not read)'}: {' '.join(key)}: {len(diff)} of {len(wa)} values, first at {diff[0]}: wine {wa[diff[0]] if diff[0] < len(wa) else '-'} ours {wb[diff[0]] if diff[0] < len(wb) else '-'}")
        bad += judged
    print(f"measurements: {len(wine[0])} lines compared, {bad} differ where it matters")
    # pixels
    per_font = {}
    for key, rows in wine[1].items():
        other = ours[1].get(key)
        if other is None:
            print(f"no pixels of {key} in ours"); continue
        s = per_font.setdefault(key[1] if key[0] == "glyph" else "chars", {"n": 0, "px": 0, "diff": 0, "sum": 0, "max": 0, "same": 0, "lit_w": 0, "lit_o": 0})
        s["n"] += 1; worst = 0; d = 0
        if key[0] == "glyph":
            for ra, rb in zip(rows, other):
                for i in range(0, min(len(ra), len(rb)), 2):
                    va, vb = int(ra[i:i + 2], 16), int(rb[i:i + 2], 16)
                    s["px"] += 1; s["sum"] += abs(va - vb); worst = max(worst, abs(va - vb)); d += va != vb
                    s["lit_w"] += va not in (0, 0x40); s["lit_o"] += vb not in (0, 0x40)
        else:
            for ra, rb in zip(rows, other):
                for ca, cb in zip(ra, rb):
                    s["px"] += 1; d += ca != cb; s["lit_w"] += ca == "#"; s["lit_o"] += cb == "#"
        s["diff"] += d; s["max"] = max(s["max"], worst); s["same"] += d == 0
        if verbose and d:
            print(f"  {key}: {d} pixels differ, worst {worst}")
    for f, s in per_font.items():
        what = f"font {f} (antialiased, {s['n']} glyphs)" if f != "chars" else f"font 0 (one bit, the game's {s['n']} characters)"
        mean = f", mean difference {s['sum'] / s['px']:.2f} of 255, largest {s['max']}" if f != "chars" else ""
        print(f"pixels, {what}: {s['same']} identical; {s['diff']} of {s['px']} pixels differ ({100 * s['diff'] / s['px']:.2f} %){mean}; text pixels Wine {s['lit_w']}, ours {s['lit_o']}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
