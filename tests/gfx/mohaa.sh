#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/gfx/mohaa.sh: Medal of Honor: Allied Assault's demo (Windows x86, OpenGL) from the user's download in Node,
# strict, on the virtual clock (docs/mohaa.md). Builds out/img-mohaa from MOHAA_ZIP (default ~/games/mohaaspdemo.zip)
# and plays the `menu` session (the logos, then the menu), the `level` session (through the menu into the level),
# with sound (Miles on DirectSound), and the `play` session (the level, then walking, shooting, the weapons, a quick
# save with F5, the in-game menu). Passes when each session ends with exit code 0, the null back end finds every
# record consistent, `play` wrote the quick save, and, when the oracle's runs are there (out/oracle/mohaa/<session>1:
# tools/oracle-wine/mohaa-play.sh), the engine's console log (main\qconsole.log) is Wine's but for what describes the
# machine (tools/oracle-wine/mohaa-console.py) and the level session's sound is within 1 dB of Wine's recording where
# both sound.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
zip=${MOHAA_ZIP:-$HOME/games/mohaaspdemo.zip}; img=out/img-mohaa; oracle=${ORACLE:-out/oracle/mohaa/menu1}
target/release/pisco build recipes/mohaa --source "$zip" --out $img --cache out/pc-mohaa > out/mohaa.build.log 2>&1 || { echo "the build failed:"; tail -3 out/mohaa.build.log; exit 1; }
for session in menu level play; do
  saves=out/mohaa-saves-$session; rm -rf $saves; oracle=${ORACLE:-out/oracle/mohaa}/${session}1
  start=$(date +%s)
  SW_STRICT=1 SW_CLOCK=16667 SW_AUDIO_WAV=$root/out/mohaa.$session.wav timeout 2400 target/release/pisco run $img --session recipes/mohaa/sessions/$session.json --saves $saves -- +set logfile 2 > out/mohaa.$session.log 2>&1; code=$?
  [ $code = 0 ] || { echo "the $session session ended with exit code $code:"; grep -v '^pisco:   \|vfs: read' out/mohaa.$session.log | tail -4 | cut -c1-300; exit 1; }
  gpu=$(grep '^pisco: gpu (null back end)' out/mohaa.$session.log)
  grep -q ' 0 of the records were inconsistent' <<<"$gpu" || { echo "$session: the null back end found records it could not use: $gpu"; exit 1; }
  echo "$session: $(grep -o 'session done: .*' out/mohaa.$session.log), $(( $(date +%s) - start )) s; $(sed 's/.*: \([0-9]* draws[^;]*\).*/\1/' <<<"$gpu")"
  log=$saves/@game/main/qconsole.log
  [ $session != play ] || [ -s $saves/@game/main/save/unnamedsoldier/quick.sav ] || { echo "play: F5 wrote no quick save"; exit 1; }
  [ -f "$log" ] || { echo "$session: the engine wrote no main\\qconsole.log"; exit 1; }
  if [ -f "$oracle/qconsole.log" ]; then c=$(python3 -I tools/oracle-wine/mohaa-console.py "$log" "$oracle/qconsole.log") || { echo "$session: the console log is not Wine's:"; echo "$c" | tail -8; exit 1; }; echo "  console log: $(tail -1 <<<"$c")"
  else echo "  no console log of the oracle in $oracle: not compared"; fi
done
if [ -f out/oracle/mohaa/level1/wine-audio.wav ]; then
  a=$(python3 -I tools/oracle-wine/audio-compare.py out/mohaa.level.wav out/oracle/mohaa/level1/wine-audio.wav) || { echo "$a"; exit 1; }
  echo "  sound: $(tail -1 <<<"$a")"
  awk '/where both sound/ { for (i = 1; i <= NF; i++) if ($i == "dB" && $(i - 1) ~ /^[-+]?[0-9.]+$/) d = $(i - 1); if (d < 0) d = -d; exit !(d <= 1) }' <<<"$a" || { echo "the level's sound is not within 1 dB of Wine's"; exit 1; }
fi
