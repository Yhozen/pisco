#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/d3d9-ffp.sh [run.log]: the fixed-function shader generator (rt/win32/d3d9_ffp.c), built natively.
# It writes the WGSL of a representative set of state keys (tests/gfx/d3d9-ffp.c lists them: the vertex formats,
# lighting, fog, points, texture operations and alpha tests docs/narbacular-drop.md names) and every file must
# pass naga's validation. With a run's log (SW_D3D9_STATS=1 prints each shader's key) the keys that run made
# are checked as well. Passes when the generator refused none of the keys and naga accepted every shader.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); out=$root/out/ffp; rm -rf "$out"; mkdir -p "$out"
command -v naga > /dev/null || { echo "naga is not installed (tools/setup-toolchain.sh naga)"; exit 1; }
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$root/rt/win32" "$root/tests/gfx/d3d9-ffp.c" "$root/rt/win32/d3d9_ffp.c" -o "$out/d3d9-ffp"
keys=()
if [ -n "${1:-}" ]; then sed -n 's/^.*Direct3D 9 shader \(vs\|fs\) \([0-9a-f]*\):.*$/\1 \2/p' "$1" | sort -u > "$out/run-keys.txt"; keys=("$out/run-keys.txt"); fi
"$out/d3d9-ffp" "$out" "${keys[@]}" > "$out/list.txt" 2> "$out/summary.txt" || { grep FAILED "$out/list.txt" | head -5; cat "$out/summary.txt"; exit 1; }
bad=0
for f in "$out"/*.wgsl; do
  naga "$f" > "$f.log" 2>&1 || { bad=$((bad + 1)); [ $bad -gt 3 ] || { echo "naga rejects $(basename "$f") ($(grep "^$(basename "$f") " "$out/list.txt" | cut -d' ' -f3-)):"; head -12 "$f.log"; }; }
done
[ $bad = 0 ] || { echo "$bad shaders rejected by naga"; exit 1; }
extra=""; [ -z "${1:-}" ] || extra=", $(wc -l < "$out/run-keys.txt") of them from the run"
echo "$(sed 's/, 0 failures//' "$out/summary.txt")$extra: naga accepts every one"
