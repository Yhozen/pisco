# SPDX-License-Identifier: GPL-2.0-or-later
# usage: d3d12-compare.py WINE_LOG OUR_LOG: the Direct3D 12 methods an app called, under Wine (vkd3d's own trace:
# VKD3D_DEBUG=trace; a method vkd3d has as a stub says so as a fixme) and in our run (SW_CALL_STATS=1: the
# "calls before checkpoint ..." lines, rt/windows-x64/dxgi_swapchain.c). IUnknown's methods are left out. Every
# method called here must be one vkd3d saw called; the ones only Wine's side has are printed (Wine's DXGI swap chain
# is built on vkd3d and calls the queue and a fence itself). Exit status 1 if a method is called here only.
import re, sys, collections
CLASS = {'device': 'Device', 'command_list': 'CommandList', 'command_queue': 'CommandQueue', 'descriptor_heap': 'DescriptorHeap',
         'command_allocator': 'CommandAllocator', 'resource': 'Resource', 'fence': 'Fence', 'heap': 'Heap', 'root_signature': 'RootSignature',
         'pipeline_state': 'PipelineState', 'query_heap': 'QueryHeap', 'command_signature': 'CommandSignature'}
UNKNOWN = {'QueryInterface', 'AddRef', 'Release'}
wine = collections.Counter()
pat = re.compile(r'(?:trace|fixme):vkd3d:d3d12_(' + '|'.join(sorted(CLASS, key=len, reverse=True)) + r')_([A-Z][A-Za-z0-9]*)\b')
with open(sys.argv[1], 'rb') as f:
    for line in f:
        m = pat.search(line.decode('latin-1'))
        if m and m.group(2) not in UNKNOWN: wine[f'{CLASS[m.group(1)]}_{m.group(2)}'] += 1
ours = collections.Counter()
for line in open(sys.argv[2], errors='replace'):
    m = re.match(r'pisco: calls before checkpoint \S+: (.*)$', line)
    if not m: continue
    for item in m.group(1).split(', '):
        name, _, n = item.rpartition(' x')
        cls = name.split('_')[0]
        if cls in CLASS.values() and name.split('_', 1)[1] not in UNKNOWN: ours[name] += int(n)
only_here = sorted(set(ours) - set(wine)); only_wine = sorted(set(wine) - set(ours))
print(f'Direct3D 12 methods: {len(set(ours) & set(wine))} called on both sides; here only: {", ".join(only_here) or "none"}; under Wine only: {", ".join(only_wine) or "none"}')
sys.exit(1 if only_here or not ours else 0)
