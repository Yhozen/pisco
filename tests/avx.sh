#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/avx.sh: the VEX instructions (crates/lifter/src/avx.rs, rt/avx.h) against a real CPU. fixtures/avx, built for
# Zen 2, is lifted with the exact fused multiply-add (PISCO_FMA_EXACT) and run in Node; what it prints must be
# fixtures/avx/expected.txt, what it printed natively. Then, when this machine has AVX2, FMA and BMI2, the native
# run again, against the same file. Needs musl (tools/setup-toolchain.sh musl).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
out=out/avx; rm -rf "$out"; mkdir -p "$out"
fixtures/avx/build.sh "$out" > /dev/null
OPT='-O2 -DPISCO_FMA_EXACT=1' tools/build-app.sh "$out/avx" "$out" > "$out/build.log" 2>&1 || { tail "$out/build.log"; exit 1; }
grep -q 'not lifted yet, in the other functions: 0 instructions' "$out/build.log" || { grep -B2 -A40 'not lifted' "$out/build.log" | head -60; echo "avx: instructions the lifter does not know"; exit 1; }
node web/run-node.mjs "$out/app.wasm" "$out/avx" > "$out/lifted.txt"
diff fixtures/avx/expected.txt "$out/lifted.txt" || { echo "avx: the lifted results differ from the CPU's"; exit 1; }
if grep -q avx2 /proc/cpuinfo && grep -q ' fma' /proc/cpuinfo && grep -q bmi2 /proc/cpuinfo; then
  "$out/avx" | diff fixtures/avx/expected.txt - || { echo "avx: this CPU prints something else"; exit 1; }
fi
echo "avx: $(tail -1 "$out/lifted.txt"), the same lifted as on the CPU"
