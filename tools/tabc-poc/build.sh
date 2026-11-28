#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/tabc-poc/build.sh: the Alpine container image of docs/research/tabc-poc.md, out/images/alpine, and the one-time
# costs it took (out/tabc-poc/build-times.txt):
#   1. Linux 6.18, the kernel alone (tools/build-linux.sh, KERNEL_ONLY=1)
#   2. alpine:3.24.4 for linux/amd64 from Docker Hub (pull.py), its layer as the initramfs with /init and the
#      benchmark added (mkinitramfs.py); the benchmark built by zig cc (ZIG, default `python3 -m ziglang`) for x86-64
#      and aarch64, from bench.c; the workloads' script (workload) added too
#   3. pisco build recipes/alpine: the kernel lifted and compiled
#   4. the image run once in Node with every workload (tools/tabc-poc/workload all), so that each program it runs
#      (musl's loader, busybox, the benchmark) is lifted into a module of its own, in out/tabc-poc/lifted
#   5. pisco build again: the same kernel (cached), and those modules as layers, for the page
#   (INKERNEL=1: also recipes/alpine-k, out/images/alpine-k, the benchmark lifted into the kernel's module too)
# Needs what tools/build-linux.sh and pisco build need (tools/setup-toolchain.sh wasi musl linux-headers kernel).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); cd "$root"
o=out/tabc-poc; mkdir -p $o/lifted $o/root $o/bench; T=$o/build-times.txt; : > $T
step() { local n=$1; shift; local s=$(date +%s.%N); "$@"; awk -v n="$n" -v s="$s" -v e="$(date +%s.%N)" 'BEGIN { printf "%-34s %8.1f s\n", n, e - s }' | tee -a $T; }
ZIG=${ZIG:-python3 -m ziglang}
cargo build -q --release -p pisco-lifter -p pisco
step "kernel (gcc)" env KERNEL_ONLY=1 tools/build-linux.sh > /dev/null
step "pull alpine:3.24.4 amd64" python3 -I tools/tabc-poc/pull.py alpine:3.24.4 linux/amd64 $o/oci/amd64 > /dev/null
for t in x86_64:x86_64 aarch64:generic; do $ZIG cc -target ${t%%:*}-linux-musl -mcpu=${t##*:} -O2 -s -static -o $o/bench/bench-${t%%:*} tools/tabc-poc/bench.c; done
step "initramfs from the layer" python3 -I tools/tabc-poc/mkinitramfs.py $o/root/initramfs.cpio $o/oci/amd64/layer0.tar.gz \
  --add init=recipes/alpine/init --add usr/local/bin/bench=$o/bench/bench-x86_64 --add usr/local/bin/workload=tools/tabc-poc/workload > /dev/null
cp out/linux/vmlinux $o/root/vmlinux
rm -f $o/lifted/*.wasm
step "pisco build (the kernel lifted)" target/release/pisco build recipes/alpine > $o/build1.log 2>&1
# every program the benchmark runs, run once: what is met is lifted (web/lift.mjs), into out/tabc-poc/lifted
cmd='workload all'
step "first run in Node (programs lifted)" env PISCO_LIFT_CACHE=$o/lifted SW_CMDLINE="$(sed -n 's/^cmdline = "\(.*\)"$/\1/p' recipes/alpine/recipe.toml) -- $cmd" \
  target/release/pisco run out/images/alpine > $o/training.log 2>&1 < /dev/null
grep -q "container: exit 0" $o/training.log || { echo "build.sh: the first run did not end well ($o/training.log)" >&2; tail -5 $o/training.log >&2; exit 1; }
step "pisco build (with the lifted modules)" target/release/pisco build recipes/alpine > $o/build2.log 2>&1
# INKERNEL=1: recipes/alpine-k too, the benchmark also lifted into the kernel's module (an experiment of the doc)
if [ "${INKERNEL:-}" = 1 ]; then
  mkdir -p $o/root-k; cp $o/root/vmlinux $o/root/initramfs.cpio $o/root-k/; cp $o/bench/bench-x86_64 $o/root-k/bench
  step "pisco build recipes/alpine-k" target/release/pisco build recipes/alpine-k > $o/build-k.log 2>&1
fi
ls -la $o/lifted/*.wasm | awk '{ print $5, $9 }' >> $T
grep "lifted it in" $o/training.log >> $T || true
du -sb out/images/alpine >> $T
cat $T
