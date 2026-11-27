#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run from the repository root. WAMR and Wasmtime are optional (docs/research/wamr-unkernel-tabc.md).
set -euo pipefail
out=out/memory64
clang=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin/clang
want=48e7c13380222325   # fixtures/bench-native, FNV over 64 MiB
limit=$(( (5 << 30) + (64 << 20) ))
mkdir -p "$out"
cargo build -q --release -p pisco-lifter
rustc --edition=2021 --target=x86_64-unknown-linux-gnu --crate-type=lib --emit=obj \
    -C opt-level=3 -C panic=abort tests/memory64/fnv.rs -o "$out/fnv.o"
ld -m elf_x86_64 --gc-sections -e hash "$out/fnv.o" -o "$out/fnv"
target/release/lifter "$out/fnv" "$out/lifted.c"

if ! command -v wamrc >/dev/null || ! command -v iwasm >/dev/null; then
    echo "skip  WAMR: put wamrc and a Memory64-enabled iwasm on PATH"
fi
command -v wasmtime >/dev/null || echo "skip  Wasmtime: wasmtime is not on PATH"
for kind in plain shared; do
    wasm="$out/$kind.wasm"
    flags=()
    if [[ $kind == shared ]]; then flags=(-DSHARED -matomics -mbulk-memory -Wl,--shared-memory); fi
    "$clang" --target=wasm64-unknown-unknown -O2 -ffreestanding -fno-strict-aliasing -nostdlib \
        -fno-vectorize -fno-slp-vectorize -Irt "${flags[@]}" "$out/lifted.c" tests/memory64/probe.c \
        -Wl,--no-entry,--export=check,--initial-memory=131072,--max-memory="$limit" -o "$wasm"
    node --input-type=module - "$wasm" "$want" "$limit" <<'JS'
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
const [file, expected, limit] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(file));
assert.equal(instance.exports.check().toString(16), expected);
assert.equal(instance.exports.memory.buffer.byteLength, Number(limit));
JS
    if command -v wamrc >/dev/null && command -v iwasm >/dev/null; then
        wamrc --bounds-checks=1 --enable-multi-thread -o "$out/$kind.aot" "$wasm"
        for module in "$wasm" "$out/$kind.aot"; do
            iwasm --heap-size=0 -f check "$module" | tee "$module.log"
            grep -Fxq "0x$want:i64" "$module.log"
        done
    fi
    if command -v wasmtime >/dev/null; then
        wasmtime run -W threads=y,shared-memory=y -O memory-reservation="$limit" \
            --invoke check "$wasm" | tee "$wasm.wasmtime.log"
        grep -Fxq "$((16#$want))" "$wasm.wasmtime.log"
    fi
done
echo "memory64: lifted FNV at 5 GiB matches the native checksum in plain and shared memory"
