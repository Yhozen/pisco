#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/d3dx/build.sh <out dir> <d3dx9_35.dll>: d3dx.exe, with the user's own d3dx9_35.dll copied beside it.
# Environment: MINGW (llvm-mingw folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=$1; dll=$2; mkdir -p "$out"
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CC=$MINGW/bin/i686-w64-mingw32-clang
"$MINGW/bin/llvm-dlltool" -m i386 -k -d "$here/d3dx9_35.def" -l "$out/libd3dx9_35.a"
"$CC" -O1 -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -Wl,--entry=_start -Wl,--subsystem,console \
  "$here/d3dx.c" -o "$out/d3dx.exe" -lkernel32 "$out/libd3dx9_35.a" "$("$CC" -print-libgcc-file-name)"
cp "$dll" "$out/d3dx9_35.dll"
echo "$out/d3dx.exe"
