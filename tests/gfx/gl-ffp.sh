#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/gl-ffp.sh [run.log]: OpenGL's fixed-function shader generator (rt/windows-x86/gl_ffp.c), built natively.
# It writes the vertex stage and the fragment stage of every texture-unit set-up, alpha test, fog mode and the clip
# plane (tests/gfx/gl-ffp.c), and every file must pass naga's validation. With a run's log (SW_GL_STATS=1 prints
# each fragment key) the keys that run made are checked as well.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); out=$root/out/gl-ffp; rm -rf "$out"; mkdir -p "$out"
command -v naga > /dev/null || { echo "naga is not installed (tools/setup-toolchain.sh naga)"; exit 1; }
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Werror -I"$root/rt/windows-x86" "$root/tests/gfx/gl-ffp.c" "$root/rt/windows-x86/gl_ffp.c" -o "$out/gl-ffp"
keys=()
if [ -n "${1:-}" ]; then sed -n 's/^.*OpenGL shader fs \([0-9a-f]*\):.*$/\1/p' "$1" | sort -u > "$out/run-keys.txt"; keys=("$out/run-keys.txt"); fi
"$out/gl-ffp" "$out" "${keys[@]}" > "$out/list.txt" 2> "$out/summary.txt" || { grep FAILED "$out/list.txt" | head -5; cat "$out/summary.txt"; exit 1; }
bad=0
for f in "$out"/*.wgsl; do
  naga "$f" > "$f.log" 2>&1 || { bad=$((bad + 1)); [ $bad -gt 3 ] || { echo "naga rejects $(basename "$f") ($(grep "^$(basename "$f") " "$out/list.txt" | cut -d' ' -f3-)):"; head -12 "$f.log"; }; }
done
[ $bad = 0 ] || { echo "$bad shaders rejected by naga"; exit 1; }
extra=""; [ -z "${1:-}" ] || extra=", $(wc -l < "$out/run-keys.txt") of them from the run"
echo "$(sed 's/, 0 failures//' "$out/summary.txt")$extra: naga accepts every one"
