# SPDX-License-Identifier: GPL-2.0-or-later
# usage: summarize.py WINE_LOG [--image LO-HI] [--order N] [--exceptions]
# Reads a Wine debug log (WINEDEBUG=+relay, +snoop, +seh, +d3d9 ...) and prints what the title asked for:
#   - Win32 calls made from the title's own image (return address inside --image, default 0x400000-0x7fffff),
#     counted per function, DLL by DLL, in order of first call
#   - with --order N, the first N such calls in order (the start-up sequence)
#   - calls into native DLLs seen by +snoop, per function
#   - Direct3D 9 methods seen by +d3d9, per method, with the values each numeric argument took (states by name)
#   - exceptions raised (+seh), per code, on the title's threads and on the others
# Prints names and counts only: nothing of the title's data.
import re, sys, collections

args = sys.argv[1:]; log = args[0]; lo, hi = 0x400000, 0x7fffff; order = 0
for i, a in enumerate(args):
    if a == '--image': lo, hi = (int(x, 16) for x in args[i + 1].split('-'))
    if a == '--order': order = int(args[i + 1])
call = re.compile(rb'^([0-9a-f]{4}):Call ([A-Za-z0-9_.-]+)\.([A-Za-z0-9_@?$]+)\((.*)\) ret=([0-9a-f]+)')
snoop = re.compile(rb'^([0-9a-f]{4}):CALL ([A-Za-z0-9_.-]+)\.([A-Za-z0-9_@?$]+)\(')
exc = re.compile(rb'^([0-9a-f]{4}):trace:seh:dispatch_exception code=([0-9a-f]+)(?: \(([A-Z_0-9]+)\))? flags=([0-9a-f]+) addr=[0-9A-Fa-f]+(?: \(([^)]*)\))?')
d3d = re.compile(rb'^([0-9a-f]{4}):trace:d3d9:(d3d9_[A-Za-z0-9_]+) (.*)')
RS = {7: 'ZENABLE', 8: 'FILLMODE', 9: 'SHADEMODE', 14: 'ZWRITEENABLE', 15: 'ALPHATESTENABLE', 16: 'LASTPIXEL', 19: 'SRCBLEND', 20: 'DESTBLEND',
      22: 'CULLMODE', 23: 'ZFUNC', 24: 'ALPHAREF', 25: 'ALPHAFUNC', 26: 'DITHERENABLE', 27: 'ALPHABLENDENABLE', 28: 'FOGENABLE', 29: 'SPECULARENABLE',
      34: 'FOGCOLOR', 35: 'FOGTABLEMODE', 36: 'FOGSTART', 37: 'FOGEND', 38: 'FOGDENSITY', 48: 'RANGEFOGENABLE', 52: 'STENCILENABLE', 53: 'STENCILFAIL',
      54: 'STENCILZFAIL', 55: 'STENCILPASS', 56: 'STENCILFUNC', 57: 'STENCILREF', 58: 'STENCILMASK', 59: 'STENCILWRITEMASK', 60: 'TEXTUREFACTOR',
      136: 'CLIPPING', 137: 'LIGHTING', 139: 'AMBIENT', 140: 'FOGVERTEXMODE', 141: 'COLORVERTEX', 142: 'LOCALVIEWER', 143: 'NORMALIZENORMALS',
      145: 'DIFFUSEMATERIALSOURCE', 146: 'SPECULARMATERIALSOURCE', 147: 'AMBIENTMATERIALSOURCE', 148: 'EMISSIVEMATERIALSOURCE', 151: 'VERTEXBLEND',
      152: 'CLIPPLANEENABLE', 154: 'POINTSIZE', 155: 'POINTSIZE_MIN', 156: 'POINTSPRITEENABLE', 157: 'POINTSCALEENABLE', 158: 'POINTSCALE_A',
      159: 'POINTSCALE_B', 160: 'POINTSCALE_C', 161: 'MULTISAMPLEANTIALIAS', 162: 'MULTISAMPLEMASK', 163: 'PATCHEDGESTYLE', 165: 'DEBUGMONITORTOKEN',
      166: 'POINTSIZE_MAX', 167: 'INDEXEDVERTEXBLENDENABLE', 168: 'COLORWRITEENABLE', 170: 'TWEENFACTOR', 171: 'BLENDOP', 172: 'POSITIONDEGREE',
      173: 'NORMALDEGREE', 174: 'SCISSORTESTENABLE', 175: 'SLOPESCALEDEPTHBIAS', 176: 'ANTIALIASEDLINEENABLE', 178: 'MINTESSELLATIONLEVEL',
      179: 'MAXTESSELLATIONLEVEL', 180: 'ADAPTIVETESS_X', 181: 'ADAPTIVETESS_Y', 182: 'ADAPTIVETESS_Z', 183: 'ADAPTIVETESS_W',
      184: 'ENABLEADAPTIVETESSELLATION', 185: 'TWOSIDEDSTENCILMODE', 186: 'CCW_STENCILFAIL', 187: 'CCW_STENCILZFAIL', 188: 'CCW_STENCILPASS',
      189: 'CCW_STENCILFUNC', 190: 'COLORWRITEENABLE1', 191: 'COLORWRITEENABLE2', 192: 'COLORWRITEENABLE3', 193: 'BLENDFACTOR', 194: 'SRGBWRITEENABLE',
      195: 'DEPTHBIAS', 206: 'SEPARATEALPHABLENDENABLE', 207: 'SRCBLENDALPHA', 208: 'DESTBLENDALPHA', 209: 'BLENDOPALPHA',
      **{128 + i: f'WRAP{i}' for i in range(8)}, **{198 + i: f'WRAP{8 + i}' for i in range(8)}}
TSS = {1: 'COLOROP', 2: 'COLORARG1', 3: 'COLORARG2', 4: 'ALPHAOP', 5: 'ALPHAARG1', 6: 'ALPHAARG2', 7: 'BUMPENVMAT00', 8: 'BUMPENVMAT01', 9: 'BUMPENVMAT10',
       10: 'BUMPENVMAT11', 11: 'TEXCOORDINDEX', 22: 'BUMPENVLSCALE', 23: 'BUMPENVLOFFSET', 24: 'TEXTURETRANSFORMFLAGS', 26: 'COLORARG0', 27: 'ALPHAARG0',
       28: 'RESULTARG', 32: 'CONSTANT'}
SAMP = {1: 'ADDRESSU', 2: 'ADDRESSV', 3: 'ADDRESSW', 4: 'BORDERCOLOR', 5: 'MAGFILTER', 6: 'MINFILTER', 7: 'MIPFILTER', 8: 'MIPMAPLODBIAS', 9: 'MAXMIPLEVEL',
        10: 'MAXANISOTROPY', 11: 'SRGBTEXTURE', 12: 'ELEMENTINDEX', 13: 'DMAPOFFSET'}
TS = {2: 'VIEW', 3: 'PROJECTION', **{16 + i: f'TEXTURE{i}' for i in range(8)}, **{256 + i: f'WORLD{i}' for i in range(8)}}
NAMES = {'SetRenderState': RS, 'SetTextureStageState': TSS, 'SetSamplerState': SAMP, 'SetTransform': TS}
pair = re.compile(rb'(?:^|, )([a-z_]+) (0x[0-9a-f]+|-?\d+(?:\.\d+e[+-]\d+)?)(?=[,.]|$)')  # numeric arguments; pointers print as 8 bare hex digits
SKIP = {'iface', 'offset', 'size', 'start_vertex', 'start_idx', 'base_vertex_idx', 'min_vertex_idx', 'vertex_count', 'primitive_count', 'width', 'height',
        'z', 'color', 'index', 'idx', 'stride', 'adapter', 'mode_idx', 'swapchain', 'rect_count'}
num = lambda v: int(v, 16) if v.startswith('0x') else (float(v) if 'e' in v else int(v))
calls = collections.OrderedDict(); first = []; snoops = collections.OrderedDict(); methods = collections.OrderedDict()
states = collections.defaultdict(collections.Counter); excs = collections.Counter(); title_threads = set()
with open(log, 'rb') as f:
    for line in f:
        if line[5:10] == b'Call ':
            m = call.match(line)
            if m and lo <= int(m.group(5), 16) <= hi:
                k = (m.group(2).decode().lower(), m.group(3).decode()); calls[k] = calls.get(k, 0) + 1; title_threads.add(m.group(1))
                if len(first) < order: first.append(f'{k[0]}.{k[1]}({m.group(4).decode(errors="replace")[:60]}) ret={m.group(5).decode()}')
        elif line[5:10] == b'CALL ':
            m = snoop.match(line)
            if m: k = (m.group(2).decode().lower(), m.group(3).decode()); snoops[k] = snoops.get(k, 0) + 1
        elif line[5:15] == b'trace:seh:':
            m = exc.match(line)
            if m: excs[(m.group(1), m.group(2).decode(), (m.group(3) or b'').decode(), (m.group(5) or b'').decode().split('+')[0])] += 1
        elif line[5:16] == b'trace:d3d9:':
            m = d3d.match(line)
            if m:
                k = m.group(2).decode(); methods[k] = methods.get(k, 0) + 1
                kv = [(a.decode(), b.decode()) for a, b in pair.findall(m.group(3))]; short = k.split('_')[-1]
                if short in NAMES:  # state -> the values it was given
                    d = dict(kv); st = d.get('state')
                    if st is not None: states[(k, NAMES[short].get(num(st), st))][d.get('value', '')] += 1
                else:
                    for a, b in kv:
                        if a not in SKIP and not re.fullmatch(r'0[0-9]{7}', b):  # not sizes and counts, not pointers that happen to be all digits
                            states[(k, a)][b] += 1
if calls:
    print(f'== Win32 calls from the image {lo:#x}-{hi:#x}: {len(calls)} functions, {sum(calls.values())} calls')
    by = collections.OrderedDict()
    for (dll, fn), n in calls.items(): by.setdefault(dll, []).append((fn, n))
    for dll, fns in by.items():
        print(f'{dll}: {len(fns)} functions, {sum(n for _, n in fns)} calls')
        print('  ' + ', '.join(f'{fn} {n}' for fn, n in fns))
if first:
    print(f'== first {len(first)} calls'); print('\n'.join(first))
if snoops:
    print(f'== calls into native DLLs (+snoop): {len(snoops)} functions')
    print('  ' + ', '.join(f'{d}.{fn} {n}' for (d, fn), n in snoops.items()))
if methods:
    print(f'== Direct3D 9 (+d3d9): {len(methods)} methods, {sum(methods.values())} calls')
    print('  ' + ', '.join(f'{k[5:]} {n}' for k, n in methods.items()))
    print('== Direct3D 9 argument values (value x count)')
    for (k, a), c in sorted(states.items(), key=lambda t: (t[0][0], str(t[0][1]))):
        top = sorted(c.items(), key=lambda t: -t[1]); more = f' ... {len(top) - 10} more' if len(top) > 10 else ''
        print(f"  {k[5:]} {a}: " + ', '.join(f'{v} x{n}' for v, n in top[:10]) + more)
print(f'== exceptions (+seh): {sum(excs.values())}')
for (tid, code, name, module), n in sorted(excs.items(), key=lambda t: -t[1]):
    print(f"  {n:6d}  code {code} {name:28s} in {module or '?':20s} thread {tid.decode()} {'(the title)' if tid in title_threads else ''}")
