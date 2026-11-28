#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/run.sh snapshot: a machine's state as one file (docs/linux.md#snapshots). The test kernel (fixtures/microvm-hello)
# is snapshotted in ring 3 and goes on from the file in another process, to its end, as an unbroken run prints it. Then
# Linux: a shell writes a file in its tmpfs, sets a variable and starts a counter in the background; a snapshot ends
# the run; a second process goes on from the AgentFS file, where the file, the variable and the counter (still
# counting) are all there. Skipped without the AgentFS SDK (tools/setup-agentfs.sh) or the Linux image.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
d=out/snapshot; mkdir -p $d
[ -f out/npm/node_modules/agentfs-sdk/dist/index_node.js ] || { echo "no AgentFS SDK in out/npm (tools/setup-agentfs.sh): nothing checked"; exit 0; }
fail() { echo "$*"; exit 1; }

# ---- the test kernel: snapshotted when it has printed its timer's line, then gone on from the file ----
h=out/microvm-hello
if [ -x "$h/hello" ] || fixtures/microvm-hello/build.sh $h > $d/hello-build.log 2>&1; then
  tools/build-app.sh $h/hello $h/wasm > $d/hello-lift.log 2>&1 || fail "the test kernel did not build ($d/hello-lift.log)"
  rm -f $d/hello.db*
  SW_STATE=$d/hello.db SW_SNAPSHOT_ON="timer: 3 ticks" timeout 120 node web/run-node.mjs $h/wasm/app.wasm $h/hello > $d/hello1.txt 2>&1
  grep -q '^pisco: snapshot .* chunks of 64 KiB' $d/hello1.txt || fail "the test kernel took no snapshot ($d/hello1.txt)"
  SW_STATE=$d/hello.db timeout 120 node web/run-node.mjs $h/wasm/app.wasm $h/hello > $d/hello2.txt 2> $d/hello2.err
  grep -q '^pisco: snapshot .* chunks read' $d/hello2.err || fail "the second run did not go on from the snapshot ($d/hello2.err)"
  # what the second run printed after the screen it was given: the rest of the test kernel's lines
  cmp -s <(tr -d '\r' < $d/hello2.txt | grep -v '^pisco: ') fixtures/microvm-hello/expected.txt || { echo "the test kernel, gone on from its snapshot, printed otherwise (< now, > expected):"; diff <(tr -d '\r' < $d/hello2.txt | grep -v '^pisco: ') fixtures/microvm-hello/expected.txt | head -6; exit 1; }
  echo "test kernel: snapshotted at $(sed -n 's/^pisco: snapshot at \(0x[0-9a-f]*\).*/\1/p' $d/hello1.txt), gone on in another process to its end, as fixtures/microvm-hello/expected.txt"
fi

# ---- Linux: a shell's state across two processes ----
img=${IMG:-out/images/linux}
[ -f $img/index.json ] || { echo "no Linux image at $img (target/release/pisco build recipes/linux): Linux not checked"; exit 0; }
cmd="$(cat recipes/linux/cmdline) rdinit=/bin/sh"
rm -f $d/linux.db*
# (the console's input stays open: a pipe that ended would end the shell, and with it the machine; SW_CONSOLE=tty: the
# machine on the host's clock, as when someone types)
{ printf '%s\n' 'mount -t devtmpfs dev /dev; mount -t proc proc /proc; cd /tmp; echo "written before the snapshot" > note; X=42' \
    '(i=0; while true; do i=$((i+1)); echo $i > /tmp/count; sleep 1; done) &' \
    'sleep 3; echo "count at the snapshot: $(cat /tmp/count)"; echo READY""SNAP'; sleep 600; } |
  SW_STATE=$d/linux.db SW_SNAPSHOT_ON=READYSNAP SW_CONSOLE=tty SW_CMDLINE="$cmd" timeout 600 node web/run-node.mjs $img > $d/linux1.txt 2>&1
grep -q '^pisco: snapshot .* chunks of 64 KiB' $d/linux1.txt || fail "Linux took no snapshot ($d/linux1.txt)"
at=$(tr -d '\r' < $d/linux1.txt | sed -n 's/^count at the snapshot: \([0-9]*\)$/\1/p' | tail -1)
{ printf '%s\n' 'echo "note: $(cat note), X=$X, count: $(cat count)"; sleep 2; echo "after 2 s: $(cat count), in $(pwd)"' \
    'printf "\000" | dd of=/dev/port bs=1 seek=244 count=1 2>/dev/null'; sleep 600; } |
  SW_STATE=$d/linux.db SW_CONSOLE=tty timeout 600 node web/run-node.mjs $img > $d/linux2.txt 2>&1
out=$(tr -d '\r' < $d/linux2.txt)
grep -q '^pisco: snapshot .* chunks read' <<< "$out" || fail "the second run did not go on from the snapshot ($d/linux2.txt)"
line=$(grep '^note: ' <<< "$out" | tail -1); after=$(sed -n 's/^after 2 s: \([0-9]*\), in .*/\1/p' <<< "$out" | tail -1)
now=$(sed -n 's/.*count: \([0-9]*\)$/\1/p' <<< "$line")
[[ "$line" == "note: written before the snapshot, X=42, count: "* ]] || fail "the shell's file or variable did not survive ($d/linux2.txt): ${line:-nothing}"
[ -n "$at" ] && [ -n "$now" ] && [ -n "$after" ] && [ "$now" -ge "$at" ] && [ "$after" -gt "$now" ] || fail "the counter did not go on (at the snapshot ${at:-?}, then ${now:-?}, 2 s later ${after:-?}; $d/linux2.txt)"
grep -q '^after 2 s: .*, in /tmp$' <<< "$out" || fail "the shell's working folder did not survive ($d/linux2.txt)"
size=$(du -k $d/linux.db | cut -f1)
echo "Linux: a shell's file, variable, folder and background counter ($at, then $now, then $after) went on from $d/linux.db ($size KiB) in another process"
