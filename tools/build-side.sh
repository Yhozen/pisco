#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-side.sh <elf> <name> <out.wasm>: a program of a machine (rt/microvm-x64) as a module of its own (a wasm side
# module, web/side.mjs): lifted (lifter --user <name>), compiled position-independent with the flags of a machine's
# title, linked by wasm-ld -shared. web/lift.mjs runs it for a program met when it first runs.
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS, OPT (default -O2), SPLIT (default 4).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); elf=$1; name=$2; out=$3
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin; JOBS=${JOBS:-$(nproc)}
d=$(mktemp -d "${TMPDIR:-/tmp}/pisco-side.XXXXXX"); trap 'rm -rf "$d"' EXIT
"$root/target/release/lifter" "$elf" "$d/title.c" --split "${SPLIT:-4}" --user "$name" > "$d/lifter.log" 2>&1 || { cat "$d/lifter.log" >&2; exit 1; }
CF="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=64 -DGUEST_MACHINE=1 -DGUEST_LINUX=1 -mtail-call -fPIC -ffreestanding"
ls "$d"/title*.c | xargs -P "$JOBS" -I{} sh -c '"$0"/clang $1 -c "{}" -o "{}.o"' "$W" "$CF"
"$W/wasm-ld" -shared --experimental-pic --shared-memory --export="${name}_program" --unresolved-symbols=import-dynamic -o "$out.part" "$d"/title*.c.o
mv "$out.part" "$out"
