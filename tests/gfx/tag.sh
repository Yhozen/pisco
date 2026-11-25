#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/tag.sh: Tag: The Power of Paint's pixels (T4). Builds the image from the user's own installer with
# `pisco build recipes/tag`, plays menu-to-level1 in headless Chrome (WebGPU on SwiftShader) in strict mode on the
# virtual clock, and saves the frame presented at each checkpoint as out/img-tag-shots/<name>.png.
# Passes when the session ends with exit code 0, WebGPU reported no error and left no draw out, and every checkpoint
# has its PNG. With the oracle's screenshots of the same session (tools/oracle-wine/oracle.sh play tag ...) the table
# of differences is printed, and level 1's two checkpoints must be within MEAN of Wine's in every channel, on average
# (Narbacular Drop's rule for its still screens). The menu's logo colours and the splash are random and move with
# time, so those checkpoints are printed, not judged.
#   TAG_SETUP  the installer (Tag_setup.exe). Default: ~/games/Tag_setup.exe
#   ORACLE     the oracle's screenshots. Default: out/oracle/tag/d3d9
#   MEAN       default 3 (of 255)
#   TIMEOUT    seconds, default 7000 (SwiftShader draws Tag at one to three frames a second)
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
setup=${TAG_SETUP:-$HOME/games/Tag_setup.exe}; oracle=${ORACLE:-out/oracle/tag/d3d9}; mean=${MEAN:-3}
session=recipes/tag/sessions/menu-to-level1.json
[ -f "$setup" ] || { echo "no installer at $setup (TAG_SETUP)"; exit 1; }
target/release/pisco build recipes/tag --source "$setup" --out out/img-tag --cache out/pc-tag > out/img-tag.log 2>&1 || { echo "pisco build failed:"; tail -3 out/img-tag.log; exit 1; }
rm -rf out/img-tag-shots
start=$(date +%s)
TIMEOUT=${TIMEOUT:-7000} SW_STRICT=1 SW_CLOCK=16667 target/release/pisco run out/img-tag --chrome --session "$session" > out/tag.chrome.log 2>&1; code=$?
took=$(( $(date +%s) - start ))
[ $code = 0 ] || { echo "the session ended with exit code $code:"; grep -v '^pisco:   ' out/tag.chrome.log | tail -4 | cut -c1-300; exit 1; }
grep -q '^pisco: session done' out/tag.chrome.log || { echo "the session did not reach its end:"; tail -3 out/tag.chrome.log | cut -c1-300; exit 1; }
if grep -q -e '^gpu: WebGPU error' -e '^gpu: shader ' -e '^gpu: pipeline ' -e '^page exception' out/tag.chrome.log; then echo "WebGPU refused something:"; grep -e '^gpu: ' -e '^page exception' out/tag.chrome.log | head -4 | cut -c1-300; exit 1; fi
grep -q '^gpu explicit records: .*"skipped":{}}, [0-9]* presents, 0 WebGPU errors$' out/tag.chrome.log || { echo "the back end left draws out or saw errors:"; grep '^gpu explicit records' out/tag.chrome.log | cut -c1-400; exit 1; }
want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' "$session")
for n in $want; do [ -s "out/img-tag-shots/$n.png" ] || { echo "no screenshot of checkpoint $n (out/img-tag-shots/$n.png)"; exit 1; }; done
echo "$(wc -w <<<"$want") checkpoints saved in out/img-tag-shots; $took s"
grep '^gpu explicit records' out/tag.chrome.log | sed -E 's/^gpu explicit records: \{"shaders":([0-9]+),"pipelines":([0-9]+),"passes":([0-9]+),"draws":([0-9]+).*/\1 shaders, \2 pipelines, \4 draws in \3 passes, none left out, no WebGPU error/'
if [ -f "$oracle/menu.png" ]; then
  python3 -I tools/oracle-wine/shot-compare.py --table out/img-tag-shots "$oracle" > out/tag.shots.txt || { cat out/tag.shots.txt; exit 1; }
  cat out/tag.shots.txt
  awk -v max="$mean" 'NR > 1 && $1 ~ /^level1/ { n++; if ($2 > max || $3 > max || $4 > max) { bad++; print "  " $1 " is not close to Wine'"'"'s" } } END { if (bad || n != 2) exit 1 }' out/tag.shots.txt || exit 1
  echo "level 1's two checkpoints are within $mean of Wine's screenshots ($oracle) in every channel, on average"
else echo "no screenshots of the oracle in $oracle: nothing compared"; fi
