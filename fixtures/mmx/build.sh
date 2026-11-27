#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/mmx/build.sh [out dir, default out/mmx]: mmx.exe, a 32-bit Windows program with no C runtime whose
# arithmetic is MMX's (and the SSE instructions that work on MMX registers). Environment: MINGW (llvm-mingw folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:-$here/../../out/mmx}; mkdir -p "$out"
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CC=$MINGW/bin/i686-w64-mingw32-clang
"$CC" -O1 -march=pentium3 -mno-sse -mfpmath=387 -ffreestanding -fno-stack-protector -fno-builtin \
  -nostdlib -Wl,--entry=_start -Wl,--subsystem,console "$here/mmx.c" -o "$out/mmx.exe" -lkernel32 "$("$CC" -print-libgcc-file-name)"
echo "$out/mmx.exe"
