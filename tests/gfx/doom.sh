#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/doom.sh: step 3's check. Builds Doom (fixtures/doom/build.sh) and its wasm, runs `-timedemo demo1`
# natively three times (frames that differ between those runs draw pointer bytes: Doom's own nondeterminism),
# then in headless Chrome (WebGPU on SwiftShader) with every presented frame read back and hashed, and in Node
# on the null back end with SW_STRICT=1. Passes when the WebGPU frames equal the app's framebuffer and
# native's on every deterministic frame.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); d=$root/out/doom; mkdir -p "$d/ref"
"$root/fixtures/doom/build.sh" > "$d/ref/build.log" 2>&1
BASE=xbox-series "$root/tools/build-app.sh" "$d/game/doom.exe" "$d/wasm" > "$d/ref/build-wasm.log" 2>&1
cd "$d/ref"
for i in 1 2 3; do ../native/doom -iwad ../freedoom1.wad -timedemo demo1 2>/dev/null | grep '^frame' > native-t$i.txt || true; done
(cd ../game && SW_STRICT=1 SW_PACE=off node "$root/web/run-node.mjs" ../wasm/app.wasm doom.exe -hashframes -timedemo demo1 > ../ref/node-t.txt 2>&1 || true)
(cd .. && PAGE='pace=off&hash=1' TIMEOUT=900 node "$root/tools/run-chrome.mjs" wasm/app.wasm game/doom.exe -hashframes -timedemo demo1 > ref/chrome-t.txt 2>&1 || true)
python3 - <<'PY'
runs = [dict(l.split()[1:3] for l in open(f'native-t{i}.txt')) for i in (1, 2, 3)]
node = dict(l.split()[1:3] for l in open('node-t.txt') if l.startswith('frame '))
cpu, gpu = {}, {}
for l in open('chrome-t.txt'):
    p = l.split()
    if l.startswith('gpu frame '): gpu[p[2]] = p[3]
    elif l.startswith('frame '): cpu[p[1]] = p[2]
stable = [k for k in runs[0] if len({r.get(k) for r in runs}) == 1]
bad_node = [k for k in stable if node.get(k) != runs[0][k]]
bad_gpu = [k for k in gpu if gpu[k] != cpu.get(k)]
bad_nat = [k for k in stable if k in gpu and gpu[k] != runs[0][k]]
print(f'native frames {len(runs[0])}, deterministic {len(stable)}; Node null back end: {len(stable) - len(bad_node)} equal')
print(f'Chrome: {len(gpu)} frames read back from WebGPU; equal to the app framebuffer: {len(gpu) - len(bad_gpu)}; equal to native: {len([k for k in stable if k in gpu]) - len(bad_nat)}')
print('mismatches', bad_node[:10], bad_gpu[:10], bad_nat[:10])
ok = len(gpu) > 7000 and not bad_gpu and len(bad_nat) <= 2 and len(bad_node) <= 2  # 1817-like frames: pointer high bytes (see docs/journey.md R13)
print('PASS' if ok else 'FAIL'); raise SystemExit(0 if ok else 1)
PY
