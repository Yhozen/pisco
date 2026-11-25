# SPDX-License-Identifier: GPL-2.0-or-later
# usage: trace-diff.py WINE_LOG OUR_LOG [--image LO-HI] [--until NAME] [--context N]
# Compares what a title asked Windows for under Wine with what it asked our runtime for, and shows the first
# place the two differ.
#   WINE_LOG  a WINEDEBUG=+relay log; only calls made from the title's image count (--image, default 0x400000-0x7fffff)
#   OUR_LOG   the output of a run with SW_TRACE=2 (lines "[thread] dll!Name")
# Both become the sequence of function names with repeats of one name folded ("HeapAlloc x321"): how often a
# function is called in a row depends on the machine (environment variables, locale), which one comes next
# should not. --until NAME stops both at the first call of NAME (e.g. Direct3DCreate9). Exit status 1 if they differ.
# usage: trace-diff.py WINE_LOG --record FILE [--image LO-HI] [--until NAME] writes Wine's folded sequence ("Name xCount"
# a line: names and counts, nothing of the title's) to FILE; such a file can be given as WINE_LOG afterwards, so a
# check does not need Wine or its 800 MB log.
import re, sys

args = sys.argv[1:]; lo, hi, until, context = 0x400000, 0x7fffff, None, 6
for i, a in enumerate(args):
    if a == '--image': lo, hi = (int(x, 16) for x in args[i + 1].split('-'))
    if a == '--until': until = args[i + 1]
    if a == '--context': context = int(args[i + 1])
# the same function under the names the two sides print
SAME = {'RtlAllocateHeap': 'HeapAlloc', 'RtlFreeHeap': 'HeapFree', 'RtlSizeHeap': 'HeapSize', 'RtlReAllocateHeap': 'HeapReAlloc',
        'NtdllDefWindowProc_A': 'DefWindowProcA', 'NtdllDefWindowProc_W': 'DefWindowProcW', 'RtlGetLastWin32Error': 'GetLastError',
        'RtlSetLastWin32Error': 'SetLastError', 'RtlEnterCriticalSection': 'EnterCriticalSection', 'RtlLeaveCriticalSection': 'LeaveCriticalSection',
        'RtlUnwind': 'RtlUnwind', 'RtlZeroMemory': 'RtlZeroMemory'}
# Calls Wine never writes to its relay trace (thread and process ids and the last error are read straight from
# the TEB, locks and interlocked operations are excluded by default, the performance counter is a system call):
# in a 786 MB trace of a whole session none of them appears once. They are left out of our side too.
UNSEEN = {'GetCurrentProcessId', 'GetCurrentThreadId', 'GetCurrentProcess', 'GetCurrentThread', 'SetLastError', 'QueryPerformanceCounter',
          'QueryPerformanceFrequency', 'InterlockedIncrement', 'InterlockedDecrement', 'InterlockedExchange', 'InterlockedCompareExchange',
          'TlsGetValue', 'TlsSetValue', 'EnterCriticalSection', 'LeaveCriticalSection'}
def fold(names):
    out = []
    for n in names:
        n = SAME.get(n, n)
        if n in UNSEEN: continue
        if out and out[-1][0] == n: out[-1][1] += 1
        else: out.append([n, 1])
        if until and n == until: break
    return out
def wine(path):
    call = re.compile(rb'^[0-9a-f]{4}:Call [A-Za-z0-9_.-]+\.([A-Za-z0-9_@?$]+)\(.*\) ret=([0-9a-f]+)')
    with open(path, 'rb') as f:
        for line in f:
            if line[5:10] != b'Call ': continue
            m = call.match(line)
            if m and lo <= int(m.group(2), 16) <= hi: yield m.group(1).decode()
def ours(path):
    call = re.compile(r'\[\d+\] [A-Za-z0-9_.-]+!([A-Za-z0-9_@?$]+)')
    with open(path, errors='replace') as f:
        for line in f:
            m = call.search(line)
            if m: yield m.group(1)
def recorded(path):  # a file --record wrote
    for line in open(path):
        if line.strip() and not line.startswith('#'):
            name, _, count = line.split()[0], None, line.split()[1] if len(line.split()) > 1 else 'x1'
            for _ in range(int(count[1:])): yield name
def is_recorded(path):
    with open(path, 'rb') as f: return f.read(1) == b'#'
w = fold(recorded(args[0]) if is_recorded(args[0]) else wine(args[0]))
if '--record' in args:
    out = args[args.index('--record') + 1]
    with open(out, 'w') as f:
        f.write(f'# Wine\'s calls from the title\'s image, repeats folded' + (f', up to {until}' if until else '') + '. Written by tools/oracle-wine/trace-diff.py --record.\n')
        for n, c in w: f.write(f'{n} x{c}\n')
    print(f'{out}: {len(w)} steps, {sum(c for _, c in w)} calls'); sys.exit(0)
o = fold(ours(args[1]))
n = 0
while n < len(w) and n < len(o) and w[n][0] == o[n][0]: n += 1
show = lambda s, i: f'{s[i][0]} x{s[i][1]}' if i < len(s) else '(end of the trace)'
if n == len(w) == len(o) or (until and n == len(w) and n == len(o)):
    print(f'same sequence: {n} steps ({sum(c for _, c in w)} calls under Wine, {sum(c for _, c in o)} here)'); sys.exit(0)
print(f'the first {n} steps are the same; then:')
for i in range(max(0, n - context), n): print(f'    {show(w, i):40s} | {show(o, i)}')
print(f'  > {show(w, n):40s} | {show(o, n)}      <- Wine | here')
for i in range(n + 1, n + 4): print(f'    {show(w, i):40s} | {show(o, i)}')
sys.exit(1)
