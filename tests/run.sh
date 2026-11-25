#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/run.sh [step...]: the one entry point. Builds the test titles from their sources, then runs the checks.
#   build    hello, threads, bench (cargo) and cxx, dll (llvm-mingw), where tests/regress.sh looks for them;
#            the 32-bit titles: hello again for i686, and fixtures/x87
#   layout   tests/layout.sh: the runtime's files are listed only by the bases, and every one is in some base
#   node     tests/regress.sh: every title in Node (XCL's title and Stockfish too when their files are there)
#   bench    the benchmark title's checksums against the same work built natively
#   win32    the 32-bit titles in Node: hello, and the x87 and crt32 titles against what they printed under Wine
#            (fixtures/*/expected.txt; tools/oracle-wine/oracle.sh run makes them again)
#   chrome   hello, threads and cxx again in headless Chrome (WebGPU on SwiftShader), and the 32-bit titles
#   nd       Narbacular Drop, when the installed game is there (ND: its folder; default: the oracle's prefix):
#            built with its recipe and run in strict mode. Its calls until it asks for Direct3D 9 must be the
#            ones it makes under Wine (recipes/narbacular-drop/traces/start.txt). Then the menu-to-level session
#            on the virtual clock with the null Direct3D 9 device: it must end with exit code 0 after every
#            checkpoint, each at the frame and with the Direct3D calls recorded in traces/menu-to-level.txt,
#            and a second run must give the same log (ND_RECORD=1 records the checkpoints again). Its draws go
#            through the fixed-function shaders to the null GPU back end, which must find every record consistent;
#            the shaders it made must pass naga
#   ffp      tests/gfx/d3d9-ffp.sh: Direct3D 9's fixed-function pipeline as WGSL (rt/windows-x86/d3d9_ffp.c), built
#            natively: the shaders of a representative set of state keys, validated with naga
#   sm       tests/gfx/d3d9-sm.sh: Direct3D 9's shader translator (rt/windows-x86/d3d9_sm.c), built natively: shaders
#            assembled to cover shader models 1.1 to 2.x, each one's WGSL validated with naga (SM_CAPTURED: a folder of
#            a title's captured bytecode, translated too)
#   d3dx     fixtures/d3dx: Microsoft's D3DX (the user's own d3dx9_35.dll: D3DX_DLL, default ~/untrusted/tag/cab/) lifted
#            with a 32-bit program that preprocesses and compiles HLSL through it; what it prints must be what it prints
#            under Wine with the same DLL (fixtures/d3dx/expected.txt). Skipped without the DLL
#   gdi      rt/windows-x86/font.c (gdi32's fonts) built natively by tests/gdi/fonts.c, which asks it what
#            tools/oracle-wine/gdiprobe.c asks Wine's GDI; with the probe's answers (out/oracle/narbacular-drop/
#            gdiprobe.txt) every measurement D3DX and the game read must be Wine's (tools/oracle-wine/gdi-compare.py),
#            and the glyph pixels' differences are printed
#   ndgfx    tests/gfx/nd.sh: Narbacular Drop's session in headless Chrome, drawn (WebGPU on SwiftShader): a PNG at
#            every checkpoint, and the still screens close to the oracle's screenshots when those are there (minutes)
#   tag      tests/gfx/tag.sh: Tag: The Power of Paint from the user's installer (TAG_SETUP), its session in headless
#            Chrome in strict mode: a PNG at every checkpoint, the menu close to the oracle's screenshot (an hour)
#   ndplay   tests/gfx/ndplay.sh: Narbacular Drop played (M5): the level1-portals session in Node (both portals, out of
#            the cage; its sound not silent and the same on a second run; its FMOD calls against Wine's +snoop trace
#            when the oracle's is there), system.opt written by Quit and read again by a second run (SW_SAVES), then
#            the same in Chrome: the session's screenshots, and a reload that finds system.opt in OPFS (minutes)
#   images   tests/images.sh: title images (M6): `pisco build` of hello32 and, from the user's installer (ND_SETUP; skipped
#            without it), Narbacular Drop, each twice with empty caches (the same manifest digest), their shared base
#            layer, a registry-only rebuild that lifts, compiles and links nothing, then `pisco run` of the menu-to-level
#            session in Node with the nd step's checkpoints, and the image's registry read by advapi32 (minutes). The
#            nd, ndgfx and ndplay steps keep building from the installed folder with tools/build-title.sh
#   doom     tests/gfx/doom.sh: Doom as a console title in Node and Chrome (minutes)
#   st       tests/gfx/st.sh: Microsoft's SimpleTriangleDesktop (Windows x64) in Chrome, and fixtures/msvc/eh.exe with
#            the real vcruntime140.dll in Node; skipped without the user-built sample (out/gdk-samples/local, from
#            tools/build-gdk-sample.sh) or the MSVC runtime DLLs (REDIST)
#   pbr      tests/gfx/pbr.sh: Microsoft's SimplePBR (Windows x64) in Chrome; skipped without the sample (built from
#            the SDKs in WINSDK by tools/build-gdk-sample-pbr.sh) or REDIST (minutes)
#   xcl      tests/gfx/xcl.sh: XCL's console test title in Chrome, when it has been built into out/xcltitle-game
#   audio    tests/audio/run.sh: the XMA2 / Opus decoders (builds FFmpeg's the first time)
# Default: build layout node bench win32 ffp sm gdi. Needs tools/setup-toolchain.sh to have run. Exit status 1 if a step fails.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"; mkdir -p out
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CXX=$MINGW/bin/x86_64-w64-mingw32-clang++
R=target/x86_64-pc-windows-msvc/release
fail=0
step() { # name, command...: one line, the log's tail on failure
  local name=$1 log=out/test-$1.log; shift
  if "$@" > "$log" 2>&1; then echo "ok    $name: $(grep -v '^ *$' "$log" | tail -${LINES_SHOWN:-1} | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
  else echo "FAIL  $name (see $log)"; tail -6 "$log" | sed 's/^/        /'; fail=1; fi
}
build() {
  cargo build -q --release -p pisco-lifter -p pisco-shaders -p pisco || return 1
  for t in hello threads bench bench-native; do (cd "fixtures/$t" && cargo build -q --release) || return 1; done
  # one C++ runtime inside each image (-static): nothing but the title's own DLL is shipped beside it
  "$CXX" -O2 -static fixtures/cxx/eh.cpp -o fixtures/cxx/eh.exe || return 1
  mkdir -p fixtures/dll/game
  "$CXX" -O2 -static -shared fixtures/dll/lib.cpp -o fixtures/dll/game/gamelib.dll -Wl,--out-implib,fixtures/dll/game/libgamelib.a || return 1
  "$CXX" -O2 -static fixtures/dll/main.cpp -o fixtures/dll/game/title.exe -Lfixtures/dll/game -lgamelib || return 1
  (cd fixtures/hello && cargo build -q --release --target i686-pc-windows-msvc) || return 1
  # the exe and its DLL again as 32-bit programs (several 32-bit images lifted together)
  local CXX32=$MINGW/bin/i686-w64-mingw32-clang++; mkdir -p fixtures/dll/game32
  "$CXX32" -O2 -static -shared fixtures/dll/lib.cpp -o fixtures/dll/game32/gamelib.dll -Wl,--out-implib,fixtures/dll/game32/libgamelib.a || return 1
  "$CXX32" -O2 -static fixtures/dll/main.cpp -o fixtures/dll/game32/title.exe -Lfixtures/dll/game32 -lgamelib || return 1
  fixtures/x87/build.sh out/x87 > /dev/null || return 1
  fixtures/crt32/build.sh out/crt32 > /dev/null || return 1
  # the MSVC-ABI exceptions title, when clang-cl is there (tests/gfx/st.sh runs it)
  local msvc=""; if command -v clang-cl > /dev/null && command -v lld-link > /dev/null; then fixtures/msvc/build.sh || return 1; msvc=", msvc's eh"; fi
  echo "built hello threads bench cxx dll$msvc, and hello, dll, x87 and crt32 as 32-bit programs"
}
win32() {
  local ok=0 exe=fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe out pass
  tools/build-title.sh "$exe" out/hello32 > out/hello32.build.log 2>&1 || { echo "hello32: build failed (out/hello32.build.log)"; return 1; }
  out=$(SW_STRICT=1 node web/run-node.mjs out/hello32/title.wasm "$exe" 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
  echo "hello32: $pass passed, $(grep -c '^\[FAIL\]' <<<"$out") failed"; [ "$pass" = 7 ] || { tail -3 <<<"$out"; ok=1; }
  tools/build-title.sh out/x87/x87.exe out/x87/wasm > out/x87.build.log 2>&1 || { echo "x87: build failed (out/x87.build.log)"; return 1; }
  SW_STRICT=1 node web/run-node.mjs out/x87/wasm/title.wasm out/x87/x87.exe > out/x87/got.txt 2>&1
  if cmp -s out/x87/got.txt fixtures/x87/expected.txt; then echo "x87: $(wc -l < fixtures/x87/expected.txt) lines equal what it prints under Wine"
  else echo "x87: differs from what it prints under Wine (< ours, > Wine):"; diff out/x87/got.txt fixtures/x87/expected.txt | head -12; ok=1; fi
  # the 32-bit exe and its DLL: all but the last check (a C++ exception thrown in the DLL: mingw's DWARF unwinding
  # for 32-bit code is not dispatched yet; MSVC's, which Windows games use, is T1's)
  tools/build-title.sh fixtures/dll/game32/title.exe out/dll32 > out/dll32.build.log 2>&1 || { echo "dll32: build failed (out/dll32.build.log)"; return 1; }
  out=$(SW_STRICT=1 timeout 120 node web/run-node.mjs out/dll32/title.wasm fixtures/dll/game32/title.exe 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
  echo "dll32: $pass passed, the exception across the DLL not yet (32-bit DWARF unwinding)"; [ "$pass" = 6 ] || { tail -3 <<<"$out"; ok=1; }
  tools/build-title.sh out/crt32/crt32.exe out/crt32/wasm > out/crt32.build.log 2>&1 || { echo "crt32: build failed (out/crt32.build.log)"; return 1; }
  SW_STRICT=1 node web/run-node.mjs out/crt32/wasm/title.wasm out/crt32/crt32.exe > out/crt32/got.txt 2>&1
  if cmp -s out/crt32/got.txt fixtures/crt32/expected.txt; then echo "crt32: $(wc -l < fixtures/crt32/expected.txt) lines (msvcrt.dll maths and printf, SList) equal what it prints under Wine"
  else echo "crt32: differs from what it prints under Wine (< ours, > Wine):"; diff out/crt32/got.txt fixtures/crt32/expected.txt | head -12; ok=1; fi
  return $ok
}
bench() {
  tools/build-title.sh fixtures/bench/$R/bench.exe out/bench > out/bench.build.log 2>&1 || { echo "build failed (out/bench.build.log)"; return 1; }
  local want got; want=$(fixtures/bench-native/target/release/bench-native | grep -o 'checksum [0-9a-f]*')
  got=$(node web/run-node.mjs out/bench/title.wasm fixtures/bench/$R/bench.exe 2>&1 | grep -o 'checksum [0-9a-f]*')
  [ -n "$want" ] && [ "$want" = "$got" ] && echo "$(wc -l <<<"$want") checksums equal the native build's" || { echo "checksums differ from the native build's"; return 1; }
}
chrome() {
  local n exe out pass bad ok=0
  for n in hello threads cxx; do
    case $n in cxx) exe=fixtures/cxx/eh.exe ;; *) exe=fixtures/$n/$R/$n.exe ;; esac
    [ -f "out/regress-$n/title.wasm" ] || tools/build-title.sh "$exe" "out/regress-$n" > "out/regress-$n.build.log" 2>&1 || { echo "$n: build failed"; ok=1; continue; }
    out=$(TIMEOUT=300 node tools/run-chrome.mjs "out/regress-$n/title.wasm" "$exe" 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out"); bad=$(grep -c '^\[FAIL\]' <<<"$out")
    echo "$n: $pass passed, $bad failed"; { [ "$pass" -gt 0 ] && [ "$bad" = 0 ]; } || ok=1
  done
  # the 32-bit titles, when the win32 step has built them
  if [ -f out/hello32/title.wasm ]; then
    out=$(TIMEOUT=300 node tools/run-chrome.mjs out/hello32/title.wasm fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
    echo "hello32: $pass passed, $(grep -c '^\[FAIL\]' <<<"$out") failed"; [ "$pass" = 7 ] || ok=1
  fi
  if [ -f out/x87/wasm/title.wasm ]; then
    TIMEOUT=300 node tools/run-chrome.mjs out/x87/wasm/title.wasm out/x87/x87.exe 2>&1 | grep -v -e '^$' -e '^gpu: ' > out/x87/got-chrome.txt   # the page's own first line names the GPU
    if cmp -s out/x87/got-chrome.txt fixtures/x87/expected.txt; then echo "x87: equal to what it prints under Wine"; else echo "x87: differs from what it prints under Wine"; ok=1; fi
  fi
  return $ok
}
nd() {
  RECIPE=recipes/narbacular-drop/title.toml SPLIT=16 OPT=-O1 tools/build-title.sh "$ND/Narbacular Drop.exe" out/nd > out/nd.build.log 2>&1 || { echo "build failed (out/nd.build.log)"; return 1; }
  # start-up, every call traced: a session of one empty step ends the run when the first frame has been presented
  SW_STRICT=1 SW_TRACE=2 SW_SESSION_STEPS='wait 0' timeout 300 node web/run-node.mjs out/nd/title.wasm "$ND/Narbacular Drop.exe" > out/nd.run.log 2>&1
  grep -q 'Direct3DCreate9' out/nd.run.log || { echo "did not reach Direct3DCreate9:"; tail -3 out/nd.run.log; return 1; }
  python3 -I tools/oracle-wine/trace-diff.py recipes/narbacular-drop/traces/start.txt out/nd.run.log --until Direct3DCreate9 || return 1
  # the session: menu, story, tutorial, the first level
  local want=recipes/narbacular-drop/traces/menu-to-level.txt code n
  nd_session > out/nd.session.log 2>&1; code=$?
  [ $code = 0 ] || { echo "the session ended with exit code $code:"; tail -3 out/nd.session.log | cut -c1-300; return 1; }
  grep -E '^pisco: (checkpoint |session done)' out/nd.session.log | sed 's/^pisco: //' > out/nd.session.checkpoints.txt
  grep -q '^session done' out/nd.session.checkpoints.txt || { echo "the session did not reach its end:"; tail -3 out/nd.session.log | cut -c1-300; return 1; }
  [ -z "${ND_RECORD:-}" ] || { grep '^#' $want > out/nd.session.head.txt; cat out/nd.session.head.txt out/nd.session.checkpoints.txt > $want; }
  diff <(grep -v '^#' $want) out/nd.session.checkpoints.txt > out/nd.session.diff || { echo "the session's checkpoints are not the recorded ones (< $want, > this run):"; head -6 out/nd.session.diff; return 1; }
  nd_session > out/nd.session.2.log 2>&1
  cmp -s out/nd.session.log out/nd.session.2.log || { echo "two runs of the session differ: out/nd.session.log, out/nd.session.2.log"; return 1; }
  # what reached the GPU ring: counted and checked by the null back end; the shaders this run made, by naga
  grep -q '^pisco: gpu (null back end): .* 0 of the records were inconsistent$' out/nd.session.log || { echo "the null GPU back end did not find the session's records consistent:"; grep '^pisco: gpu' out/nd.session.log | tail -3 | cut -c1-300; return 1; }
  local shaders="naga is not installed: its shaders were not validated"
  if command -v naga > /dev/null; then shaders=$(tests/gfx/d3d9-ffp.sh out/nd.session.log 2>&1) || { echo "the session's shaders: $shaders"; return 1; }; shaders="its $(wc -l < out/ffp/run-keys.txt) shaders pass naga"; fi
  n=$(grep -c '^checkpoint ' out/nd.session.checkpoints.txt)
  echo "session: $n checkpoints as recorded, $(sed -n 's/^session done: .*, \([0-9]*\) frames$/\1/p' out/nd.session.checkpoints.txt) frames, two runs give the same log"
  echo "$(sed -n 's/^pisco: gpu (null back end): \(.*\); 0 of the records were inconsistent$/\1/p' out/nd.session.log), all consistent, and $shaders"
}
d3dx() {
  fixtures/d3dx/build.sh out/d3dx "$D3DX_DLL" > /dev/null || return 1
  tools/build-title.sh out/d3dx/d3dx.exe out/d3dx/wasm > out/d3dx.build.log 2>&1 || { echo "build failed (out/d3dx.build.log)"; return 1; }
  SW_STRICT=1 timeout 600 node web/run-node.mjs out/d3dx/wasm/title.wasm out/d3dx/d3dx.exe > out/d3dx/got.txt 2>&1
  if cmp -s out/d3dx/got.txt fixtures/d3dx/expected.txt; then echo "the preprocessed text and 3 bytecodes equal what Microsoft's D3DX makes under Wine"
  else echo "differs from Wine's (< ours, > Wine):"; diff out/d3dx/got.txt fixtures/d3dx/expected.txt | head -8; return 1; fi
}
gdi() {
  mkdir -p out/gdi
  cc -O1 -DFONT_NO_EMBED -Irt/windows-x86 tests/gdi/fonts.c rt/windows-x86/font.c -lm -o out/gdi/fonts || return 1
  out/gdi/fonts rt/windows-x86/fonts > out/gdi/fonts.txt || return 1
  local probe=out/oracle/narbacular-drop/gdiprobe.txt
  [ -f $probe ] || { echo "$(grep -c '^font ' out/gdi/fonts.txt) fonts measured; no answers of Wine's in $probe to compare with (tools/oracle-wine/gdiprobe.c)"; return 0; }
  python3 -I tools/oracle-wine/gdi-compare.py $probe out/gdi/fonts.txt
}
nd_session() { SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 SW_SESSION=recipes/narbacular-drop/sessions/menu-to-level.json timeout 600 node web/run-node.mjs out/nd/title.wasm "$ND/Narbacular Drop.exe"; }
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
D3DX_DLL=${D3DX_DLL:-$HOME/untrusted/tag/cab/d3dx9_35.dll}
REDIST=${REDIST:-$HOME/untrusted/vcredist/x/Contents/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT}; export REDIST
WINSDK=${WINSDK:-$HOME/winsdk}
for s in ${*:-build layout node bench win32 ffp sm gdi}; do case $s in
  build)  step build build ;;
  layout) step layout tests/layout.sh ;;
  node)   LINES_SHOWN=8 step node tests/regress.sh ;;
  bench)  step bench bench ;;
  win32)  LINES_SHOWN=4 step win32 win32 ;;
  chrome) LINES_SHOWN=5 step chrome chrome ;;
  nd)     if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=3 step nd nd; else echo "skip  nd: the game is not installed (ND=$ND)"; fi ;;
  ffp)    if command -v naga > /dev/null; then step ffp tests/gfx/d3d9-ffp.sh; else echo "skip  ffp: naga is not installed (tools/setup-toolchain.sh naga)"; fi ;;
  sm)     if command -v naga > /dev/null; then step sm tests/gfx/d3d9-sm.sh ${SM_CAPTURED:+$SM_CAPTURED/*}; else echo "skip  sm: naga is not installed (tools/setup-toolchain.sh naga)"; fi ;;
  gdi)    LINES_SHOWN=5 step gdi gdi ;;
  d3dx)   if [ -f "$D3DX_DLL" ]; then step d3dx d3dx; else echo "skip  d3dx: no d3dx9_35.dll in D3DX_DLL=$D3DX_DLL (Tag's installer has one)"; fi ;;
  ndgfx)  if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=16 ND=$ND step ndgfx tests/gfx/nd.sh; else echo "skip  ndgfx: the game is not installed (ND=$ND)"; fi ;;
  ndplay) if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=16 ND=$ND step ndplay tests/gfx/ndplay.sh; else echo "skip  ndplay: the game is not installed (ND=$ND)"; fi ;;
  tag)    if [ -f "${TAG_SETUP:-$HOME/games/Tag_setup.exe}" ]; then LINES_SHOWN=10 step tag tests/gfx/tag.sh; else echo "skip  tag: no installer at TAG_SETUP=${TAG_SETUP:-$HOME/games/Tag_setup.exe}"; fi ;;
  doom)   LINES_SHOWN=4 step doom tests/gfx/doom.sh ;;
  st)     if [ ! -f "$REDIST/vcruntime140.dll" ]; then echo "skip  st: no MSVC runtime DLLs in REDIST=$REDIST"
          elif [ ! -f out/gdk-samples/local/SimpleTriangleDesktop.exe ]; then echo "skip  st: out/gdk-samples/local is not built (tools/build-gdk-sample.sh)"
          elif [ ! -f fixtures/msvc/eh.exe ]; then echo "skip  st: fixtures/msvc/eh.exe is not built (the build step, with clang-cl)"
          else LINES_SHOWN=4 step st tests/gfx/st.sh; fi ;;
  pbr)    if [ ! -f "$REDIST/vcruntime140.dll" ]; then echo "skip  pbr: no MSVC runtime DLLs in REDIST=$REDIST"
          elif [ ! -f out/gdk-samples/pbr-local/SimplePBR.exe ] && [ ! -d "$WINSDK/xwin" ]; then echo "skip  pbr: the sample is not built and there are no SDKs to build it in WINSDK=$WINSDK"
          else LINES_SHOWN=3 step pbr tests/gfx/pbr.sh; fi ;;
  images) LINES_SHOWN=9 step images tests/images.sh ;;
  audio)  step audio tests/audio/run.sh ;;
  xcl)    if [ -f out/xcltitle-game/xcl_testtitle.exe ]; then LINES_SHOWN=3 step xcl tests/gfx/xcl.sh; else echo "skip  xcl: out/xcltitle-game/xcl_testtitle.exe is not built"; fi ;;
  *) echo "unknown step: $s"; fail=1 ;;
esac; done
exit $fail
