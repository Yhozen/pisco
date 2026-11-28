#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# lift-side.sh <elf> <name> <out.wasm> [<hex address>...]: one x86-64 program as a wasm side module for PISCO's
# runtime in unkernel's kernel (runtime.c): tools/build-side.sh's lift (lifter --user, machine mode), compiled for a
# memory that is not shared (unkernel's one-CPU kernel). tabc's supervisor runs it for a program met for the first
# time (TABC_PISCO_LIFT); run-node.mjs too. Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS,
# OPT (default -O2), SPLIT (default 16).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); elf=$1; name=$2; out=$3; shift 3
entries=(); for a in "$@"; do entries+=(--entry "$a"); done
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin; JOBS=${JOBS:-$(nproc)}
d=$(mktemp -d "${TMPDIR:-/tmp}/tabc-pisco-side.XXXXXX"); trap 'rm -rf "$d"' EXIT
"$root/target/release/lifter" "$elf" "$d/app.c" --split "${SPLIT:-16}" --user "$name" "${entries[@]}" > "$d/lifter.log" 2>&1 || { cat "$d/lifter.log" >&2; exit 1; }
CF="--target=wasm32-wasip1 -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=64 -DGUEST_MACHINE=1 -DGUEST_LINUX=1 -mtail-call -fPIC -ffreestanding"
ls "$d"/app*.c | xargs -P "$JOBS" -I{} sh -c '"$0"/clang $1 -c "{}" -o "{}.o"' "$W" "$CF"
"$W/wasm-ld" -shared --experimental-pic --export="${name}_program" --unresolved-symbols=import-dynamic -o "$out.part" "$d"/app*.c.o
mv "$out.part" "$out"
