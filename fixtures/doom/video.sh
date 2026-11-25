#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/doom/video.sh [out.mp4]: records our console Doom in headless Chrome (WebGPU on SwiftShader): the title
# screen and attract demo, a new game played from scripted keys, a save (F2), a reload of the page (a new title
# process), the save loaded back (F3) from the browser's storage. Then captions are added at the marks.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); game=$root/out/doom/game
out=$(realpath -m "${1:-$root/out/doom/video/doom-pisco.mp4}"); mkdir -p "$(dirname "$out")"
raw=${out%.mp4}.raw.mp4
cd "$root/out/doom"
KEYS="$(cat "$here/session1.json")" KEYS2="$(cat "$here/session2.json")" RELOAD_AT=${RELOAD_AT:-62} DURATION=${DURATION:-98} TIMEOUT=200 \
  CAPTURE="$raw" PAGE=stats=1 node "$root/tools/run-chrome.mjs" "$root/out/doom/wasm/title.wasm" "$game/doom.exe" > "${out%.mp4}.log" 2>&1 || true
grep -E "mp4|storage|stopping|reloading|gpu stats|timed out|fatal" "${out%.mp4}.log" | tail -8 || true
node "$here/captions.mjs" "$raw" "$raw.marks.json" "$out"
echo "$out"
