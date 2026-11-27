# SPDX-License-Identifier: GPL-2.0-or-later
# mohaa-console.py <ours qconsole.log> <Wine's qconsole.log>: the engine's console logs of the same session, set side
# by side once what describes the machine is left out (paths, dates, the CPU, the OpenGL driver and its pixel
# formats, the sound driver, DirectInput, host names, times). Prints the lines that differ; exit code 1 if any do.
import re, sys
MACHINE = re.compile(r'logfile opened|\.pk3 \(|^C:\\|/main$|detecting CPU|^CPU:|DI coop|dinput\.dll|Input Initialization Complete|calling LoadLibrary|PFDs found|PIXELFORMAT|'
                     r'^GL_|\.\.\.(using|ignoring) |not found$|compressed textures|clocks$|DrawBackground|^Hostname|^IP:|Initialization Complete|Working directory|'
                     r'Parse/Load time|CL_InitCGame: |[0-9.]+ seconds$|[Ss]ound|Direct Sound|Wave Out|provider opened|^driver - |speaker setup|^reverb - |samplebits|^speed - |^-+$|S_StopAllSounds|r_lastValidRenderer')
def lines(path):
    text = open(path, encoding='latin-1').read().replace('\r', '')
    return [l for l in text.split('\n') if l.strip() and not MACHINE.search(l)]
ours, wine = lines(sys.argv[1]), lines(sys.argv[2])
n = min(len(ours), len(wine))   # Wine's may stop sooner (the run is stopped from outside)
bad = [(i, a, b) for i, (a, b) in enumerate(zip(ours[:n], wine[:n])) if a != b]
for i, a, b in bad[:20]: print(f'line {i}: ours  {a}\n         Wine  {b}')
print(f'{n - len(bad)} of {n} lines equal ({len(ours)} ours, {len(wine)} Wine\'s, machine lines left out)')
sys.exit(1 if bad else 0)
