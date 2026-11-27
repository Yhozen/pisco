#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/linux-dyn/build.sh <out dir> <ld-musl-x86_64.so.1>: the fixture, linked dynamically against musl's loader
# (which is also its C library: Alpine's, tools/build-linux.sh). Makes <out>/dyn (position-independent, its loader
# /lib/ld-musl-x86_64.so.1), <out>/libgreet.so (which dyn is linked against) and <out>/libplug.so (which it opens).
# Prints the out dir. Environment: MUSL (musl's folder, for its headers and start files).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=$1; ldso=$2; mkdir -p "$out/c"
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}; ln -sf "$ldso" "$out/c/libc.so"
cc() { "$MUSL/bin/musl-gcc" -O2 -fPIC "$@"; }
link() { gcc -nostdlib -Wl,--hash-style=both "$@" -L"$out/c" -lc; }
cc -c "$here/greet.c" -o "$out/greet.o"; link -shared -Wl,-soname,libgreet.so "$out/greet.o" -o "$out/libgreet.so"
cc -c "$here/plug.c" -o "$out/plug.o"; link -shared -Wl,-soname,libplug.so "$out/plug.o" -o "$out/libplug.so"
cc -c "$here/dyn.c" -o "$out/dyn.o"
link -pie -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 "$MUSL/lib/Scrt1.o" "$MUSL/lib/crti.o" "$out/dyn.o" -L"$out" -lgreet "$MUSL/lib/crtn.o" -o "$out/dyn"
echo "$out"
