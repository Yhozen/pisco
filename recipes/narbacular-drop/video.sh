#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# recipes/narbacular-drop/video.sh [out.mp4]: records Narbacular Drop in headless Chrome (WebGPU on SwiftShader) playing
# a session (default the level1-portals one: the menu, the story, the first level, both portals, out of the cage),
# with its sound, through tools/serve.mjs --capture. The session runs on the virtual clock, so the recording does too:
# each presented frame and the sound are placed by the app's clock, not the wall's (web/browser/main.js), and the
# video plays at the game's speed however slowly SwiftShader draws. Prints the audio track's levels (ffmpeg's
# volumedetect). Needs out/nd/app.wasm (tests/run.sh nd). Environment: ND (the installed game's folder), SESSION.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); cd "$root"
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
out=$(realpath -m "${1:-$root/out/nd/video.mp4}"); mkdir -p "$(dirname "$out")"
TIMEOUT=${TIMEOUT:-3600} SW_STRICT=1 SW_CLOCK=16667 SW_SESSION=${SESSION:-$here/sessions/level1-portals.json} CAPTURE="$out" \
  node tools/run-chrome.mjs out/nd/app.wasm "$ND/Narbacular Drop.exe" > "${out%.mp4}.log" 2>&1 || { echo "the recording failed (${out%.mp4}.log)"; tail -3 "${out%.mp4}.log"; exit 1; }
grep -E "\.mp4: " "${out%.mp4}.log" | tail -1
ffmpeg -hide_banner -i "$out" -map 0:a -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume" | sed 's/^.*\] /audio: /'
echo "$out"
