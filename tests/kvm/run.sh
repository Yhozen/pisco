#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd "$(dirname "$0")/../.."
skip_kvm() {
    if [[ ${KVM_REQUIRED:-0} == 1 ]]; then echo "KVM required: $*" >&2; exit 1; fi
    echo "skip  KVM: $*"
    exit 0
}
[[ $(uname -sm) == "Linux x86_64" ]] || skip_kvm "requires x86-64 Linux"

out=out/kvm
mkdir -p "$out"
cargo build --locked -q --release -p pisco-lifter
rustc --edition=2021 --target=x86_64-unknown-linux-gnu --crate-type=lib --emit=obj \
    -C opt-level=3 -C panic=abort tests/memory64/fnv.rs -o "$out/fnv.o"
# The VM loads this section at the same address. FNV only uses code and its argument buffer.
ld -m elf_x86_64 --gc-sections -Ttext=0x10000 -e hash "$out/fnv.o" -o "$out/fnv"
objcopy -O binary --only-section=.text "$out/fnv" "$out/fnv.text"
xxd -i < "$out/fnv.text" > "$out/fnv-text.inc"
target/release/lifter "$out/fnv" "$out/lifted.c"

sdk=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}
clang=clang
if [[ -x $sdk/bin/clang ]]; then clang=$sdk/bin/clang; fi
"$clang" --target=wasm32-unknown-unknown -O2 -ffreestanding -fno-strict-aliasing -nostdlib \
    -Irt -I"$out" "$out/lifted.c" tests/kvm/probe.c \
    -Wl,--no-entry,--export=check -o "$out/probe.wasm"

cargo build -q --locked --manifest-path tests/kvm/Cargo.toml --target-dir target/kvm
probe=target/kvm/debug/pisco-kvm-probe
timeout 30 "$probe" lifted "$out/probe.wasm"
[[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || skip_kvm "need read/write access to /dev/kvm"
timeout 30 "$probe" kvm "$out/probe.wasm"
