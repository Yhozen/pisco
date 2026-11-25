#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/tagplay.sh: Tag played (T5), in Node on the null GPU back end, strict, on the virtual clock.
#   1. level2-paint: through level 1 into level 2, the gun, a shot at the floor and at a wall, the erase; every
#      checkpoint is reached and the GPU records are consistent.
#   2. FMOD Ex against Wine: the run's calls (SW_TRACE_DLL=fmodex) set against the oracle's +snoop log of
#      menu-to-level1 (tools/oracle-wine/snoop-compare.py); the loads, plays, pauses and settings must agree, call
#      site and result, for at least EVENTS calls (Wine's run diverges where its phases end at other times).
#   3. Saves: the TAG.ini the game wrote is kept in the saves folder; edited there (800x600), a second run makes its
#      render targets at that size.
# Needs out/img-tag (tests/run.sh tag builds it, or: pisco build recipes/tag --source Tag_setup.exe --out out/img-tag).
#   WINE_SNOOP  the oracle's log. Default: out/oracle/tag/snoop/wine.log (without it, part 2 is skipped)
#   EVENTS      default 500
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
snoop=${WINE_SNOOP:-out/oracle/tag/snoop/wine.log}; events=${EVENTS:-500}
[ -f out/img-tag/index.json ] || { echo "out/img-tag is not built (tests/run.sh tag)"; exit 1; }
saves=out/tagplay-saves; rm -rf "$saves"; mkdir -p "$saves"
SW_STRICT=1 SW_CLOCK=16667 SW_TRACE_DLL=fmodex target/release/pisco run out/img-tag --session recipes/tag/sessions/level2-paint.json --saves "$saves" > out/tagplay.log 2>&1; code=$?
[ $code = 0 ] || { echo "level2-paint ended with exit code $code:"; grep -v '^pisco:   \|snoop:' out/tagplay.log | tail -4 | cut -c1-300; exit 1; }
want=$(python3 -I -c 'import json, sys; print(" ".join(s["shot"] for s in json.load(open(sys.argv[1]))["steps"] if "shot" in s))' recipes/tag/sessions/level2-paint.json)
for n in $want; do grep -q "^pisco: checkpoint $n frame " out/tagplay.log || { echo "checkpoint $n was not reached"; exit 1; }; done
grep -q ' 0 of the records were inconsistent' out/tagplay.log || { echo "the GPU records were inconsistent:"; grep 'pisco: gpu' out/tagplay.log | head -3 | cut -c1-300; exit 1; }
frames=$(sed -n 's/^pisco: session done: [0-9]* steps, \([0-9]*\) frames$/\1/p' out/tagplay.log)
echo "level2-paint: $(wc -w <<<"$want") checkpoints, $frames frames, GPU records consistent"
if [ -f "$snoop" ]; then
  grep 'snoop:' out/tagplay.log > out/tagplay.fmod.txt
  python3 -I tools/oracle-wine/snoop-compare.py "$snoop" out/tagplay.fmod.txt --dll fmodex --frame '?update@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ' \
    --events createSound,createStream,playSound,setPaused,setMode,init,set3DSettings,getName --context 2 > out/tagplay.fmod.cmp.txt; cmp=$?
  same=$(sed -n 's/.*; the first \([0-9]*\) are the same.*/\1/p' out/tagplay.fmod.cmp.txt | head -1)
  [ $cmp = 0 ] && [ "${same:-0}" -ge "$events" ] || { echo "FMOD Ex against Wine:"; head -12 out/tagplay.fmod.cmp.txt | cut -c1-200; exit 1; }
  echo "FMOD Ex against Wine: the first $same loads, plays, pauses and settings are the same (call site, result); every function is called on both sides"
else echo "FMOD Ex: no oracle log at $snoop, not compared"; fi
ini="$saves/@game/game data/tag.ini"
[ -f "$ini" ] || { echo "TAG.ini was not kept in $saves"; exit 1; }
sed -i 's/^uNormalWidth = [0-9]*/uNormalWidth = 800/; s/^uNormalHeight = [0-9]*/uNormalHeight = 600/' "$ini"
python3 -I -c 'import json, sys; s = json.load(open(sys.argv[1])); st = s["steps"]; i = next(k for k, x in enumerate(st) if x.get("shot") == "menu"); s["steps"] = st[:i + 1]; json.dump(s, open(sys.argv[2], "w"))' recipes/tag/sessions/level2-paint.json out/tagplay-menu.json
SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 target/release/pisco run out/img-tag --session out/tagplay-menu.json --saves "$saves" > out/tagplay.reload.log 2>&1 || { echo "the second run failed:"; tail -3 out/tagplay.reload.log | cut -c1-300; exit 1; }
grep -q 'device_CreateTexture width:.* 0x320 x' out/tagplay.reload.log && grep -q 'device_CreateTexture height:.* 0x258 x' out/tagplay.reload.log || { echo "the second run did not draw at 800x600 from the saved TAG.ini"; exit 1; }
echo "saves: TAG.ini kept, and read back by a second run (its render targets 800x600)"
