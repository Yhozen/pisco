#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# lift-side.sh <elf> <name> <out.wasm> [<hex address>...]: one x86-64 program as a wasm side module for PISCO's
# runtime in unkernel's kernel (runtime.c): tools/build-side.sh's lift (lifter --user, machine mode), compiled for a
# memory that is not shared (unkernel's one-CPU kernel). tabc's supervisor runs it for a program met for the first
# time (TABC_PISCO_LIFT); run-node.mjs too. Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS,
# OPT (default -O2), SPLIT (default 16).
# LIFTER_MUSTTAIL (default 2): jumps into other functions, direct and indirect, as tail calls (runtime.c's rt_resolve).
# JOBS's default is the CPUs, but no more than one compile per 2 GiB available: a large library's units take 1.6 GB
# each in clang, and a compile killed for memory makes xargs start no more (the lift fails, after the others).
# lift-side.sh --tools: what makes a lift's code besides the image (the lifter, rt/cpu.h, this script, the options),
# as a hash: tabc's supervisor keeps a module by the image's sha256 and this.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
export LIFTER_MUSTTAIL=${LIFTER_MUSTTAIL-2}
if [ "${1:-}" = --tools ]; then
  # (LIFT_TOOLS_KEY: a key given, for a cache kept across a lifter change that only adds instructions, which the
  # modules lifted before stop at; those that met one are removed by hand and lifted again)
  if [ -n "${LIFT_TOOLS_KEY:-}" ]; then echo "$LIFT_TOOLS_KEY"; exit 0; fi
  { cat "$root/target/release/lifter" "$root/rt/cpu.h" "$0"; echo "$LIFTER_MUSTTAIL ${OPT:--O2} ${SPLIT:-16}"; } | sha256sum | cut -c1-16; exit 0
fi
elf=$1; name=$2; out=$3; shift 3
entries=(); for a in "$@"; do entries+=(--entry "$a"); done
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
mem=$(awk '/MemAvailable/ { print int($2 / 2097152) }' /proc/meminfo 2>/dev/null || echo 0)
JOBS=${JOBS:-$(n=$(nproc); [ "$mem" -ge 1 ] && [ "$mem" -lt "$n" ] && n=$mem; echo "$n")}
d=$(mktemp -d "${TMPDIR:-/tmp}/tabc-pisco-side.XXXXXX"); trap 'rm -rf "$d"' EXIT
"$root/target/release/lifter" "$elf" "$d/app.c" --split "${SPLIT:-16}" --user "$name" "${entries[@]}" > "$d/lifter.log" 2>&1 || { cat "$d/lifter.log" >&2; exit 1; }
CF="--target=wasm32-wasip1 -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=64 -DGUEST_MACHINE=1 -DGUEST_LINUX=1 -DGUEST_POLL_FLAGS=1 -mtail-call -fPIC -ffreestanding"
ls "$d"/app*.c | xargs -P "$JOBS" -I{} sh -c '"$0"/clang $1 -c "{}" -o "{}.o"' "$W" "$CF"
"$W/wasm-ld" -shared --experimental-pic --export="${name}_program" --unresolved-symbols=import-dynamic -o "$out.part" "$d"/app*.c.o
mv "$out.part" "$out"
