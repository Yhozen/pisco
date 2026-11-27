#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# mohaa-play.sh <session.json> <run name>: Medal of Honor: Allied Assault's demo under Wine (docs/mohaa.md), from the
# state of the first start after Configure.exe: the markers it leaves (sm.001, sm.002: the engine deletes sm.002 once
# it has applied main\newconfig.cfg) and none of what the engine leaves behind (sm.000, its configs, saves and console
# log, HKCU\Software\2015\MOHAA), so that each run starts as ours does (recipes/mohaa's [files]). The session's screenshots, Wine's log and the engine's main\qconsole.log go to out/oracle/mohaa/<run>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); . "$here/env.sh"
session=${1:?session.json}; name=${2:?run name}
game="$ORACLE_HOME/prefix-mohaa/drive_c/Program Files (x86)/EA GAMES/MOHAADemo"
export ORACLE_RUN=$root/out/oracle/mohaa/$name; rm -rf "$ORACLE_RUN"; mkdir -p "$ORACLE_RUN"
"$here/oracle.sh" stop mohaa > /dev/null 2>&1 || true
rm -rf "$game/sm.000" "$game/main/qconsole.log" "$game/main/configs" "$game/main/save"; : > "$game/sm.001"; : > "$game/sm.002"
"$here/oracle.sh" reg mohaa delete 'HKCU\Software\2015\MOHAA' /f > /dev/null 2>&1 || true
# Mesa's extension list overflows the engine's buffer: the year keeps it to what 2001 had
MESA_EXTENSION_MAX_YEAR=2001 "$here/oracle.sh" start mohaa 'C:\Program Files (x86)\EA GAMES\MOHAADemo\MOHAADemo.exe' +set logfile 2 > /dev/null
"$here/oracle.sh" play mohaa "$session"
"$here/oracle.sh" stop mohaa > /dev/null 2>&1 || true
cp "$game/main/qconsole.log" "$ORACLE_RUN/" 2>/dev/null || echo "mohaa-play.sh: the engine wrote no console log" >&2
rm -f "$game/sm.000"
