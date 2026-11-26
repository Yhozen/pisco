#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/desireplay.sh: Desire (DESI:Re v0.1) in Node on the null GPU back end, strict (docs/desire.md).
#   1. `pisco build recipes/desire` from the user's zip and C++ runtime DLLs, its shaders compiled by the build.
#   2. Start-up: the calls Desire.exe makes until its main loop's first PeekMessageW are the ones it makes under Wine
#      (recipes/desire/traces/start.txt), and it runs strict to its 120th frame.
#   3. The maze: given the seeds a Wine run's std::random_device returned (SW_RANDOM), the maze the game writes is
#      that run's, byte for byte (recipes/desire/traces/mazes.txt: three runs, by sha256).
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
  SW_STRICT=1 SW_RANDOM=$seeds SW_EXIT_FRAMES=600 target/release/pisco run out/img-desire --saves "$saves" > out/desire.maze.log 2>&1 || { echo "the run with seeds $seeds failed:"; tail -3 out/desire.maze.log | cut -c1-300; exit 1; }
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
grep -v '^sha256 = ' recipes/desire/title.toml | sed 's/^name = "desire"/name = "desire-missing"/' > "$m/title.toml"
target/release/pisco build "$m" --source "$m/missing.zip" --with REDIST="$REDIST" --out "$m/img" --cache out/pc-desire > "$m/build.log" 2>&1 || { echo "pisco build of the image without WallBrick.png failed:"; tail -3 "$m/build.log"; exit 1; }
SW_STRICT=1 SW_EXIT_FRAMES=600 target/release/pisco run "$m/img" > "$m/run.log" 2>&1 || { echo "without WallBrick.png the run ended with exit code $?:"; grep -v 'window title' "$m/run.log" | tail -3 | cut -c1-300; exit 1; }
grep -q '^pisco: window title: DESI:Re | Scene error (Visual Studio Output) | Esc: Quit' "$m/run.log" || { echo "without WallBrick.png the game did not show its scene error"; exit 1; }
echo "FH4: without WallBrick.png the game throws, catches, says 'Scene error' as under Wine, and runs on to its 600th frame"
