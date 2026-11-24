#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/x87/build.sh [out dir, default out/x87]: x87.exe, a 32-bit Windows program with no C runtime and no
# SSE (x87 only, as Visual C++ 7.1 compiled). Environment: MINGW (llvm-mingw folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:-$here/../../out/x87}; mkdir -p "$out"
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CC=$MINGW/bin/i686-w64-mingw32-clang
"$CC" -O1 -march=i586 -mno-sse -mno-mmx -mfpmath=387 -masm=intel -ffreestanding -fno-stack-protector -fno-builtin \
  -nostdlib -Wl,--entry=_start -Wl,--subsystem,console "$here/x87.c" -o "$out/x87.exe" -lkernel32 "$("$CC" -print-libgcc-file-name)"
echo "$out/x87.exe"
