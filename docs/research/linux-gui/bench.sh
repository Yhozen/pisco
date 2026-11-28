#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# bench.sh: GUI work on the running desktop (DISPLAY :0): an X client's start, and a terminal's text through X
export DISPLAY=:0
# milliseconds since boot (/proc/uptime: hundredths of a second; busybox date has no %N)
ms() { read up _ < /proc/uptime; echo $(( ${up%.*}${up#*.}0 )); }
for i in 1 2 3 4; do
  s=$(ms); xterm -geometry 80x24+300+300 -e true; echo "BENCH xterm-start $(( $(ms) - s )) ms"
done
for i in 1 2 3; do
  s=$(ms); xterm -geometry 100x40+100+100 -e sh -c 'seq 1 20000'; echo "BENCH xterm-scroll-20000 $(( $(ms) - s )) ms"
done
s=$(ms); i=0; while [ $i -lt 200 ]; do xsetroot -solid "#$(printf %02x $i)4060"; i=$((i+1)); done; echo "BENCH xsetroot-200 $(( $(ms) - s )) ms"
echo BENCH-DONE
