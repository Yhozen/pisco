# SPDX-License-Identifier: GPL-2.0-or-later
# usage: fsound-compare.py WINE_LOG OUR_LOG [--quiet]
# Sets the FMOD 3 calls a title made under Wine against the ones it made in our runtime.
#   WINE_LOG  a WINEDEBUG=+snoop log of the title with the shipped fmod.dll unpacked (README.md), with a sound device
#   OUR_LOG   a run with SW_FMOD_TRACE=1 (lines "pisco: fmod: Name(args) = result", rt/win32/fmod3.c)
# Wine's clock is the wall's and ours is virtual, so the two runs differ in how many frames a phase lasts, and so
# in how often the per-frame calls are made and in when the sounds the game plays at random times (torches) start.
# What is compared:
#   1. every function: how often each side called it, and per frame (FSOUND_Update is called once a frame);
#   2. the calls that are not made every frame ("events": loading, opening, playing, stopping, the settings of a new
#      channel), with the samples and streams by file name: the longest common start of the two sequences, then
#      per kind of event (its name and arguments, handles left out) how often each side made it;
#   3. the results: channel handles in the common start; truth values (FMOD returns them in AL, so only the low
#      byte of Wine's EAX counts).
# Exit status 1 if a function is called on one side only or the common start is shorter than the start-up's calls.
import re, sys, collections

PER_FRAME = {'Update', '3D_Listener_SetAttributes', '3D_Listener_SetCurrent', 'SetVolume', 'SetVolumeAbsolute'}
BOOL = {'SetHWND', 'SetMinHardwareChannels', 'SetMemorySystem', 'Init', 'File_SetCallbacks', 'Reverb_SetProperties', 'StopSound', 'SetVolume',
        'SetVolumeAbsolute', 'SetPan', 'SetPaused', 'SetFrequency', 'SetLoopMode', 'SetPriority', 'SetReserved', 'SetCurrentPosition',
        '3D_SetAttributes', 'Stream_Close', 'Stream_SetLoopCount', 'Stream_SetLoopPoints'}
VOID = {'Close', 'SetSFXMasterVolume', '3D_SetDistanceFactor', '3D_SetDopplerFactor', '3D_SetRolloffFactor', '3D_Listener_SetAttributes',
        '3D_Listener_SetCurrent', 'Update'}
HANDLE_RESULT = {'PlaySoundEx', 'Stream_PlayEx'}
SOUND_ARG = {'PlaySoundEx': 1, 'Stream_PlayEx': 1, 'Stream_Close': 0, 'Stream_SetLoopCount': 0, 'Stream_SetLoopPoints': 0}

def split_args(text):  # "0053ee58 \"Audio\\\\x.mp3\",00002002" -> ['0053ee58 "Audio\\x.mp3"', '00002002']
    out, cur, quoted = [], '', False
    for ch in text:
        if ch == '"': quoted = not quoted
        if ch == ',' and not quoted: out.append(cur); cur = ''
        else: cur += ch
    if cur: out.append(cur)
    return [a.replace('\\\\', '\\') for a in out]

def wine(path):
    calls, pending = [], None
    for raw in open(path, 'rb'):
        line = raw.decode('latin1').rstrip()
        m = re.match(r'^[0-9a-f]{4}:CALL fmod\._FSOUND_(\w+)@\d+\((.*)\) ret=', line)
        if m: pending = (m.group(1), m.group(2)); continue
        m = re.match(r'^[0-9a-f]{4}:RET  fmod\._FSOUND_(\w+)@\d+\((.*)\) retval=([0-9a-f]+)', line)
        if m:
            name, args = m.group(1), m.group(2)
            if pending and pending[0] == name and not pending[1].startswith('<unknown'): args = pending[1]
            calls.append((name, split_args(args), m.group(3))); pending = None
    return calls

def ours(path):
    calls = []
    for line in open(path, errors='replace'):
        m = re.match(r'^pisco: fmod: (\w+)\((.*)\) = (\S+)$', line.rstrip())
        if m: calls.append((m.group(1), split_args(m.group(2)), m.group(3)))
    return calls

def events(calls):  # the calls that are not per frame, with sounds by name; (kind, result) pairs
    names, out = {}, []
    for name, args, ret in calls:
        if name in ('Sample_Load', 'Stream_Open'):
            label = re.sub(r'^[0-9a-f]+ ', '', args[1 if name == 'Sample_Load' else 0])
            names[ret.lower().zfill(8)] = label.strip('"')
        if name in PER_FRAME: continue
        shown = list(args)
        if name in SOUND_ARG and len(shown) > SOUND_ARG[name]: shown[SOUND_ARG[name]] = names.get(shown[SOUND_ARG[name]].lower().zfill(8), '?')
        if name in ('Sample_Load', 'Stream_Open'): shown = [re.sub(r'^[0-9a-f]+ ', '', a) for a in shown]
        if name == '3D_SetAttributes': shown = [shown[0]] + ['ptr' if a != '00000000' else '0' for a in shown[1:]]
        if name in ('Reverb_SetProperties', 'SetMemorySystem'): shown = ['ptr', *shown[1:]]   # addresses of the title's own memory
        handle_arg = name not in ('Sample_Load', 'Stream_Open', 'Init', 'SetHWND', 'SetMinHardwareChannels', 'File_SetCallbacks', 'SetSFXMasterVolume',
                                  'Reverb_SetProperties', 'SetMemorySystem') and not name.startswith('3D_Set') or name == '3D_SetAttributes'
        if name in VOID: result = ''
        elif name in BOOL: result = str(int(ret[-2:], 16) != 0) if ret not in ('0', '1') else str(ret == '1')
        elif name in ('Sample_Load', 'Stream_Open'): result = 'sound' if int(ret, 16) else 'none'
        else: result = ret.lower().zfill(8)
        kind = f'{name}({",".join(shown)})'
        general = f'{name}({",".join(["H"] + shown[1:] if handle_arg and shown and name not in ("PlaySoundEx", "Stream_PlayEx") else shown)})'
        out.append((kind, general, result))
    return out

w_calls, o_calls = wine(sys.argv[1]), ours(sys.argv[2]); quiet = '--quiet' in sys.argv
if not w_calls: sys.exit(f'{sys.argv[1]}: no FSOUND calls (a +snoop log of the unpacked fmod.dll?)')
if not o_calls: sys.exit(f'{sys.argv[2]}: no FSOUND calls (run with SW_FMOD_TRACE=1)')
wc, oc = collections.Counter(c[0] for c in w_calls), collections.Counter(c[0] for c in o_calls)
wf, of = wc['Update'] or 1, oc['Update'] or 1
print(f'FSOUND calls: Wine {len(w_calls)}, here {len(o_calls)}; frames (FSOUND_Update): Wine {wc["Update"]}, here {oc["Update"]}')
print(f'  {"function":28s} {"Wine":>8s} {"here":>8s}   {"Wine/frame":>10s} {"here/frame":>10s}')
for n in sorted(set(wc) | set(oc), key=lambda n: (-(wc[n] + oc[n]), n)):
    pf = f'{wc[n] / wf:10.3f} {oc[n] / of:10.3f}' if n in PER_FRAME else ''
    print(f'  {n:28s} {wc[n]:8d} {oc[n]:8d}   {pf}')
only = sorted((set(wc) ^ set(oc)))
we, oe = events(w_calls), events(o_calls)
n = 0
while n < len(we) and n < len(oe) and we[n][0] == oe[n][0] and we[n][2] == oe[n][2]: n += 1
print(f'events (calls not made every frame): Wine {len(we)}, here {len(oe)}; the first {n} are the same, with the same results'
      + (' (all of them)' if n == len(we) == len(oe) else ''))
if n < len(we) or n < len(oe):
    show = lambda s, i: f'{s[i][0]} = {s[i][2]}' if i < len(s) else '(end)'
    for i in range(max(0, n - 2), min(n + 3, max(len(we), len(oe)))): print(f'  {"> " if i == n else "  "}{show(we, i):70s} | {show(oe, i)}')
wk, ok = collections.Counter(e[1] for e in we), collections.Counter(e[1] for e in oe)
same = sum((wk & ok).values())
print(f'kinds of event, handles left out: {len(set(wk) | set(ok))}, {len(set(wk) & set(ok))} made on both sides; '
      f'{same} of Wine\'s {len(we)} events have a counterpart here, {same} of our {len(oe)} there')
if not quiet:
    for k in sorted(set(wk) | set(ok), key=lambda k: (-(wk[k] + ok[k]), k)):
        if wk[k] != ok[k]: print(f'  {k:70s} Wine {wk[k]:4d}  here {ok[k]:4d}')
# the game picks among numbered variants of a sound at random (noknees_walk1..6, torch_pop1..3): plays by family
family = lambda k: re.sub(r'\d+(\.\w+)', r'*\1', k)
wfam, ofam = collections.Counter(family(e[1]) for e in we if e[1].startswith(('PlaySoundEx', 'Stream_PlayEx'))), collections.Counter(family(e[1]) for e in oe if e[1].startswith(('PlaySoundEx', 'Stream_PlayEx')))
print('sounds started, by family (a trailing number dropped):')
for k in sorted(set(wfam) | set(ofam), key=lambda k: -(wfam[k] + ofam[k])): print(f'  {k:70s} Wine {wfam[k]:4d}  here {ofam[k]:4d}')
# per frame, by phase: the level's frames (FSOUND_3D_Listener_SetCurrent: one listener a scene), the menu's (the
# torches' volumes, FSOUND_SetVolume) and the still screens' (neither). The two sides spend different numbers of
# frames in each; within a phase a frame should make the same calls
PHASES = ('still', 'menu', 'level')
def phases(calls):
    out = {p: collections.Counter() for p in PHASES}; frames = dict.fromkeys(PHASES, 0); cur = collections.Counter()
    for name, _, _ in calls:
        cur[name] += 1
        if name == 'Update':
            ph = 'level' if cur['3D_Listener_SetCurrent'] else 'menu' if cur['SetVolume'] else 'still'; frames[ph] += 1; out[ph].update(cur); cur = collections.Counter()
    return out, frames
(wp, wfr), (op, ofr) = phases(w_calls), phases(o_calls)
for ph in PHASES:
    print(f'per frame, {ph} frames (Wine {wfr[ph]}, here {ofr[ph]}): ' + ', '.join(f'{n} {wp[ph][n] / max(wfr[ph], 1):.2f} / {op[ph][n] / max(ofr[ph], 1):.2f}' for n in sorted(PER_FRAME)))
wr = collections.Counter(e[2] for e in we if e[2] in ('True', 'False')); orr = collections.Counter(e[2] for e in oe if e[2] in ('True', 'False'))
print(f'truth values returned (events): Wine {dict(wr)}, here {dict(orr)}')
if only: print(f'called on one side only: {", ".join(only)}')
startup = next((i for i, e in enumerate(we) if e[0].startswith(('Sample_Load', 'Stream_Open'))), len(we))
sys.exit(1 if only or n < startup else 0)
