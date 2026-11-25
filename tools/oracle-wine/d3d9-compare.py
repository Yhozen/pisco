# SPDX-License-Identifier: GPL-2.0-or-later
# usage: d3d9-compare.py WINE_LOG OUR_LOG
# Sets what a title asked Direct3D 9 for under Wine against what it asked our null device for.
#   WINE_LOG  a WINEDEBUG=+d3d9 log of a session (read once: a digest is kept beside it as WINE_LOG.d3d9.json)
#   OUR_LOG   the output of the same session in our runtime with SW_D3D9_STATS=1 and SW_D3D9_FRAMES=1
# Prints three comparisons:
#   methods   the set of methods called on each side. Wine's trace names every method as it is entered
#             ("d3d9_device_Clear iface ..."); only those lines count as calls. Its other lines (reference counts,
#             objects it creates for itself) are listed apart.
#   arguments for the arguments our device counts (states set, formats, pools, usages, flags, primitive types):
#             the values each side saw
#   frames    what one frame is made of: every distinct set of per-frame call counts on each side, and how many
#             frames have a set the other side has too. The two runs differ in how many frames a phase lasts
#             (Wine renders in real time, our clock is virtual), not in what a frame of a phase calls.
# Counts depend on the number of frames, so methods are compared as a set and by calls a frame, never by totals.
import collections, json, os, re, struct, sys

if len(sys.argv) != 3: sys.exit('usage: d3d9-compare.py WINE_LOG OUR_LOG   (see the head of this file)')
wine_path, our_path = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'summarize.py')).read()          # the state names live in summarize.py
tables = {}; exec(src[src.index('RS = {'):src.index('pair = ')], tables)
STATE_NAMES = {'device_SetRenderState': tables['RS'], 'device_SetTextureStageState': tables['TSS'], 'device_SetSamplerState': tables['SAMP'], 'device_SetTransform': tables['TS']}
# Reference counting and interface queries: Wine logs its own along with the title's, under names of its own
REFS = re.compile(r'(^|_)(AddRef|Release|QueryInterface|incref|decref)$')

def number(v):
    if v.startswith('0x'): return int(v, 16)
    if 'e' in v or '.' in v: return struct.unpack('<I', struct.pack('<f', float(v)))[0]
    return int(v) & 0xFFFFFFFF

def wine_digest(path):
    cache = path + '.d3d9.json'
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(path): return json.load(open(cache))
    entry = re.compile(rb'^[0-9a-f]{4}:trace:d3d9:d3d9_([A-Za-z0-9_]+) iface [0-9A-Fa-f]+[,.] ?(.*)')
    other = re.compile(rb'^[0-9a-f]{4}:trace:d3d9:([A-Za-z0-9_]+) ')
    pair = re.compile(rb'(?:^|, )([a-z_]+) (0x[0-9a-f]+|-?\d+(?:\.\d+e[+-]\d+)?)(?=[,.]|$)')
    calls = collections.Counter(); lines = collections.Counter(); args = collections.defaultdict(collections.Counter)
    frames = collections.Counter(); order = []; frame = collections.Counter()
    with open(path, 'rb') as f:
        for line in f:
            if line[5:16] != b'trace:d3d9:': continue
            m = entry.match(line)
            if not m:
                m = other.match(line)
                if m: lines[m.group(1).decode()] += 1
                continue
            name = m.group(1).decode(); calls[name] += 1; frame[name] += 1
            kv = dict((a.decode(), b.decode()) for a, b in pair.findall(m.group(2)))
            if name in STATE_NAMES:
                if 'state' in kv: args[f'{name} {number(kv["state"])}'][str(number(kv.get('value', '0')))] += 1
            else:
                for a, b in kv.items(): args[f'{name} {a}'][str(number(b))] += 1
            if name == 'device_Present':
                key = ', '.join(f'{k} {n}' for k, n in sorted(frame.items()) if not REFS.search(k))
                if not order or order[-1][0] != key: order.append([key, 0])
                order[-1][1] += 1; frames[key] += 1; frame = collections.Counter()
    d = {'calls': calls, 'lines': lines, 'args': args, 'frames': frames, 'order': order}
    json.dump(d, open(cache, 'w'))
    return json.loads(json.dumps(d))

def ours(path):
    calls = {}; args = collections.defaultdict(dict); frames = collections.Counter(); order = []; state = None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('series-web: '): line = line[len('series-web: '):]
        if line.startswith('== Direct3D 9 argument values'): state = 'args'; continue
        if line.startswith('== Direct3D 9:'): state = 'calls'; continue
        m = re.match(r'frames (\d+)-(\d+): ?(.*)', line)
        if m:
            n = int(m.group(2)) - int(m.group(1)) + 1
            key = ', '.join(sorted(x.strip() for x in m.group(3).split(',') if x.strip() and not REFS.search(x.strip().rsplit(' ', 1)[0])))
            frames[key] += n; order.append([key, n]); continue
        if state == 'calls' and line.startswith('  '):
            for item in line.split(','):
                k, n = item.strip().rsplit(' ', 1); calls[k] = int(n)
            state = None
        elif state == 'args' and line.startswith('  ') and ':' in line:
            key, values = line.strip().split(':', 1)
            for v in values.split(','):
                v = v.strip()
                if v and ' x' in v and not v.startswith('...'): val, n = v.rsplit(' x', 1); args[key][str(number(val))] = int(n)
    return {'calls': calls, 'args': args, 'frames': frames, 'order': order}

w = wine_digest(wine_path); o = ours(our_path)
if not o['calls']: sys.exit(f'd3d9-compare.py: {our_path} has no "== Direct3D 9" statistics (run with SW_D3D9_STATS=1)')
wc = w['calls']; oc = o['calls']
frames_w = wc.get('device_Present', 0) or 1; frames_o = oc.get('device_Present', 0) or 1

print(f'== methods: Wine {len(wc)} called ({sum(wc.values())} calls, {frames_w} frames), here {len(oc)} ({sum(oc.values())} calls, {frames_o} frames)')
both = [k for k in wc if k in oc]
print(f'  in both: {len(both)}')
for title, keys, side in (('only under Wine', [k for k in wc if k not in oc], wc), ('only here', [k for k in oc if k not in wc], oc)):
    plain = [k for k in keys if not REFS.search(k)]; refs = [k for k in keys if REFS.search(k)]
    print(f'  {title}: ' + (', '.join(f'{k} {side[k]}' for k in plain) or 'none'))
    if refs: print(f'  {title}, reference counting and interface queries: ' + ', '.join(f'{k} {side[k]}' for k in refs))
print('  Wine\'s other trace lines (no method entry: its own reference counts and objects): ' + ', '.join(f'{k} {n}' for k, n in sorted(w['lines'].items(), key=lambda t: -t[1])))
print('  calls in both, where the count is not a per-frame one (equal counts are not listed):')
once = [(k, wc[k], oc[k]) for k in both if wc[k] != oc[k] and not REFS.search(k) and max(wc[k], oc[k]) < 3 * max(frames_w, frames_o) // 10]
print('    ' + (', '.join(f'{k} {a} | {b}' for k, a, b in once) or 'none') + '      (Wine | here)')
same = [k for k in both if wc[k] == oc[k]]
print(f'  the same number of calls on both sides: {len(same)} methods: ' + ', '.join(f'{k} {wc[k]}' for k in same))

print('== argument values our device counts (value xWine|xHere; only differences in the set of values are marked)')
named = lambda key: (lambda m, a: f'{m} {STATE_NAMES[m].get(int(a), a)}' if m in STATE_NAMES and a.isdigit() else key)(*key.split(' ', 1)) if ' ' in key else key
ok = 0
for key in sorted(o['args'], key=named):
    if key.startswith('draw '): continue
    a = w['args'].get(key, {}); b = o['args'][key]
    if key.startswith('device_SetTransform '): a = {'0': sum(a.values())} if a else {}
    if set(a) == set(b): ok += 1; continue
    only_w = sorted(set(a) - set(b), key=int); only_o = sorted(set(b) - set(a), key=int)
    print(f'  {named(key)}: ' + (f'only under Wine {", ".join(hex(int(v)) + " x" + str(a[v]) for v in only_w)}' if only_w else '') + ('; ' if only_w and only_o else '')
          + (f'only here {", ".join(hex(int(v)) + " x" + str(b[v]) for v in only_o)}' if only_o else '') + ('' if a else ' (Wine\'s trace does not print this argument)'))
print(f'  {ok} arguments took the same set of values on both sides')
missing = sorted(named(k) for k in w['args'] if k not in o['args'] and k.split(' ', 1)[0] in STATE_NAMES)
if missing: print('  states only Wine saw set: ' + ', '.join(missing))
print('  what draws used here (not in Wine\'s trace):')
for key in sorted(o['args']):
    if key.startswith('draw '): print(f'    {key}: ' + ', '.join(f'{hex(int(v))} x{n}' for v, n in sorted(o['args'][key].items(), key=lambda t: -t[1])))

print('== frames: the calls one frame makes (reference counting left out)')
def passed_through(key):   # Wine's texture_2d_(Un)LockRect calls its surface_(Un)LockRect, which its trace shows too
    calls = dict(x.rsplit(' ', 1) for x in key.split(', ') if x)
    for m in ('LockRect', 'UnlockRect'):
        n = int(calls.get('surface_' + m, 0)) - int(calls.get('texture_2d_' + m, 0))
        if 'surface_' + m in calls: calls['surface_' + m] = str(n)
        if calls.get('surface_' + m) == '0': del calls['surface_' + m]
    return ', '.join(f'{k} {v}' for k, v in sorted(calls.items()))
fw = collections.Counter(); fo = collections.Counter(o['frames'])
for k, n in w['frames'].items(): fw[passed_through(k)] += n
if not fo: print('  (no "frames" lines in our log: run with SW_D3D9_FRAMES=1)')
else:
    common = set(fw) & set(fo)
    print(f'  distinct frames: Wine {len(fw)}, here {len(fo)}, the same on both sides {len(common)}')
    print(f'  frames whose calls the other side has too: Wine {sum(fw[k] for k in common)} of {sum(fw.values())}, here {sum(fo[k] for k in common)} of {sum(fo.values())}')
    pick = lambda key, name: next((int(x.rsplit(' ', 1)[1]) for x in key.split(', ') if x.rsplit(' ', 1)[0] == name), 0)
    def phases(order):  # runs of frames by (draws, indexed draws, target changes), the long ones
        out = []
        for key, n in order:
            t = (pick(key, 'device_DrawPrimitive'), pick(key, 'device_DrawIndexedPrimitive'), pick(key, 'device_SetRenderTarget'))
            kind = 'level' if t[2] else 'scene' if t[1] else 'screen'
            if out and out[-1][0] == kind: out[-1][1] += n; out[-1][2].add(t)
            else: out.append([kind, n, {t}])
        return out
    for name, order in (('Wine', w['order']), ('here', o['order'])):
        print(f'  {name}: ' + '; '.join(f'{kind} x{n} (draws {min(t[0] for t in ts)}-{max(t[0] for t in ts)}, indexed {min(t[1] for t in ts)}-{max(t[1] for t in ts)}, targets {max(t[2] for t in ts)})' for kind, n, ts in phases(order)))
    # A kind of frame only one side has, against the nearest kind of the other side: what the difference is made of
    counts = lambda key: dict((k, int(n)) for k, n in (x.rsplit(' ', 1) for x in key.split(', ') if x))
    def nearest(key, others):
        a = counts(key); best = None
        for other in others:
            b = counts(other); d = {k: a.get(k, 0) - b.get(k, 0) for k in set(a) | set(b) if a.get(k, 0) != b.get(k, 0)}
            if best is None or sum(abs(v) for v in d.values()) < sum(abs(v) for v in best.values()): best = d
        return best or {}
    for name, mine, theirs in (('Wine', fw, fo), ('here', fo, fw)):
        rest = sorted(((n, k) for k, n in mine.items() if k not in theirs), reverse=True)
        if not rest: continue
        print(f'  frames only {"under Wine" if name == "Wine" else "here"}: {len(rest)} kinds, {sum(n for n, _ in rest)} frames. Each against the nearest kind of the other side, the calls it has more (+) or fewer (-):')
        diffs = collections.Counter()
        for n, k in rest: diffs[', '.join(f'{m} {v:+d}' for m, v in sorted(nearest(k, theirs).items()))] += n
        for d, n in diffs.most_common(6): print(f'    x{n}: {d}')
        if len(diffs) > 6: print(f'    ... {len(diffs) - 6} more differences, {sum(n for _, n in diffs.most_common()[6:])} frames')
