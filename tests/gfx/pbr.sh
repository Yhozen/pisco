#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/pbr.sh: step 2's check. Microsoft's SimplePBR (Xbox-GDK-Samples, desktop x64, built by clang-cl with
# all of DirectXTK12: tools/build-gdk-sample-pbr.sh) staged with the MSVC C++ runtime DLLs, built, its DXIL
# shaders translated, then run in headless Chrome (WebGPU on SwiftShader): every frame read back from WebGPU and
# hashed. Passes when no draw is skipped, WebGPU reports no error, and the scene (static without input) hashes
# the same on every frame after the first few. A frame is kept as out/pbr-shots/frame.png for a look by eye:
# the sample's own screenshot (media/image3.PNG) was taken from another camera position, so it is no pixel
# reference. Environment: REDIST as in tests/gfx/st.sh.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
REDIST=${REDIST:-$HOME/untrusted/vcredist/x/Contents/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT}
src=out/gdk-samples/pbr-local; d=out/pbr-game; o=out/pbr
[ -f "$src/SimplePBR.exe" ] || tools/build-gdk-sample-pbr.sh > out/pbr-sample.build.log 2>&1 || { echo "SimplePBR build failed"; exit 1; }
rm -rf "$d"; mkdir -p "$d"
(cd "$src" && cp -r $(ls | grep -v '^obj$\|\.pdb$') "$root/$d"/)
cp "$REDIST"/{msvcp140.dll,vcruntime140.dll,vcruntime140_1.dll} "$d"/
tools/build-app.sh "$d/SimplePBR.exe" "$o" > "$o.build.log" 2>&1 || { echo "build failed ($o.build.log)"; exit 1; }
node tools/shader-prep.mjs "$d/SimplePBR.exe" "$d"/SkyboxEffect_*.cso > "$o.shaders.log" 2>&1 # (DirectXTK effects the sample never uses may fail)
tail -1 "$o.shaders.log"
PAGE='hash=1&pace=off&stats=1' DURATION=120 TIMEOUT=160 node tools/run-chrome.mjs "$o/app.wasm" "$d/SimplePBR.exe" > "$o.chrome.log" 2>&1
frames=$(grep -c '^gpu frame' "$o.chrome.log"); hashes=$(grep '^gpu frame' "$o.chrome.log" | tail -n +6 | awk '{print $4}' | sort -u | wc -l)
errs=$(grep -c 'WebGPU error\|page exception\|has no translation' "$o.chrome.log"); stats=$(grep 'gpu stats' "$o.chrome.log" | tail -1)
echo "Chrome: $frames frames read back, $hashes distinct hash(es) after the first 5, $errs errors; $stats"
mkdir -p out/pbr-shots && rm -f out/pbr-shots/run.mp4*
CAPTURE=out/pbr-shots/run.mp4 DURATION=30 TIMEOUT=60 node tools/run-chrome.mjs "$o/app.wasm" "$d/SimplePBR.exe" > out/pbr-shots/run.log 2>&1
ffmpeg -loglevel error -y -sseof -1 -i out/pbr-shots/run.mp4 -frames:v 1 out/pbr-shots/frame.png && echo "frame: out/pbr-shots/frame.png"
{ [ "$frames" -ge 30 ] && [ "$hashes" = 1 ] && [ "$errs" = 0 ] && [[ "$stats" == *'"skipped":{}'* ]]; } && echo PASS || { echo FAIL; exit 1; }
