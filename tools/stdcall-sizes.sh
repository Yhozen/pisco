#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# stdcall-sizes.sh < names: for each "dll!name" line on stdin, prints "dll!name bytes" when the 32-bit import
# library of that DLL knows the function: the bytes of arguments a stdcall function removes from the stack
# (`_Name@12` -> 12), or 0 for a cdecl one (`_name`). A name that carries its own decoration (`_Name@12`,
# as fmod.dll exports them) needs no library. The C runtime's DLLs export only cdecl functions besides their C++
# ones (`?name@@...`): a plain name the library lacks (mingw keeps ldexp and others in its own static library) is
# cdecl too. Names nothing knows are left out: the runtime stops if one is called.
# The libraries are llvm-mingw's (MINGW); what each DLL exports is cached in out/apis/i686/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; cache=$root/out/apis/i686; mkdir -p "$cache"
names=$(mktemp); trap 'rm -f "$names"' EXIT; sort -u > "$names"
for dll in $(cut -d! -f1 "$names" | sort -u); do
  lib=$MINGW/i686-w64-mingw32/lib/lib${dll%.dll}.a
  [ -f "$cache/$dll" ] || { [ -f "$lib" ] && "$MINGW/bin/llvm-nm" "$lib" 2>/dev/null | awk '$2 == "T" && $3 !~ /^__imp_/ {s = substr($3, 2); n = split(s, p, "@"); print p[1], (n > 1 ? p[2] : 0)}' | sort -u > "$cache/$dll" || : > "$cache/$dll"; }
  awk -v dll="$dll" 'FILENAME == ARGV[1] { size[$1] = $2; next }
    { split($0, a, "!"); if (a[1] != dll) next; n = a[2]
      if (n in size) print $0, size[n]
      else if (match(n, /@[0-9]+$/)) print $0, substr(n, RSTART + 1)
      else if (dll ~ /^(msvcrt?[0-9]*d?|ucrtbase|api-ms-win-crt-.*)\.dll$/ && n !~ /^\?/) print $0, 0 }' "$cache/$dll" "$names"
done
