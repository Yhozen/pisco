#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/build-ps5-raymarch.sh [out dir, default out/ps5-raymarch]: the PS5 raymarching tech demo
# (https://github.com/Alexius0815/ps5-raymarch-techdemo, GPL-3.0-or-later, by Alexius0815; not part of PISCO and not
# kept in this repository) as a web title: its v1.4 release is downloaded, checked against its SHA-256, and its
# eboot.elf lifted and built against the ps5 base (bases/ps5, docs/ps5.md). The release is the author's own
# homebrew: no Sony firmware, keys or SDK files are fetched or needed.
# Then: node web/run-node.mjs <out>/title.wasm <out>/eboot.elf --smoke-test, or tools/run-chrome.mjs the same,
# or tools/serve.mjs and a browser (docs/ps5.md).
# Environment: ZIP (the release zip, if it is already here).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${1:-$root/out/ps5-raymarch}" && cd "${1:-$root/out/ps5-raymarch}" && pwd)
url=https://github.com/Alexius0815/ps5-raymarch-techdemo/releases/download/v1.4/raymarch-techdemo-1.4.zip
zip_sha=2914476ea7712027c2aeaf248546b82c55e3ec047d4a0f9789ad43ff95b53a92
elf_sha=160ace0f1aa0cac6b404b9ba6798d0f9a5aa1ff4637abb640318a771be11fdf8
zip=${ZIP:-$out/raymarch-techdemo-1.4.zip}
[ -f "$zip" ] || curl -fsSL -o "$zip" "$url"
echo "$zip_sha  $zip" | sha256sum -c --quiet
unzip -o -q -j "$zip" 'raymarch-techdemo-1.4/eboot.elf' 'raymarch-techdemo-1.4/LICENSE' 'raymarch-techdemo-1.4/NOTICE.md' -d "$out"
echo "$elf_sha  $out/eboot.elf" | sha256sum -c --quiet
[ -x "$root/target/release/lifter" ] || (cd "$root" && cargo build --release)
"$root/tools/build-title.sh" "$out/eboot.elf" "$out"
echo "built $out/title.wasm (the demo's license: $out/LICENSE, $out/NOTICE.md)"
