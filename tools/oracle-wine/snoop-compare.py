# SPDX-License-Identifier: GPL-2.0-or-later
# usage: snoop-compare.py WINE_LOG OUR_LOG --dll NAME [--frame FUNCTION] [--events NAME,NAME...] [--context N]
# Sets the calls a title made into one DLL under Wine against the ones it made in our runtime, for a DLL our
# runtime answers itself (FMOD Ex for Tag: --dll fmodex --frame '?update@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ').
#   WINE_LOG  a WINEDEBUG=+snoop log with the title's own copy of the DLL (it prints each call's arguments on its
#             RET line the first time and on its CALL line after that, the result and the caller every time)
#   OUR_LOG   a run with SW_TRACE_DLL=NAME (rt/process.c: "snoop: RET  Name(arguments) retval=R ret=CALLER")
# A call is its function, the address it was called from and its result: arguments hold the title's pointers and
# handles, which differ between the two runs. Wine's clock is the wall's and ours is virtual, so per-frame calls
# repeat a different number of times in a phase; what is compared:
#   1. the longest common start of the two sequences, with the calls just past it; with --events, of the calls to
#      those functions only (by the name between '?' and the first '@', e.g. createSound,playSound), and only those
#      that succeeded: what the per-frame calls between them are depends on how long each phase lasted;
#   2. each function's calls on both sides, and per frame (the --frame function is called once a frame).
# Exit status 1 if a function is called on one side only.
import collections, re, sys

args = sys.argv[1:]
if len(args) < 4 or '--dll' not in args: raise SystemExit(__doc__ if __doc__ else 'usage: snoop-compare.py WINE_LOG OUR_LOG --dll NAME [--frame FUNCTION] [--context N]')
wine_path, our_path = args[0], args[1]
dll = args[args.index('--dll') + 1].lower()
frame_fn = args[args.index('--frame') + 1] if '--frame' in args else None
context = int(args[args.index('--context') + 1]) if '--context' in args else 3

def wine(path):
    call = re.compile(r'^[0-9a-f]{4}:(?:[0-9a-f]{4}:)?(CALL|RET )\s+' + re.escape(dll) + r'\.([^\s(]+)\((.*)\) (?:retval=([0-9a-f]+) )?ret=([0-9a-f]+)', re.I)
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            if 'snoop' not in line and dll not in line.lower(): continue
            m = call.match(line)
            if m and m.group(1) == 'RET ': out.append((m.group(2), m.group(5), m.group(4)))
    return out
def ours(path):
    call = re.compile(r'snoop: RET  ([^\s(]+)\((.*)\) retval=([0-9a-f]+) ret=([0-9a-f]+)')
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            m = call.search(line)
            if m: out.append((m.group(1), m.group(4), m.group(3)))
    return out

w, o = wine(wine_path), ours(our_path)
events = set(args[args.index('--events') + 1].split(',')) if '--events' in args else None
base = lambda n: n[1:n.index('@')] if n.startswith('?') and '@' in n else n
seq = lambda calls: [c for c in calls if base(c[0]) in events and c[2] == '00000000'] if events else calls
short = lambda n: (n[1:n.index('@@')] if n.startswith('?') and '@@' in n else n).replace('@', '::').split('::')[::-1]
name = lambda n: '::'.join(short(n)) if n.startswith('?') else n
show = lambda c: f'{name(c[0])} from {c[1]} = {c[2]}' if c else '(end)'
sw, so = seq(w), seq(o)
n = 0
while n < len(sw) and n < len(so) and sw[n] == so[n]: n += 1
print(f'== {dll}: Wine {len(w)} calls, here {len(o)}' + (f'; of {",".join(sorted(events))} that succeeded, Wine {len(sw)}, here {len(so)}' if events else '') + f'; the first {n} are the same (function, call site, result)')
for i in range(n, min(n + context, max(len(sw), len(so)))):
    a, b = (sw[i] if i < len(sw) else None), (so[i] if i < len(so) else None)
    print(f'  {i}: {show(a):70s} | {show(b)}      (Wine | here)')
cw, co = collections.Counter(c[0] for c in w), collections.Counter(c[0] for c in o)
fw, fo = (cw.get(frame_fn, 0), co.get(frame_fn, 0)) if frame_fn else (0, 0)
print(f'== calls (Wine | here' + (f'; per frame, {fw} and {fo} frames' if frame_fn else '') + ')')
bad = 0
for f in sorted(set(cw) | set(co), key=lambda f: -max(cw[f], co[f])):
    per = f'   {cw[f] / fw:7.2f} | {co[f] / fo:7.2f}' if frame_fn and fw and fo else ''
    mark = '' if cw[f] and co[f] else '   <- one side only'
    bad += not (cw[f] and co[f])
    print(f'  {name(f):40s} {cw[f]:7d} | {co[f]:7d}{per}{mark}')
rw = collections.Counter((c[0], c[2]) for c in w); ro = collections.Counter((c[0], c[2]) for c in o)
odd = sorted(k for k in set(rw) | set(ro) if k[1] != '00000000' and (rw[k] == 0) != (ro[k] == 0))
if odd: print('== results other than FMOD_OK on one side only: ' + ', '.join(f'{name(f)} = {r} ({rw[(f, r)]} | {ro[(f, r)]})' for f, r in odd))
sys.exit(1 if bad else 0)
