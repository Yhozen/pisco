#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# oracle.sh: runs a Windows title under Wine without a screen, as the reference our own runs are compared with.
#   oracle.sh prefix <title> [installer.exe [installer arguments...]]
#       makes the title's Wine prefix (a 1024x768 virtual desktop, so a full-screen title stays a window
#       that keeps running when it loses focus) and runs the installer in it, if one is given
#   oracle.sh start <title> <program.exe, a Windows path in the prefix> [program arguments...]
#       starts the X server if needed and the program in the background; Wine's log goes to the run folder
#   oracle.sh play <title> <session.json>      plays a session script (see README.md); screenshots go to the run folder
#   oracle.sh run <title> <program.exe, a path on this machine> [arguments...]
#       runs a console program to its end and prints what it printed (Wine's own messages go to the run folder)
#   oracle.sh stop <title>                     ends the title's Wine processes (and the recording of its sound)
#   oracle.sh sound start|stop                  the sound device: a PipeWire daemon with a null sink (pipewire.conf),
#       started by `start` and `run` when needed. While a title runs, what it plays is recorded to the run folder's
#       wine-audio.wav (48 kHz stereo, from the sink's monitor)
#   oracle.sh reg <title> <reg arguments...>   `wine reg` in the title's prefix
#   oracle.sh dir <title>                      prints the run folder
# Environment: ORACLE_DISPLAY (default :77), ORACLE_RUN (the run folder, default out/oracle/<title>),
#   TRACE (Wine debug channels for `start`, e.g. +relay or +d3d9,+seh; default none: `warn+heap` is always on, so that
#   new heap blocks are filled with 0x55 as our runtime fills them with 0xCD), ORACLE_SOUND (0: no sound device),
#   and env.sh's ORACLE_HOME / WINE_HOME. A title's prefix is $ORACLE_HOME/prefix-<title>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); . "$here/env.sh"
cmd=${1:?usage: oracle.sh prefix|start|run|play|stop|reg|dir|sound <title> ...}
if [ "$cmd" = sound ]; then title=-; else title=${2:?title}; shift; fi; shift
export WINEPREFIX=${WINEPREFIX_OVERRIDE:-$ORACLE_HOME/prefix-$title}
export DISPLAY=${ORACLE_DISPLAY:-:77}
run=${ORACLE_RUN:-$root/out/oracle/$title}; [ "$title" = - ] || mkdir -p "$run"
xserver() { # one X server per display, shared by every title
  [ -e "/tmp/.X11-unix/X${DISPLAY#:}" ] && return
  Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$run/xvfb.log" 2>&1 &
  for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X${DISPLAY#:}" ] && return; sleep 0.1; done
  echo "oracle.sh: Xvfb did not start on $DISPLAY" >&2; exit 1
}
sound() { # start: the PipeWire daemon and wireplumber, once for every title; stop: both
  local d=$ORACLE_SOUND_DIR; mkdir -p "$d"; chmod 700 "$d"
  local env="XDG_RUNTIME_DIR=$d PIPEWIRE_RUNTIME_DIR=$d XDG_STATE_HOME=$d/state XDG_CONFIG_HOME=$d/config DBUS_SESSION_BUS_ADDRESS=disabled:"
  if [ "$1" = stop ]; then
    for f in wireplumber pipewire; do [ -f "$d/$f.pid" ] && kill "$(cat "$d/$f.pid")" 2>/dev/null; rm -f "$d/$f.pid"; done; return
  fi
  [ -f "$d/pipewire.pid" ] && kill -0 "$(cat "$d/pipewire.pid")" 2>/dev/null && [ -S "$d/pulse/native" ] && return
  rm -rf "$d/pulse" "$d"/pipewire-0*
  env $env pipewire -c "$here/pipewire.conf" > "$d/pipewire.log" 2>&1 & echo $! > "$d/pipewire.pid"
  for _ in $(seq 50); do [ -S "$d/pulse/native" ] && break; sleep 0.1; done
  env $env wireplumber > "$d/wireplumber.log" 2>&1 & echo $! > "$d/wireplumber.pid"
  sleep 1   # wireplumber finds the sink and makes it the default
  [ -S "$d/pulse/native" ] || { echo "oracle.sh: the sound device did not start ($d/pipewire.log)" >&2; exit 1; }
}
record() { # what the title plays, until `stop`
  [ "$ORACLE_SOUND" = 0 ] && return
  ffmpeg -hide_banner -loglevel error -y -f pulse -i sw-null.monitor -ac 2 -ar 48000 "$run/wine-audio.wav" < /dev/null > "$run/record.log" 2>&1 &
  echo $! > "$run/record.pid"
}
case $cmd in
  sound) sound "${1:?start or stop}"; exit 0 ;;
esac
if [ "$ORACLE_SOUND" != 0 ] && { [ "$cmd" = start ] || [ "$cmd" = run ]; }; then sound start; fi
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
    record
    WINEDEBUG=${TRACE:--all},warn+heap nohup wine "$(basename "$unix")" "$@" > "$run/wine.log" 2>&1 &
    echo $! > "$run/pid"; echo "started $(basename "$unix") on $DISPLAY (log: $run/wine.log$([ "$ORACLE_SOUND" = 0 ] || echo ", sound: $run/wine-audio.wav"))" ;;
  run)
    xserver; exe=$(realpath "${1:?program}"); shift
    [ -f "$WINEPREFIX/system.reg" ] || "$0" prefix "$title" > /dev/null
    WINEDEBUG=-all wine "$exe" "$@" 2> "$run/run.log" ;;
  play)
    session=${1:?session.json}
    "$ORACLE_HOME/venv/bin/python" -I "$here/play.py" "$DISPLAY" "$session" "$run" ;;
  stop) wineserver -k 2>/dev/null || true; rm -f "$run/pid"
    if [ -f "$run/record.pid" ]; then p=$(cat "$run/record.pid"); kill -INT "$p" 2>/dev/null; for _ in $(seq 50); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; rm -f "$run/record.pid"; fi ;;
  reg)  wine reg "$@" ;;
  dir)  echo "$run" ;;
  *) echo "oracle.sh: unknown command $cmd" >&2; exit 2 ;;
esac
