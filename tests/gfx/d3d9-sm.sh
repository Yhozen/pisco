#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/d3d9-sm.sh [bytecode files...]: Direct3D 9's shader translator (rt/windows-x86/d3d9_sm.c), built natively.
# tests/gfx/d3d9-sm.c assembles shaders that cover each opcode, register, modifier and form of flow control of
# shader models 1.1 to 2.x (vertex and pixel) and translates them; every WGSL file must pass naga's validation, and
# the shaders it must refuse are refused. Bytecode files given (a title's shaders captured under Wine, kept in
# out/, never in the repository) are translated and validated too.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); out=$root/out/sm; rm -rf "$out"; mkdir -p "$out"
command -v naga > /dev/null || { echo "naga is not installed (tools/setup-toolchain.sh naga)"; exit 1; }
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$root/rt/windows-x86" "$root/tests/gfx/d3d9-sm.c" "$root/rt/windows-x86/d3d9_sm.c" -lm -o "$out/d3d9-sm"
"$out/d3d9-sm" "$out" "$@" > "$out/list.txt" 2> "$out/summary.txt" || { grep -E 'FAILED|should have' "$out/list.txt" | head -5; cat "$out/summary.txt"; exit 1; }
bad=0
for f in "$out"/*.wgsl; do
  naga "$f" > "$f.log" 2>&1 || { bad=$((bad + 1)); [ $bad -gt 3 ] || { echo "naga rejects $(basename "$f"):"; head -14 "$f.log"; }; }
done
[ $bad = 0 ] || { echo "$bad shaders rejected by naga"; exit 1; }
# a constant expression whose value is not finite: naga lets it pass, Chrome's Tint refuses the module
if grep -l 'bitcast<f32>(0x' "$out"/*.wgsl > "$out/inf.txt" 2>/dev/null && [ -s "$out/inf.txt" ]; then echo "an infinity or NaN written as a constant (Tint refuses it): $(head -3 "$out/inf.txt" | xargs -n1 basename | tr '\n' ' ')"; exit 1; fi
echo "$(cat "$out/summary.txt"), $(grep -c ' refused: ' "$out/list.txt") refused as they must be: naga accepts every one"
