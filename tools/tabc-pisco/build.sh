#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build.sh [out]: PISCO's part of tabc-pisco (docs/research/tabc-pisco.md), in out (default out/tabc-pisco):
#   kernel.wasm    unkernel's one-CPU kernel as tabc 1.0.0 ships it (npm unkernel@$UNKERNEL), kw_run patched to ask PISCO
#                  first (patch-kernel.mjs; built here from the release, never kept in the repository)
#   runtime.wasm   PISCO's runtime for x86-64 tasks (runtime.c on rt/cpu.h), in the kernel's memory above its RAM
#   tramp.wasm     the four functions the kernel's `pisco` imports call, a table filled once the runtime is there
#   x86load        the aarch64 loader binfmt_misc runs for every x86-64 ELF file
# Needs node and npm, wasi-sdk (WASI_SDK), a clang that targets aarch64 and ld.lld, and the lifter (cargo build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); out=${1:-$root/out/tabc-pisco}
UNKERNEL=${UNKERNEL:-0.2.0-261125-cec0a49}   # the version tabc 1.0.0 pins (nix/unkernel.nix)
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
mkdir -p "$out"
(cd "$root" && cargo build --release -q -p pisco-lifter)
if [ ! -f "$out/unkernel/package/dist/unkernel.wasm" ]; then
  mkdir -p "$out/unkernel" && (cd "$out/unkernel" && npm pack -s "unkernel@$UNKERNEL" > /dev/null && tar xzf "unkernel-$UNKERNEL.tgz")
fi
[ -d "$out/node_modules/binaryen" ] || npm i -s --prefix "$out" binaryen@132 > /dev/null
# (run from out, where binaryen is: ES modules find packages from where the script is)
cp "$here/patch-kernel.mjs" "$here/mktramp.mjs" "$out/"
node "$out/patch-kernel.mjs" "$out/unkernel/package/dist/unkernel.wasm" "$out/kernel.wasm" | tail -1
node "$out/mktramp.mjs" "$out/tramp.wasm"
CF="--target=wasm32-wasip1 -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext -mtail-call -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=64 -DGUEST_MACHINE=1 -DGUEST_LINUX=1"
ex=$(grep -v '^#' "$root/rt/microvm-x64/side-exports.txt" | tr ' ' '\n' | grep -v '^$' | sort -u)
LINK="-Wl,--import-memory,--initial-memory=$((0x41000000)),--max-memory=4294967296,--global-base=$((0x40000000)),-z,stack-size=8388608"
LINK="$LINK -Wl,--export-table,--growable-table,--export=__stack_pointer,--export=__heap_base"
for x in $ex; do LINK="$LINK -Wl,-u,$x,--export=$x"; done
"$W/clang" $CF -mexec-model=reactor "$here/runtime.c" -o "$out/runtime.wasm" $LINK -lsetjmp
clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -fuse-ld=lld \
  -Wl,--image-base=0x40000000,-e,_start "$here/x86load.c" -o "$out/x86load"
echo "$out: kernel.wasm $(stat -c %s "$out/kernel.wasm"), runtime.wasm $(stat -c %s "$out/runtime.wasm"), x86load $(stat -c %s "$out/x86load") bytes"
