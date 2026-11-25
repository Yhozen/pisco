#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# census.sh <the title's exe> [out dir]: what lifting and running it will need, without running it.
#  - the lifter's census: images, functions, instructions it cannot lift yet
#  - imports by DLL: how many the runtime implements (the IMP(...) in the files of BASE, default: of every base), and the missing ones
#  - the shader census of the title's folder
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); exe=$1; out=${2:-$root/out/census}; mkdir -p "$out"
dir=$(dirname "$exe")
echo "== lifter"
"$root/target/release/lifter" "$exe" "$out/title.c" --split 1 2>&1 | head -60
echo "== imports (implemented / total, by DLL)"
bases=${BASE:+bases/$BASE/1/base.toml}; files=$(for b in ${bases:-$(cd "$root" && ls bases/*/*/base.toml)}; do d=$(dirname "$b"); "$root/target/release/pisco" sources "base/$(basename "$(dirname "$d")"):$(basename "$d")" --all-libraries; done | sort -u | sed "s#^#$root/#")
grep -ho 'IMP([a-z0-9_]*, *[A-Za-z0-9_]*)' $files | sed -E 's/IMP\(([a-z0-9_]+), *([A-Za-z0-9_]+)\)/\1.dll!\2/' | sort -u > "$out/implemented.txt"
grep -o '"[^"]*![^"]*",' "$out/title.c" | tr -d '",' | sort -u > "$out/imports.txt"
# API sets and console DLLs resolve to the DLL behind them (see find_import in rt/process.c)
canon() { sed -E 's/^api-ms-win-crt-[^!]*!/ucrtbase.dll!/; s/^(api-ms-win-core-|ext-ms-win-)[^!]*!/kernel32.dll!/; s/^vcruntime140[_0-9]*\.dll!/vcruntime140.dll!/'; }
shipped=$(ls "$dir" | tr 'A-Z' 'a-z')
while read -r imp; do
  dll=$(echo "${imp%%!*}" | tr 'A-Z' 'a-z'); echo "$shipped" | grep -qxF "$dll" && continue # a DLL lifted with the title
  c=$(echo "$imp" | canon); name=${c#*!}
  if grep -qxF "$c" "$out/implemented.txt" || grep -qxF "msvcrt.dll!$name" "$out/implemented.txt" || grep -qxF "kernel32.dll!$name" "$out/implemented.txt"; then echo "ok $imp"; else echo "missing $imp"; fi
done < "$out/imports.txt" > "$out/import-status.txt"
awk '{split($2, a, "!"); d=tolower(a[1]); t[d]++; if ($1=="ok") k[d]++} END {for (d in t) printf "  %4d / %-4d %s\n", k[d], t[d], d}' "$out/import-status.txt" | sort -k3
echo "  missing: $(grep -c '^missing' "$out/import-status.txt") (list: $out/import-status.txt)"
echo "== shaders"
"$root/target/release/shader-census" "$dir"
