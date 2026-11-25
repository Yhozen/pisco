#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/regress.sh [names...]: rebuilds the regression titles against the current runtime and runs them in Node.
# Names: hello threads cxx dll xcl sf (default: all). Logs: out/regress-<name>.{build,run}.log.
# Expected: every [PASS] line of hello / threads / cxx / dll; XCL's test title 46 of 50 in Node (the 4 checks that
# read pixels back need the WebGPU back end: tests/gfx/xcl.sh runs it in Chrome); Stockfish's bench 2,497,913 nodes.
# Environment: SF (Stockfish 19's Windows exe, default ~/untrusted/stockfish19/stockfish/stockfish-windows-x86-64-universal.exe)
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
SF=${SF:-$HOME/untrusted/stockfish19/stockfish/stockfish-windows-x86-64-universal.exe}
declare -A EXE=( [hello]=fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe [threads]=fixtures/threads/target/x86_64-pc-windows-msvc/release/threads.exe
  [cxx]=fixtures/cxx/eh.exe [dll]=fixtures/dll/game/title.exe [xcl]=out/xcltitle-game/xcl_testtitle.exe [sf]=$SF )
declare -A ARGS=( [xcl]=--hello [sf]=bench )
declare -A BASES=( [xcl]=xbox-series )   # the others are Windows x64 programs (the default base for 64-bit)
names=${*:-hello threads cxx dll xcl sf}; fail=0
for n in $names; do
  exe=${EXE[$n]}; o=out/regress-$n
  if [ ! -f "$exe" ]; then echo "$n: $exe missing, skipped"; continue; fi
  if ! BASE=${BASES[$n]:-windows-x64} tools/build-title.sh "$exe" "$o" > "$o.build.log" 2>&1; then echo "$n: build failed (see $o.build.log)"; fail=1; continue; fi
  timeout 1800 node web/run-node.mjs "$o/title.wasm" "$exe" ${ARGS[$n]:-} > "$o.run.log" 2>&1; code=$?
  pass=$(grep -c '^\[PASS\]' "$o.run.log"); bad=$(grep -c '^\[FAIL\]' "$o.run.log")
  case $n in
    sf) nodes=$(grep -o 'Nodes searched *: *[0-9]*' "$o.run.log" | grep -o '[0-9]*$'); nps=$(grep -o 'Nodes/second *: *[0-9]*' "$o.run.log" | grep -o '[0-9]*$')
        if [ "$nodes" = 2497913 ]; then echo "sf: $nodes nodes ($nps nodes/s)"; else echo "sf: FAILED ($nodes nodes, exit $code)"; fail=1; fi ;;
    xcl) echo "xcl: $pass passed, $bad failed in Node (exit $code; 4 pixel checks need WebGPU)"; [ "$pass" -ge 46 ] || fail=1 ;;
    *) echo "$n: $pass passed, $bad failed (exit $code)"; { [ "$bad" = 0 ] && [ "$code" = 0 ] && [ "$pass" -gt 0 ]; } || fail=1 ;;
  esac
done
exit $fail
