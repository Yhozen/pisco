#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/nd.sh: Narbacular Drop's pixels (milestone M4). Plays the menu-to-level session in headless Chrome
# (WebGPU on SwiftShader) on the virtual clock: Direct3D 9 goes through the fixed-function shaders and the GPU
# ring's explicit records to WebGPU, and the frame presented at each checkpoint is saved as out/nd/shots/<name>.png.
# Passes when the session ends with exit code 0, WebGPU reported no error and left no draw out, and every
# checkpoint has its PNG. With the oracle's screenshots of the same session (tools/oracle-wine/oracle.sh play) the
# table of differences is printed, and the story, controls and tutorial screens (still pictures: they do not depend
# on when the shot is taken) must be close to Wine's in every channel: a mean difference below MEAN.
#   ORACLE  the oracle's screenshots. Default: out/oracle/narbacular-drop/plain if it is there (Wine with D3DX's
#           processor-specific code turned off, as our virtual CPU makes it run: tools/oracle-wine/README.md;
#           MEAN defaults to 0.5 of 255), else out/oracle/narbacular-drop/d3d9 (MEAN defaults to 3).
# Needs out/nd/title.wasm (tests/run.sh nd builds it). Environment: ND (the installed game's folder), TIMEOUT
# (seconds, default 1500).
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
session=recipes/narbacular-drop/sessions/menu-to-level.json
if [ -n "${ORACLE:-}" ]; then oracle=$ORACLE; mean=${MEAN:-3}
elif [ -f out/oracle/narbacular-drop/plain/menu.png ]; then oracle=out/oracle/narbacular-drop/plain; mean=${MEAN:-0.5}
else oracle=out/oracle/narbacular-drop/d3d9; mean=${MEAN:-3}; fi
[ -f out/nd/title.wasm ] || { echo "out/nd/title.wasm is not built (tests/run.sh nd)"; exit 1; }
rm -rf out/nd/shots
start=$(date +%s)
TIMEOUT=${TIMEOUT:-1500} SW_STRICT=1 SW_CLOCK=16667 SW_SESSION=$session node tools/run-chrome.mjs out/nd/title.wasm "$ND/Narbacular Drop.exe" > out/nd.chrome.log 2>&1; code=$?
took=$(( $(date +%s) - start ))
[ $code = 0 ] || { echo "the session ended with exit code $code:"; grep -v '^pisco:   ' out/nd.chrome.log | tail -4 | cut -c1-300; exit 1; }
grep -q '^pisco: session done' out/nd.chrome.log || { echo "the session did not reach its end:"; tail -3 out/nd.chrome.log | cut -c1-300; exit 1; }
if grep -q -e '^gpu: WebGPU error' -e '^gpu: shader ' -e '^gpu: pipeline ' -e '^page exception' out/nd.chrome.log; then echo "WebGPU refused something:"; grep -e '^gpu: ' -e '^page exception' out/nd.chrome.log | head -4 | cut -c1-300; exit 1; fi
grep -q '^gpu explicit records: .*"skipped":{}}, [0-9]* presents, 0 WebGPU errors$' out/nd.chrome.log || { echo "the back end left draws out or saw errors:"; grep '^gpu explicit records' out/nd.chrome.log | cut -c1-400; exit 1; }
want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' "$session")
for n in $want; do [ -s "out/nd/shots/$n.png" ] || { echo "no screenshot of checkpoint $n (out/nd/shots/$n.png)"; exit 1; }; done
grep '^shot ' out/nd.chrome.log | sed -E 's/^shot ([^:]*): frame ([0-9]+) at ([0-9.]+) s,.*; ([0-9]+) frames since the last in ([0-9.]+) s \(([0-9.]+) a second\)$/\1 \2 \3 \4 \5 \6/' > out/nd.shots.times.txt
fps() { awk -v n="$1" '$1 == n { print $6 }' out/nd.shots.times.txt; }
echo "$(wc -w <<<"$want") checkpoints saved in out/nd/shots; $took s; frames a second: menu $(fps menu-play), story $(fps intro-2), level $(fps level)"
grep '^gpu explicit records' out/nd.chrome.log | sed -E 's/^gpu explicit records: \{"shaders":([0-9]+),"pipelines":([0-9]+),"passes":([0-9]+),"draws":([0-9]+).*/\1 shaders, \2 pipelines, \4 draws in \3 passes, none left out, no WebGPU error/'
if [ -f "$oracle/menu.png" ]; then
  python3 -I tools/oracle-wine/shot-compare.py --table out/nd/shots "$oracle" --diff out/nd/shots-diff > out/nd.shots.txt || { cat out/nd.shots.txt; exit 1; }
  cat out/nd.shots.txt
  awk -v max="$mean" 'NR > 1 && $1 ~ /^(intro|controls|tutorial)/ { n++; if ($2 > max || $3 > max || $4 > max) { bad++; print "  " $1 " is not close to Wine'"'"'s" } } END { if (bad || n < 8) exit 1 }' out/nd.shots.txt || { echo "the still screens differ from Wine's"; exit 1; }
  echo "the 8 still screens are within $mean of Wine's screenshots ($oracle) in every channel, on average"
else echo "no screenshots of the oracle in $oracle: nothing compared"; fi
