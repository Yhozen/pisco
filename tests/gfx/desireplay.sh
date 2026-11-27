#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/desireplay.sh: Desire (DESI:Re v0.1) in Node on the null GPU back end, strict (docs/desire.md).
#   1. `pisco build recipes/desire` from the user's zip and C++ runtime DLLs, its shaders compiled by the build.
#   2. Start-up: the calls Desire.exe makes until its main loop's first PeekMessageW are the ones it makes under Wine
#      (recipes/desire/traces/start.txt), and it runs strict to its 120th frame.
#   3. The maze: given the seeds a Wine run's std::random_device returned (SW_RANDOM), the maze the game writes is
#      that run's, byte for byte (recipes/desire/traces/mazes.txt: three runs, by sha256).
#   4. FH4: an asset taken away; the game throws, catches and says so as under Wine (below).
#   5. Played (D3), strict, on the virtual clock, with the keyboard: the sessions start (the logo skipped, Esc),
#      caught (the enemy finds the player: GAME OVER) and goal (walked to the goal: CLEAR, the ending), each ending
#      with exit code 0; no shader without a translation. With the oracle's runs (ORACLE, default
#      out/oracle/desire): the window's title at each checkpoint is Wine's (tools/oracle-wine/title-compare.py), and
#      every Direct3D 12 method called is one vkd3d saw called (tools/oracle-wine/d3d12-compare.py).
#   DESIRE_ZIP  the zip (DESI-Re-v0.1-Windows-x64.zip). Default: ~/games/DESI-Re-v0.1-Windows-x64.zip
#   REDIST      the x64 C++ runtime DLLs (msvcp140, vcruntime140, vcruntime140_1)
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
zip=${DESIRE_ZIP:-$HOME/games/DESI-Re-v0.1-Windows-x64.zip}
target/release/pisco build recipes/desire --source "$zip" --with REDIST="$REDIST" --out out/img-desire --cache out/pc-desire > out/img-desire.log 2>&1 || { echo "pisco build failed:"; grep -v '^pisco:   ' out/img-desire.log | tail -4; exit 1; }
grep -E '^pisco: (imports|shaders):' out/img-desire.log | sed 's/^pisco: //' | tr '\n' ';' | sed 's/;$/\n/'
SW_STRICT=1 SW_TRACE=2 SW_EXIT_FRAMES=120 SW_PACE=off target/release/pisco run out/img-desire > out/desire.start.log 2>&1 || { echo "the start ended with exit code $?:"; grep -v '^pisco: \[' out/desire.start.log | tail -4 | cut -c1-300; exit 1; }
grep -q 'exit after 120 frames' out/desire.start.log || { echo "the run did not reach its 120th frame"; exit 1; }
grep -q 'no translation' out/desire.start.log && { echo "a shader has no translation:"; grep 'no translation' out/desire.start.log | head -2; exit 1; }
python3 -I tools/oracle-wine/trace-diff.py recipes/desire/traces/start.txt out/desire.start.log --our-image 10000000-100ba000 --unseen GetSystemTimeAsFileTime --until PeekMessageW || exit 1
echo "start: strict to the 120th frame; its calls until the main loop are Wine's"
n=0
while read -r seeds sum; do
  case $seeds in '#'*|'') continue ;; esac
  saves=out/desireplay-saves; rm -rf "$saves"; mkdir -p "$saves"
  SW_STRICT=1 SW_RANDOM=$seeds SW_CLOCK=16667 SW_EXIT_FRAMES=600 target/release/pisco run out/img-desire --saves "$saves" > out/desire.maze.log 2>&1 || { echo "the run with seeds $seeds failed:"; tail -3 out/desire.maze.log | cut -c1-300; exit 1; }
  got=$(sha256sum < "$saves/@game/assets/maps/generated-stage01.map" 2>/dev/null | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "seeds $seeds: the maze is not Wine's (sha256 ${got:-none}, Wine's $sum)"; exit 1; }
  n=$((n + 1))
done < recipes/desire/traces/mazes.txt
echo "mazes: $n of $n are Wine's, byte for byte, from Wine's seeds"
# 4. MSVC's FH4: an asset taken away makes the engine throw and catch (C++ exceptions whose tables are FH4's); under
#    Wine its title becomes "Scene error (Visual Studio Output) | Esc: Quit" and it keeps running. Here too: an image
#    built from a copy of the zip without Textures/WallBrick.png (its recipe without the sha256)
m=out/desire-missing; rm -rf "$m"; mkdir -p "$m"
python3 -I -c 'import sys, zipfile
src, dst = zipfile.ZipFile(sys.argv[1]), zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED)
for i in src.infolist():
    if not i.filename.endswith("/Textures/WallBrick.png"): dst.writestr(i, src.read(i))
dst.close()' "$zip" "$m/missing.zip"
grep -v '^sha256 = ' recipes/desire/recipe.toml | sed 's/^name = "desire"/name = "desire-missing"/' > "$m/recipe.toml"
target/release/pisco build "$m" --source "$m/missing.zip" --with REDIST="$REDIST" --out "$m/img" --cache out/pc-desire > "$m/build.log" 2>&1 || { echo "pisco build of the image without WallBrick.png failed:"; tail -3 "$m/build.log"; exit 1; }
SW_STRICT=1 SW_CLOCK=16667 SW_EXIT_FRAMES=600 target/release/pisco run "$m/img" > "$m/run.log" 2>&1 || { echo "without WallBrick.png the run ended with exit code $?:"; grep -v 'window title' "$m/run.log" | tail -3 | cut -c1-300; exit 1; }
grep -q '^pisco: window title: DESI:Re | Scene error (Visual Studio Output) | Esc: Quit' "$m/run.log" || { echo "without WallBrick.png the game did not show its scene error"; exit 1; }
echo "FH4: without WallBrick.png the game throws, catches, says 'Scene error' as under Wine, and runs on to its 600th frame"
oracle=${ORACLE:-out/oracle/desire}
for s in start caught goal; do
  SW_STRICT=1 SW_CLOCK=16667 SW_CALL_STATS=1 target/release/pisco run out/img-desire --session recipes/desire/sessions/$s.json > out/desire.$s.log 2>&1 || { echo "session $s ended with exit code $?:"; grep -v 'window title' out/desire.$s.log | tail -3 | cut -c1-300; exit 1; }
  grep -q 'no translation' out/desire.$s.log && { echo "session $s: a shader has no translation"; exit 1; }
  shots=$(grep -c '^pisco: checkpoint ' out/desire.$s.log)
  echo "$s: $shots checkpoints, $(grep -c '^pisco: window title' out/desire.$s.log) titles, ended through the game$(grep -o 'session done.*' out/desire.$s.log | sed 's/^/ (/; s/$/)/')"
done
grep -q '^pisco: window title: DESI:Re | GAME OVER' out/desire.caught.log || { echo "caught: no GAME OVER"; exit 1; }
grep -q '^pisco: window title: DESI:Re | CLEAR' out/desire.goal.log || { echo "goal: the goal was not reached (no CLEAR)"; exit 1; }
if [ -d "$oracle/start1" ] && [ -d "$oracle/caught" ]; then
  python3 -I tools/oracle-wine/title-compare.py "$oracle/start1" out/desire.start.log | tail -1 || exit 1
  python3 -I tools/oracle-wine/title-compare.py "$oracle/caught" out/desire.caught.log | tail -1 || exit 1
else echo "titles: no oracle runs in $oracle, not compared"; fi
if [ -f "$oracle/vkd3d/wine.log" ]; then python3 -I tools/oracle-wine/d3d12-compare.py "$oracle/vkd3d/wine.log" out/desire.start.log || exit 1
else echo "Direct3D 12: no vkd3d trace in $oracle/vkd3d, not compared"; fi
SW_STRICT=1 SW_CLOCK=16667 target/release/pisco run out/img-desire --session recipes/desire/sessions/goal.json > out/desire.goal.2.log 2>&1 || { echo "goal, a second time: exit code $?"; exit 1; }
cmp -s <(grep -E '^pisco: (checkpoint|window title)' out/desire.goal.log) <(grep -E '^pisco: (checkpoint|window title)' out/desire.goal.2.log) || { echo "goal: a second run differs in its checkpoints or titles"; exit 1; }
echo "goal again: the same checkpoints and titles"
