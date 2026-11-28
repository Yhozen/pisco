#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# oracle.sh: boots a kernel on QEMU's machine, as the reference our own runs of it are compared with.
#   oracle.sh run <title> [extra kernel parameters...]
#       boots out/<title>/bzImage with out/<title>/initramfs.cpio and the command line of recipes/<title>/cmdline
#       (tools/build-linux.sh makes the files), on the machine of env.sh with one CPU, a serial port (the console,
#       printed here and kept in the run folder's console.txt) and an exit port at 0xf4. Exits with the byte the
#       guest wrote to the exit port, or 125 when the machine ended another way (a panic, a reset)
#   oracle.sh programs <title>
#       what the last run's programs printed: the console from the kernel's "Run /init" on, without that line.
#       This is what must be equal on every machine (recipes/<title>/traces/programs.txt)
#   oracle.sh dir <title>      prints the run folder
# Environment: ORACLE_RUN (the run folder, default out/oracle/<title>), ORACLE_TIMEOUT (seconds, default 300), and
#   env.sh's QEMU, ORACLE_QEMU_MACHINE, ORACLE_QEMU_CPU, ORACLE_QEMU_MEMORY.
# The K0 gate (docs/research/kernel-as-an-app.md) is `run` on less hardware:
#   ORACLE_QEMU_MACHINE=q35,hpet=off,pit=off,pic=off oracle.sh run linux tsc_early_khz=2900000
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); . "$here/env.sh"
cmd=${1:?usage: oracle.sh run|programs|dir <title> ...}; title=${2:?title}; shift 2
run=${ORACLE_RUN:-$root/out/oracle/$app}; mkdir -p "$run"
case $cmd in
  dir) echo "$run" ;;
  programs)
    [ -f "$run/console.txt" ] || { echo "oracle.sh: no run of $app yet (oracle.sh run $app)" >&2; exit 2; }
    sed -n '/^Run \/init as init process/,$p' "$run/console.txt" | sed 1d ;;
  run)
    command -v "$QEMU" > /dev/null || { "$here/setup.sh"; exit 2; }
    d=$root/out/$app
    for f in bzImage initramfs.cpio; do [ -f "$d/$f" ] || { echo "oracle.sh: $d/$f is not built (tools/build-linux.sh)" >&2; exit 2; }; done
    line="$(cat "$root/recipes/$app/cmdline")${*:+ $*}"
    code=0
    timeout "${ORACLE_TIMEOUT:-300}" "$QEMU" -M "$ORACLE_QEMU_MACHINE,accel=tcg" -cpu "$ORACLE_QEMU_CPU" -m "$ORACLE_QEMU_MEMORY" -smp 1 \
      -display none -no-reboot -nodefaults -no-user-config -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
      -kernel "$d/bzImage" -initrd "$d/initramfs.cpio" -append "$line" < /dev/null 2> "$run/qemu.log" | tr -d '\r' | tee "$run/console.txt" || code=${PIPESTATUS[0]}
    # the exit port: QEMU ends with (the byte written << 1) | 1; anything even is another end
    if [ $((code & 1)) = 1 ]; then exit $((code >> 1)); fi
    echo "oracle.sh: the machine ended without the exit port (QEMU's code $code; $run/qemu.log)" >&2; exit 125 ;;
  *) echo "oracle.sh: unknown command $cmd" >&2; exit 2 ;;
esac
