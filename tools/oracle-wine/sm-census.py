# SPDX-License-Identifier: GPL-2.0-or-later
# sm-census.py DIR: Direct3D 9 shader bytecodes (*.bin, one each) -> versions, sizes, opcode counts per shader model,
# modifiers, register types, sampler types. Python standard library only.
import os, struct, sys, hashlib, collections

OPS = {0: 'nop', 1: 'mov', 2: 'add', 3: 'sub', 4: 'mad', 5: 'mul', 6: 'rcp', 7: 'rsq', 8: 'dp3', 9: 'dp4', 10: 'min', 11: 'max',
       12: 'slt', 13: 'sge', 14: 'exp', 15: 'log', 16: 'lit', 17: 'dst', 18: 'lrp', 19: 'frc', 20: 'm4x4', 21: 'm4x3', 22: 'm3x4',
       23: 'm3x3', 24: 'm3x2', 25: 'call', 26: 'callnz', 27: 'loop', 28: 'ret', 29: 'endloop', 30: 'label', 31: 'dcl', 32: 'pow',
       33: 'crs', 34: 'sgn', 35: 'abs', 36: 'nrm', 37: 'sincos', 38: 'rep', 39: 'endrep', 40: 'if', 41: 'ifc', 42: 'else',
       43: 'endif', 44: 'break', 45: 'breakc', 46: 'mova', 47: 'defb', 48: 'defi', 64: 'texcoord', 65: 'texkill', 66: 'tex',
       67: 'texbem', 68: 'texbeml', 69: 'texreg2ar', 70: 'texreg2gb', 71: 'texm3x2pad', 72: 'texm3x2tex', 73: 'texm3x3pad',
       74: 'texm3x3tex', 76: 'texm3x3spec', 77: 'texm3x3vspec', 78: 'expp', 79: 'logp', 80: 'cnd', 81: 'def', 82: 'texreg2rgb',
       83: 'texdp3tex', 84: 'texm3x2depth', 85: 'texdp3', 86: 'texm3x3', 87: 'texdepth', 88: 'cmp', 89: 'bem', 90: 'dp2add',
       91: 'dsx', 92: 'dsy', 93: 'texldd', 94: 'setp', 95: 'texldl', 96: 'breakp'}
REG = {0: 'r', 1: 'v', 2: 'c', 3: 'a/t', 4: 'oPos/oFog/oPts', 5: 'oD', 6: 'oT', 7: 'i', 8: 'oC', 9: 'oDepth', 10: 's', 14: 'b',
       15: 'aL', 17: 'vPos/vFace', 18: 'l', 19: 'p'}
SAMP = {0: '?', 2: '2d', 3: 'cube', 4: 'volume'}
USAGE = {0: 'position', 1: 'blendweight', 2: 'blendindices', 3: 'normal', 4: 'psize', 5: 'texcoord', 6: 'tangent', 7: 'binormal',
         8: 'tessfactor', 9: 'positiont', 10: 'color', 11: 'fog', 12: 'depth', 13: 'sample'}

def regtype(t): return ((t >> 28) & 7) | ((t >> 8) & 0x18)

def decode(data):
    t = struct.unpack('<%dI' % (len(data) // 4), data)
    ver = t[0]; kind = 'vs' if ver >> 16 == 0xFFFE else 'ps'; major, minor = (ver >> 8) & 0xFF, ver & 0xFF
    model = '%s_%d_%s' % (kind, major, {0: '0', 1: 'x'}.get(minor, str(minor)) if major >= 2 else str(minor))
    ops, mods, regs, samplers, usages, comments = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), []
    i = 1
    while i < len(t):
        tok = t[i]
        if tok == 0xFFFF: break
        op = tok & 0xFFFF
        if op == 0xFFFE:
            n = (tok >> 16) & 0x7FFF; comments.append(bytes(data[(i + 1) * 4:(i + 1 + n) * 4])[:4]); i += 1 + n; continue
        n = (tok >> 24) & 0xF
        name = OPS.get(op, 'op%d' % op)
        ctl = (tok >> 16) & 0xFF
        if op == 66 and major >= 2: name = {0: 'texld', 1: 'texldp', 2: 'texldb'}.get(ctl, 'texld?%d' % ctl)
        if op in (41, 45, 94) and ctl: name += '_' + {1: 'gt', 2: 'eq', 3: 'ge', 4: 'lt', 5: 'ne', 6: 'le'}.get(ctl, str(ctl))
        if tok & (1 << 28): mods['predicated'] += 1
        if tok & (1 << 30): mods['coissue'] += 1
        params = t[i + 1:i + 1 + n]
        ops[name] += 1
        if op == 31:  # dcl: usage token, then the register
            u, r = params[0], params[1]
            if regtype(r) == 10: samplers[SAMP.get((u >> 27) & 0xF, '?')] += 1
            elif kind == 'vs' or major >= 3: usages['%s%d' % (USAGE.get(u & 0x1F, '?'), (u >> 16) & 0xF)] += 1
            else: usages['%s%d' % (REG.get(regtype(r), '?'), r & 0x7FF)] += 1
        elif op not in (47, 48, 81):
            for k, p in enumerate(params):
                regs[REG.get(regtype(p), 'type%d' % regtype(p))] += 1
                if k == 0 and op not in (40, 41, 25, 26, 27, 38, 45, 96, 65) or (op == 94 and k == 0):
                    dm = (p >> 20) & 0xF
                    if dm & 1: mods['_sat'] += 1
                    if dm & 2: mods['_pp'] += 1
                    if dm & 4: mods['_centroid'] += 1
                else:
                    if p & (1 << 13): mods['relative addressing'] += 1
                    sm = (p >> 24) & 0xF
                    if sm: mods['src modifier %d' % sm] += 1
        i += 1 + n
    return model, ops, mods, regs, samplers, usages, comments

d = sys.argv[1]
by_model = collections.defaultdict(collections.Counter); allmods = collections.defaultdict(collections.Counter)
allregs = collections.defaultdict(collections.Counter); allsamp = collections.Counter(); allusage = collections.defaultdict(collections.Counter)
models = collections.Counter(); hashes = set(); total = 0
for f in sorted(os.listdir(d)):
    if not f.endswith('.bin'): continue
    data = open(os.path.join(d, f), 'rb').read(); hashes.add(hashlib.sha256(data).hexdigest())
    model, ops, mods, regs, samp, us, com = decode(data)
    models[model] += 1; by_model[model].update(ops); allmods[model].update(mods); allregs[model].update(regs); allsamp.update(samp); allusage[model].update(us)
    ninst = sum(v for k, v in ops.items() if k not in ('dcl', 'def', 'defi', 'defb'))
    total += len(data)
    print('%-12s %-8s %5d bytes %3d instructions (+%d dcl, %d def) comments %s' % (f, model, len(data), ninst, ops['dcl'], ops['def'] + ops['defi'] + ops['defb'], b' '.join(com).decode('latin1')))
print('\n%d shaders (%d distinct), %d bytes; models: %s' % (sum(models.values()), len(hashes), total, dict(models)))
for m in sorted(by_model):
    print('\n%s: %s' % (m, ', '.join('%s %d' % kv for kv in sorted(by_model[m].items(), key=lambda kv: (-kv[1], kv[0])))))
    print('  modifiers: %s' % dict(allmods[m]))
    print('  registers: %s' % dict(allregs[m]))
    print('  declared inputs: %s' % dict(allusage[m]))
print('\nsampler types: %s' % dict(allsamp))
