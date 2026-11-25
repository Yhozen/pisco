#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/st.sh: step 1's check. Microsoft's SimpleTriangleDesktop (Xbox-GDK-Samples), built for Windows by
# clang-cl (out/gdk-samples/local) and by real MSVC (out/gdk-samples/boat-msvc, tools/build-gdk-sample*.sh), is
# staged with the MSVC C++ runtime DLLs it imports (msvcp140, vcruntime140, vcruntime140_1, lifted with it),
# built, and run in headless Chrome (WebGPU on SwiftShader). Frame 120 is read back from WebGPU: the clear
# colour around a triangle of exactly 960 x 540 / 2 pixels whose centre is ½ red + ¼ green + ¼ blue
# (linear, sRGB-encoded: 187,137,137), from the sample's vertices. Then 600+ frames hashed with no WebGPU
# error, and fixtures/msvc/eh.exe (MSVC C++ exceptions through the real vcruntime140) in Node.
# Environment: REDIST (folder with the x64 CRT redistributable DLLs; default: the 14.44 one fetched into
# ~/untrusted/vcredist from Visual Studio's package manifest). Nothing of Microsoft's goes into the repository.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
REDIST=${REDIST:-$HOME/untrusted/vcredist/x/Contents/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT}
fail=0
for v in local boat-msvc; do
  src=out/gdk-samples/$v; d=out/st-game/$v; o=out/st-$v
  [ -f "$src/SimpleTriangleDesktop.exe" ] || { echo "$v: $src not built, skipped"; continue; }
  rm -rf "$d"; mkdir -p "$d"
  cp -r "$src"/{Assets,MicrosoftGame.config,PixelShader.cso,VertexShader.cso,SimpleTriangleDesktop.exe} "$d"/; cp "$src"/*.dll "$d"/
  cp "$REDIST"/{msvcp140.dll,vcruntime140.dll,vcruntime140_1.dll} "$d"/
  tools/build-title.sh "$d/SimpleTriangleDesktop.exe" "$o" > "$o.build.log" 2>&1 || { echo "$v: build failed ($o.build.log)"; fail=1; continue; }
  node tools/shader-prep.mjs "$d/VertexShader.cso" "$d/PixelShader.cso" > /dev/null
  PAGE='probe=120' DURATION=25 TIMEOUT=60 node tools/run-chrome.mjs "$o/title.wasm" "$d/SimpleTriangleDesktop.exe" > "$o.chrome.log" 2>&1
  p=$(grep -o 'probe frame 120: .*' "$o.chrome.log"); echo "$v: $p"
  [[ "$p" == *"centre 187,137,137,255 corner 65,65,65,255 differ 259200" ]] || fail=1
done
o=out/st-local
PAGE='hash=1&pace=off&stats=1' DURATION=150 TIMEOUT=200 node tools/run-chrome.mjs "$o/title.wasm" out/st-game/local/SimpleTriangleDesktop.exe > "$o.soak.log" 2>&1
frames=$(grep -c '^gpu frame' "$o.soak.log"); hashes=$(grep '^gpu frame' "$o.soak.log" | awk '{print $4}' | sort -u | wc -l); errs=$(grep -c 'WebGPU error\|page exception' "$o.soak.log")
echo "soak: $frames frames, $hashes distinct hash(es), $errs errors"; { [ "$frames" -ge 600 ] && [ "$hashes" = 1 ] && [ "$errs" = 0 ]; } || fail=1
mkdir -p out/msvc-game && cp fixtures/msvc/eh.exe out/msvc-game/ && cp "$REDIST"/{vcruntime140.dll,vcruntime140_1.dll,msvcp140.dll} out/msvc-game/
tools/build-title.sh out/msvc-game/eh.exe out/msvc-eh > out/msvc-eh.build.log 2>&1
timeout 60 node web/run-node.mjs out/msvc-eh/title.wasm out/msvc-game/eh.exe > out/msvc-eh.run.log 2>&1
echo "MSVC exceptions: $(grep -c '^\[PASS\]' out/msvc-eh.run.log) passed, $(grep -c '^\[FAIL\]' out/msvc-eh.run.log) failed"
[ "$(grep -c '^\[PASS\]' out/msvc-eh.run.log)" = 6 ] || fail=1
[ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
