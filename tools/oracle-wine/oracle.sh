#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# oracle.sh: runs a Windows title under Wine without a screen, as the reference our own runs are compared with.
#   oracle.sh prefix <title> [installer.exe [installer arguments...]]
#       makes the title's Wine prefix (a 1024x768 virtual desktop, so a full-screen title stays a window
#       that keeps running when it loses focus) and runs the installer in it, if one is given
#   oracle.sh start <title> <program.exe, a Windows path in the prefix> [program arguments...]
#       starts the X server if needed and the program in the background; Wine's log goes to the run folder
#   oracle.sh play <title> <session.json>      plays a session script (see README.md); screenshots go to the run folder
#   oracle.sh stop <title>                     ends the title's Wine processes
#   oracle.sh reg <title> <reg arguments...>   `wine reg` in the title's prefix
#   oracle.sh dir <title>                      prints the run folder
# Environment: ORACLE_DISPLAY (default :77), ORACLE_RUN (the run folder, default out/oracle/<title>),
#   TRACE (Wine debug channels for `start`, e.g. +relay or +d3d9,+seh; default none),
#   and env.sh's ORACLE_HOME / WINE_HOME. A title's prefix is $ORACLE_HOME/prefix-<title>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); . "$here/env.sh"
cmd=${1:?usage: oracle.sh prefix|start|play|stop|reg|dir <title> ...}; title=${2:?title}; shift 2
export WINEPREFIX=${WINEPREFIX_OVERRIDE:-$ORACLE_HOME/prefix-$title}
export DISPLAY=${ORACLE_DISPLAY:-:77}
run=${ORACLE_RUN:-$root/out/oracle/$title}; mkdir -p "$run"
xserver() { # one X server per display, shared by every title
  [ -e "/tmp/.X11-unix/X${DISPLAY#:}" ] && return
  Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$run/xvfb.log" 2>&1 &
  for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X${DISPLAY#:}" ] && return; sleep 0.1; done
  echo "oracle.sh: Xvfb did not start on $DISPLAY" >&2; exit 1
}
case $cmd in
  prefix)
    xserver
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
      rm -rf "$WINEPREFIX"   # a prefix whose first boot was interrupted cannot load 32-bit programs
      wineboot -i > "$run/wineboot.log" 2>&1; wineserver -w
      wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f > /dev/null
      wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d 1024x768 /f > /dev/null
      wineserver -w
    fi
    if [ $# -gt 0 ]; then installer=$(realpath "$1"); shift; wine "$installer" "$@" > "$run/install.log" 2>&1; wineserver -w; fi
    echo "$WINEPREFIX" ;;
  start)
    xserver; exe=${1:?program}; shift
    unix=$(winepath -u "$exe" 2>/dev/null); [ -f "$unix" ] || { echo "oracle.sh: no such program: $exe" >&2; exit 1; }
    cd "$(dirname "$unix")"
    WINEDEBUG=${TRACE:--all} nohup wine "$(basename "$unix")" "$@" > "$run/wine.log" 2>&1 &
    echo $! > "$run/pid"; echo "started $(basename "$unix") on $DISPLAY (log: $run/wine.log)" ;;
  play)
    session=${1:?session.json}
    "$ORACLE_HOME/venv/bin/python" -I "$here/play.py" "$DISPLAY" "$session" "$run" ;;
  stop) wineserver -k 2>/dev/null || true; rm -f "$run/pid" ;;
  reg)  wine reg "$@" ;;
  dir)  echo "$run" ;;
  *) echo "oracle.sh: unknown command $cmd" >&2; exit 2 ;;
esac
