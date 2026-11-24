#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/run.sh [step...]: the one entry point. Builds the test titles from their sources, then runs the checks.
#   build    hello, threads, bench (cargo) and cxx, dll (llvm-mingw), where tests/regress.sh looks for them
#   node     tests/regress.sh: every title in Node (XCL's title and Stockfish too when their files are there)
#   bench    the benchmark title's checksums against the same work built natively
#   chrome   hello, threads and cxx again in headless Chrome (WebGPU on SwiftShader)
#   doom     tests/gfx/doom.sh: Doom as a console title in Node and Chrome (minutes)
#   xcl      tests/gfx/xcl.sh: XCL's console test title in Chrome, when it has been built into out/xcltitle-game
#   audio    tests/audio/run.sh: the XMA2 / Opus decoders (builds FFmpeg's the first time)
# Default: build node bench. Needs tools/setup-toolchain.sh to have run. Exit status 1 if a step fails.
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
  cargo build -q --release -p lifter -p shaders || return 1
  for t in hello threads bench bench-native; do (cd "fixtures/$t" && cargo build -q --release) || return 1; done
  # one C++ runtime inside each image (-static): nothing but the title's own DLL is shipped beside it
  "$CXX" -O2 -static fixtures/cxx/eh.cpp -o fixtures/cxx/eh.exe || return 1
  mkdir -p fixtures/dll/game
  "$CXX" -O2 -static -shared fixtures/dll/lib.cpp -o fixtures/dll/game/gamelib.dll -Wl,--out-implib,fixtures/dll/game/libgamelib.a || return 1
  "$CXX" -O2 -static fixtures/dll/main.cpp -o fixtures/dll/game/title.exe -Lfixtures/dll/game -lgamelib || return 1
  echo "built hello threads bench cxx dll"
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
  done; return $ok
}
for s in ${*:-build node bench}; do case $s in
  build)  step build build ;;
  node)   LINES_SHOWN=8 step node tests/regress.sh ;;
  bench)  step bench bench ;;
  chrome) LINES_SHOWN=3 step chrome chrome ;;
  doom)   LINES_SHOWN=4 step doom tests/gfx/doom.sh ;;
  audio)  step audio tests/audio/run.sh ;;
  xcl)    if [ -f out/xcltitle-game/xcl_testtitle.exe ]; then LINES_SHOWN=3 step xcl tests/gfx/xcl.sh; else echo "skip  xcl: out/xcltitle-game/xcl_testtitle.exe is not built"; fi ;;
  *) echo "unknown step: $s"; fail=1 ;;
esac; done
exit $fail
