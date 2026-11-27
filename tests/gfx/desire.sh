#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/desire.sh: Desire's pixels (D4). Plays the sessions start, caught and goal in headless Chrome (WebGPU on
# SwiftShader), strict, on the virtual clock, and saves the frame presented at each checkpoint as
# out/desire-shots/<session>/<name>.png. Passes when each session ends with exit code 0, WebGPU reported no error and
# every checkpoint has its PNG. With the oracle's screenshots (ORACLE, default out/oracle/desire: the runs start1 and
# caught, whose seeds the sessions carry) the table of differences is printed, and the still screens must be within
# MEAN of Wine's in every channel, on average: the logo (start), the maze before any input and the GAME OVER picture
# (caught). Not judged: start's maze (Wine's shot, 2.6 s after Enter on lavapipe, is still the fade from black) and
# goal's shots (Wine on lavapipe runs the game too slowly to walk it; docs/desire.md).
#   Needs out/img-desire (tests/run.sh desireplay builds it).  MEAN default 3 (of 255).  TIMEOUT seconds, default 3000.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
oracle=${ORACLE:-out/oracle/desire}; mean=${MEAN:-3}
[ -f out/img-desire/index.json ] || { echo "out/img-desire is not built (tests/run.sh desireplay)"; exit 1; }
judged=""
for s in start caught goal; do
  rm -rf out/img-desire-shots out/desire-shots/$s; mkdir -p out/desire-shots/$s
  start=$(date +%s)
  TIMEOUT=${TIMEOUT:-3000} SW_STRICT=1 SW_CLOCK=16667 target/release/pisco run out/img-desire --chrome --session recipes/desire/sessions/$s.json > out/desire.chrome.$s.log 2>&1; code=$?
  [ $code = 0 ] || { echo "$s ended with exit code $code:"; grep -v 'window title\|^pisco:   ' out/desire.chrome.$s.log | tail -4 | cut -c1-300; exit 1; }
  if grep -q -e '^gpu: WebGPU error' -e '^gpu: shader ' -e '^gpu: pipeline ' -e '^page exception' -e 'no translation' out/desire.chrome.$s.log; then echo "$s: WebGPU refused something:"; grep -e '^gpu: ' -e '^page exception' -e 'no translation' out/desire.chrome.$s.log | head -4 | cut -c1-300; exit 1; fi
  want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' recipes/desire/sessions/$s.json)
  for n in $want; do [ -s "out/img-desire-shots/$n.png" ] || { echo "$s: no screenshot of checkpoint $n"; exit 1; }; cp "out/img-desire-shots/$n.png" out/desire-shots/$s/; done
  echo "$s: $(wc -w <<<"$want") checkpoints saved in out/desire-shots/$s; $(( $(date +%s) - start )) s"
  w=$oracle/$([ $s = start ] && echo start1 || echo $s)
  if [ -d "$w" ] && ls "$w"/*.png > /dev/null 2>&1 && [ $s != goal ]; then
    python3 -I tools/oracle-wine/shot-compare.py --table out/desire-shots/$s "$w" > out/desire.shots.$s.txt || { cat out/desire.shots.$s.txt; exit 1; }
    sed 's/^/  /' out/desire.shots.$s.txt
    keep=$([ $s = start ] && echo logo || echo 'stage|gameover')
    awk -v max="$mean" -v keep="^($keep)\$" 'NR > 1 && $1 ~ keep { n++; if ($2 > max || $3 > max || $4 > max) { bad++; print "  " $1 " is not close to Wine'"'"'s" } } END { if (bad || !n) exit 1 }' out/desire.shots.$s.txt || exit 1
    judged="$judged $(grep -E -o "^($keep) " out/desire.shots.$s.txt | tr -d ' ' | tr '\n' ' ')"
  fi
done
[ -n "$judged" ] && echo "within $mean of Wine's screenshots in every channel, on average:$judged" || echo "no screenshots of the oracle in $oracle: nothing compared"
