#!/bin/bash
# tests/gfx/xcl.sh: step 4's check. XCL's console test title (github.com/ivall/xcl, tests/testtitle, built into
# out/xcltitle-game) against the current runtime: compiles the HLSL it hands D3DCompile ahead of time
# (tools/hlsl-prep.mjs: a Node run logs the sources), then runs it in headless Chrome (WebGPU on SwiftShader)
# where its 50 checks run, the D3D12.X ones reading pixels back from the GPU. Passes when none fails.
# Needs out/ffmpeg (tools/build-ffmpeg-audio.sh) for the XMA2 / Opus voices.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
exe=out/xcltitle-game/xcl_testtitle.exe; o=out/regress-xcl
[ -f out/ffmpeg/lib/libavcodec.a ] || tools/build-ffmpeg-audio.sh
tools/build-title.sh "$exe" "$o" > "$o.build.log" 2>&1
# up to three rounds: a shader compiled now lets the title reach the next D3DCompile
for i in 1 2 3; do
  timeout 600 node web/run-node.mjs "$o/title.wasm" "$exe" --hello > "$o.run.log" 2>&1 || true
  grep -q 'd3dcompile-source' "$o.run.log" || break
  node tools/hlsl-prep.mjs "$o.run.log" --title "$(dirname "$exe")"
done
echo "Node (null back end): $(grep -c '^\[PASS\]' "$o.run.log") passed, $(grep -c '^\[FAIL\]' "$o.run.log") failed"
PAGE='pace=off' TIMEOUT=600 node tools/run-chrome.mjs "$o/title.wasm" "$exe" --hello > "$o.chrome.log" 2>&1 || true
pass=$(grep -c '^\[PASS\]' "$o.chrome.log" || true); bad=$(grep -c '^\[FAIL\]' "$o.chrome.log" || true)
echo "Chrome (WebGPU): $pass passed, $bad failed"; grep '^\[FAIL\]' "$o.chrome.log" || true
[ "$pass" = 50 ] && [ "$bad" = 0 ] && echo PASS || { echo FAIL; exit 1; }
