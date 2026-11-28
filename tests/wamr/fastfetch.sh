#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run from the repository root, with a prepared PISCO fastfetch image and WAMR on PATH.
set -euo pipefail
out=out/wamr-fastfetch
image=${FASTFETCH_IMAGE:-out/images/fastfetch}
mkdir -p "$out"
node --input-type=module - "$image" "$out" <<'JS'
import { writeFileSync } from 'node:fs';
import { openImageDir } from './web/image.mjs';
const [dir, out] = process.argv.slice(2);
const image = await openImageDir(dir);
if (image.settings.exe !== 'usr/bin/fastfetch') throw new Error('expected the fastfetch recipe');
writeFileSync(out + '/fastfetch', image.drive.read(image.settings.exe));
writeFileSync(out + '/app.wasm', image.wasm);
JS
cc -O2 -Wall -Werror -shared -fPIC -I"$WAMR_INCLUDE" tests/wamr/fastfetch-host.c -o "$out/host.so"
wamrc --enable-multi-thread --bounds-checks=1 -o "$out/app.aot" "$out/app.wasm"
PISCO_TEST_ELF="$out/fastfetch" timeout 300 iwasm --heap-size=0 --native-lib="$out/host.so" \
    --env=SW_STRICT=1 --env=SW_CLOCK=16667 --env=SW_EXE=/usr/bin/fastfetch \
    "$out/app.aot" --pipe -l none > "$out/got.txt"
diff -u recipes/fastfetch/traces/pipe.txt "$out/got.txt"
echo "fastfetch: all 13 lines from WAMR AOT match the PISCO reference"
