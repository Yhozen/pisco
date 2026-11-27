#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/mohaa.sh: Medal of Honor: Allied Assault's demo (Windows x86, OpenGL) from the user's download in Node,
# strict, on the virtual clock (docs/mohaa.md). Builds out/img-mohaa from MOHAA_ZIP (default ~/games/mohaaspdemo.zip)
# and plays the `menu` session: the logos, then the menu. Passes when the session ends with exit code 0, the null
# back end finds every record consistent and nothing was left out, and the engine's console log (main\qconsole.log)
# is Wine's but for what describes the machine (tools/oracle-wine/mohaa-console.py), when the oracle's log is there.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
zip=${MOHAA_ZIP:-$HOME/games/mohaaspdemo.zip}; img=out/img-mohaa; oracle=${ORACLE:-out/oracle/mohaa/menu1}
target/release/pisco build recipes/mohaa --source "$zip" --out $img > out/mohaa.build.log 2>&1 || { echo "the build failed:"; tail -3 out/mohaa.build.log; exit 1; }
saves=out/mohaa-saves; rm -rf $saves
start=$(date +%s)
SW_STRICT=1 SW_CLOCK=16667 timeout 1200 target/release/pisco run $img --session recipes/mohaa/sessions/menu.json --saves $saves -- +set logfile 2 > out/mohaa.menu.log 2>&1; code=$?
[ $code = 0 ] || { echo "the menu session ended with exit code $code:"; grep -v '^pisco:   \|vfs: read' out/mohaa.menu.log | tail -4 | cut -c1-300; exit 1; }
grep -q 'checkpoint menu frame' out/mohaa.menu.log || { echo "the session did not reach its checkpoint"; exit 1; }
gpu=$(grep '^pisco: gpu (null back end)' out/mohaa.menu.log)
grep -q ' 0 of the records were inconsistent' <<<"$gpu" || { echo "the null back end found records it could not use: $gpu"; exit 1; }
echo "menu: $(grep -o 'session done: .*' out/mohaa.menu.log), $(( $(date +%s) - start )) s; $(sed 's/.*: \([0-9]* draws[^;]*\).*/\1/' <<<"$gpu")"
log=$saves/@game/main/qconsole.log
[ -f "$log" ] || { echo "the engine wrote no main\\qconsole.log"; exit 1; }
if [ -f "$oracle/qconsole.log" ]; then python3 -I tools/oracle-wine/mohaa-console.py "$log" "$oracle/qconsole.log" || exit 1
else echo "no console log of the oracle in $oracle: not compared"; fi
