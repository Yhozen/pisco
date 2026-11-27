#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/mohaa-chrome.sh: the demo's sessions in headless Chrome, strict, on the virtual clock (needs out/img-mohaa:
# the mohaa step; about an hour on SwiftShader). `menu` (the logos to the menu) and `level` (through the menu into the
# level). Passes when WebGPU reports no error, every checkpoint is saved, and the still screens (the menu, its hover,
# the difficulty menu, the briefing) are within MEAN (default 3) of 255 of the oracle's screenshots in every channel on
# average (out/oracle/mohaa/<session>1: tools/oracle-wine/mohaa-play.sh). The level's own views move (its leaves, its
# soldiers, Wine's wall clock) and are printed, not judged.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
mean=${MEAN:-3}
[ -f out/img-mohaa/index.json ] || { echo "out/img-mohaa is not built (tests/run.sh mohaa)"; exit 1; }
judged=""
for s in ${SESSIONS:-menu level}; do
  rm -rf out/img-mohaa-shots out/mohaa-shots/$s; mkdir -p out/mohaa-shots/$s; start=$(date +%s)
  TIMEOUT=${TIMEOUT:-7000} SW_STRICT=1 SW_CLOCK=16667 target/release/pisco run out/img-mohaa --chrome --session recipes/mohaa/sessions/$s.json > out/mohaa.chrome.$s.log 2>&1; code=$?
  [ $code = 0 ] || { echo "$s ended with exit code $code:"; grep -v '^pisco:   ' out/mohaa.chrome.$s.log | tail -4 | cut -c1-300; exit 1; }
  if grep -q -e '^gpu: WebGPU error' -e '^gpu: shader ' -e '^gpu: pipeline ' -e '^page exception' out/mohaa.chrome.$s.log; then echo "$s: WebGPU refused something:"; grep -e '^gpu: ' -e '^page exception' out/mohaa.chrome.$s.log | head -4 | cut -c1-300; exit 1; fi
  want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' recipes/mohaa/sessions/$s.json)
  for n in $want; do [ -s "out/img-mohaa-shots/$n.png" ] || { echo "$s: no screenshot of checkpoint $n"; exit 1; }; cp "out/img-mohaa-shots/$n.png" out/mohaa-shots/$s/; done
  echo "$s: $(wc -w <<<"$want") checkpoints saved in out/mohaa-shots/$s; $(( $(date +%s) - start )) s"
  oracle=out/oracle/mohaa/${s}1
  if ls "$oracle"/*.png > /dev/null 2>&1; then
    python3 -I tools/oracle-wine/shot-compare.py --table out/mohaa-shots/$s "$oracle" > out/mohaa.shots.$s.txt || { cat out/mohaa.shots.$s.txt; exit 1; }
    sed 's/^/  /' out/mohaa.shots.$s.txt
    keep='menu|hover|difficulty|briefing'
    awk -v max="$mean" -v keep="^($keep)\$" 'NR > 1 && $1 ~ keep { if ($2 > max || $3 > max || $4 > max) { bad++; print "  " $1 " is not close to Wine'"'"'s" } } END { if (bad) exit 1 }' out/mohaa.shots.$s.txt || exit 1
    judged="$judged $(grep -E -o "^($keep) " out/mohaa.shots.$s.txt | tr -d ' ' | tr '\n' ' ')"
  else echo "$s: no screenshots of the oracle in $oracle: not compared"; fi
done
[ -n "$judged" ] && echo "within $mean of Wine's screenshots in every channel, on average:$judged" || echo "nothing compared"
