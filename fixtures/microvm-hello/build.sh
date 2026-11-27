#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/microvm-hello/build.sh <out dir>: the test kernel as <out dir>/hello (an ELF linked as Linux is, with a
# PVH note so QEMU boots it). Needs the machine's gcc and GNU ld.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=$(mkdir -p "$1" && cd "$1" && pwd)
CF="-O2 -ffreestanding -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -fno-jump-tables -fno-builtin -ffile-prefix-map=$here=."
gcc $CF -c "$here/boot.S" -o "$out/boot.o"
gcc $CF -c "$here/hello.c" -o "$out/hello.o"
ld -nostdlib -static -no-pie -z noexecstack -z max-page-size=4096 --build-id=none -T "$here/link.ld" "$out/boot.o" "$out/hello.o" -o "$out/hello"
echo "$out/hello"
