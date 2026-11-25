#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/ndplay.sh: Narbacular Drop played (milestone M5): input, sound and what the game writes.
#   1. Node, the level1-portals session on the virtual clock (into the first level, both portals placed, out of the
#      cage): it must end with exit code 0 after every checkpoint; its sound (SW_AUDIO_WAV) must not be silent, and a
#      second run must give the same samples. With the oracle's +snoop trace of the session
#      (out/oracle/narbacular-drop/portals/wine.log) its FMOD calls are set against Wine's
#      (tools/oracle-wine/fsound-compare.py): the start-up's calls and results must be Wine's.
#   2. Node, the menu-quit session twice with one SW_SAVES folder: the first run finds no system.opt, Quit makes the
#      game write it (out/nd/saves/@game/system.opt, equal to the oracle's when it is there), the second run reads it.
#   3. Chrome (unless NDPLAY_CHROME=0), the level1-portals session drawn: a PNG at every checkpoint
#      (out/nd/play-shots), the table of differences with the oracle's screenshots when they are there; then the
#      menu-quit session in one page and a second page in the same browser profile, which must find system.opt
#      in OPFS and read it.
# Needs out/nd/title.wasm (tests/run.sh nd builds it). Environment: ND (the installed game's folder).
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
exe="$ND/Narbacular Drop.exe"; S=recipes/narbacular-drop/sessions; oracle=out/oracle/narbacular-drop
[ -f out/nd/title.wasm ] || { echo "out/nd/title.wasm is not built (tests/run.sh nd)"; exit 1; }
node_run() { SW_STRICT=1 SW_CLOCK=16667 timeout 900 node web/run-node.mjs out/nd/title.wasm "$exe"; }
level() { python3 -I tools/oracle-wine/audio-compare.py "$1" | sed -E 's/.*; mean ([-0-9.]+) dB, peak ([-0-9.]+) dB; samples sha256 ([0-9a-f]+)$/\1 \2 \3/'; }

# 1. the portals, in Node
SW_D3D9_STATS=1 SW_FMOD_TRACE=1 SW_AUDIO_WAV=out/nd.play.wav SW_SESSION=$S/level1-portals.json node_run > out/nd.play.log 2>&1 || { echo "the session ended with exit code $?:"; tail -3 out/nd.play.log | cut -c1-300; exit 1; }
want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' $S/level1-portals.json)
for n in $want; do grep -q "^pisco: checkpoint $n frame" out/nd.play.log || { echo "no checkpoint $n"; exit 1; }; done
grep -q '^pisco: session done' out/nd.play.log || { echo "the session did not reach its end"; exit 1; }
read -r mean peak sum <<<"$(level out/nd.play.wav)"
awk -v m="$mean" 'BEGIN { exit !(m > -40) }' || { echo "the session's sound is silent (mean $mean dB)"; exit 1; }
SW_AUDIO_WAV=out/nd.play.2.wav SW_SESSION=$S/level1-portals.json node_run > out/nd.play.2.log 2>&1
read -r _ _ sum2 <<<"$(level out/nd.play.2.wav)"
[ "$sum" = "$sum2" ] || { echo "two runs of the session gave different sound ($sum, $sum2)"; exit 1; }
echo "portals (Node): $(wc -w <<<"$want") checkpoints, $(sed -n 's/^pisco: session done: .*, \([0-9]*\) frames$/\1/p' out/nd.play.log) frames; sound mean $mean dB, peak $peak dB, the same samples on a second run"
if [ -f $oracle/portals/wine.log ]; then
  python3 -I tools/oracle-wine/fsound-compare.py $oracle/portals/wine.log out/nd.play.log --quiet > out/nd.play.fsound.txt || { echo "FMOD calls differ from Wine's:"; sed -n '/^events/,/^kinds/p' out/nd.play.fsound.txt | head -8; exit 1; }
  echo "FMOD against Wine: $(grep '^events' out/nd.play.fsound.txt | sed 's/^events (calls not made every frame): //'); $(grep '^kinds' out/nd.play.fsound.txt | sed 's/^kinds of event, handles left out: //')"
else echo "no +snoop trace of the oracle in $oracle/portals: FMOD calls not compared"; fi

# 2. what the game writes, kept between runs (Node)
rm -rf out/nd/saves
SW_TRACE=1 SW_SAVES=out/nd/saves SW_SESSION=$S/menu-quit.json node_run > out/nd.quit.1.log 2>&1 || { echo "menu-quit ended with exit code $?"; exit 1; }
grep -q 'CreateFile system.opt: error 2' out/nd.quit.1.log || { echo "the first run found a system.opt"; exit 1; }
[ -s out/nd/saves/@game/system.opt ] || { echo "Quit did not leave out/nd/saves/@game/system.opt"; exit 1; }
same=""; [ -f $oracle/quit/system.opt ] && { cmp -s out/nd/saves/@game/system.opt $oracle/quit/system.opt && same=", the bytes Wine's run wrote" || { echo "system.opt differs from the one written under Wine"; exit 1; }; }
SW_TRACE=1 SW_SAVES=out/nd/saves SW_SESSION=$S/menu-quit.json node_run > out/nd.quit.2.log 2>&1 || { echo "menu-quit (second run) ended with exit code $?"; exit 1; }
grep -q 'from game/system.opt gave 20 (overlay)' out/nd.quit.2.log || { echo "the second run did not read system.opt"; exit 1; }
echo "saves (Node): Quit wrote system.opt ($(stat -c %s out/nd/saves/@game/system.opt) bytes$same); the next run read it"

[ "${NDPLAY_CHROME:-1}" = 0 ] && exit 0
# 3. Chrome: the portals drawn, then a reload that keeps system.opt
rm -rf out/nd/shots
TIMEOUT=2400 SW_STRICT=1 SW_CLOCK=16667 SW_SESSION=$S/level1-portals.json node tools/run-chrome.mjs out/nd/title.wasm "$exe" > out/nd.play.chrome.log 2>&1 || { echo "the session ended with exit code $? in Chrome:"; tail -3 out/nd.play.chrome.log | cut -c1-300; exit 1; }
grep -q '^pisco: session done' out/nd.play.chrome.log || { echo "the session did not reach its end in Chrome"; exit 1; }
grep -q '^gpu explicit records: .*"skipped":{}}, [0-9]* presents, 0 WebGPU errors$' out/nd.play.chrome.log || { echo "the back end left draws out or saw errors"; exit 1; }
rm -rf out/nd/play-shots; mv out/nd/shots out/nd/play-shots
for n in $want; do [ -s "out/nd/play-shots/$n.png" ] || { echo "no screenshot of checkpoint $n"; exit 1; }; done
echo "portals (Chrome): $(wc -w <<<"$want") checkpoints saved in out/nd/play-shots"
if [ -f $oracle/portals/blue.png ]; then python3 -I tools/oracle-wine/shot-compare.py --table out/nd/play-shots $oracle/portals --diff out/nd/play-shots-diff | tee out/nd.play.shots.txt; fi
rm -rf out/nd/chrome-profile
port=$((9000 + RANDOM % 500))
PORT=$port CHROME_PROFILE=out/nd/chrome-profile TIMEOUT=1200 SW_STRICT=1 SW_CLOCK=16667 SW_SESSION=$S/menu-quit.json node tools/run-chrome.mjs out/nd/title.wasm "$exe" > out/nd.quit.chrome.1.log 2>&1 || { echo "menu-quit ended with exit code $? in Chrome"; exit 1; }
grep -q '^storage: saved game/system.opt (20 bytes)' out/nd.quit.chrome.1.log || { echo "Quit did not store system.opt in OPFS"; exit 1; }
PORT=$port CHROME_PROFILE=out/nd/chrome-profile TIMEOUT=600 SW_STRICT=1 SW_CLOCK=16667 SW_TRACE=1 SW_SESSION_STEPS='wait 1000' node tools/run-chrome.mjs out/nd/title.wasm "$exe" > out/nd.quit.chrome.2.log 2>&1 || { echo "the reloaded page ended with exit code $?"; exit 1; }
grep -q '^storage: 1 file(s) from earlier sessions' out/nd.quit.chrome.2.log && grep -q 'from game/system.opt gave 20 (overlay)' out/nd.quit.chrome.2.log || { echo "the reloaded page did not read system.opt from OPFS"; exit 1; }
echo "saves (Chrome): Quit stored system.opt in OPFS; a new page in the same profile read it"
