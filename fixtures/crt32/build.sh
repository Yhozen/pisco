#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/crt32/build.sh [out dir, default out/crt32]: crt32.exe, a 32-bit Windows program whose C functions all come
# from msvcrt.dll (llvm-mingw's import library for it) and that has no C runtime of its own. Environment: MINGW.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:-$here/../../out/crt32}; mkdir -p "$out"
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CC=$MINGW/bin/i686-w64-mingw32-clang
"$CC" -O1 -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -Wl,--entry=_start -Wl,--subsystem,console \
  "$here/crt32.c" -o "$out/crt32.exe" -lkernel32 -lmsvcrt-os "$("$CC" -print-libgcc-file-name)"
echo "$out/crt32.exe"
