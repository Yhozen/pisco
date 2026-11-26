#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-fastfetch.sh [out dir, default out/fastfetch]: fastfetch (https://github.com/fastfetch-cli/fastfetch, MIT) as a
# Linux title: one x86-64 program, statically linked with musl, not position-independent, with its symbols (the
# lifter finds its functions by them). Its source is fetched and checked by sha256; nothing of it is in the
# repository (docs/fastfetch.md). No library it can load at run time is built in, and no threads: a title is one
# process with one thread.
# fastfetch is C23: it is compiled with llvm-mingw's clang (a cross compiler: its x86-64 back end serves Linux as
# well), against musl (tools/setup-toolchain.sh musl) and the kernel's headers (tools/setup-toolchain.sh linux-headers).
# The link is GNU ld's (fastfetch passes it an option of its own).
# Environment: MUSL, MINGW (the toolchains' folders), TOOLS (default ~/tools: the kernel headers go there).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mkdir -p "${1:-$root/out/fastfetch}" && cd "${1:-$root/out/fastfetch}" && pwd)
T=${TOOLS:-$HOME/tools}; MUSL=${MUSL:-$T/musl-1.2.5}; MINGW=${MINGW:-$T/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}
VERSION=2.71.0; SHA=e71d93de41e8e4b2554c36d2ae51758112a21ecf3a8c98605a2c689251e62987
HEADERS=linux-headers-6.6-r0   # (tools/setup-toolchain.sh linux-headers)
[ -x "$MUSL/bin/musl-gcc" ] || { echo "build-fastfetch.sh: no musl in $MUSL (tools/setup-toolchain.sh musl)" >&2; exit 2; }
[ -x "$MINGW/bin/clang" ] || { echo "build-fastfetch.sh: no clang in $MINGW (tools/setup-toolchain.sh mingw)" >&2; exit 2; }
"$root/tools/setup-toolchain.sh" linux-headers > /dev/null
src=$out/fastfetch-$VERSION
if [ ! -d "$src" ]; then
  curl -fsSL -o "$out/src.tar.gz" "https://github.com/fastfetch-cli/fastfetch/archive/refs/tags/$VERSION.tar.gz"
  echo "$SHA  $out/src.tar.gz" | sha256sum -c --quiet
  tar -xzf "$out/src.tar.gz" -C "$out"; rm "$out/src.tar.gz"
fi
OFF=""; for o in VULKAN WAYLAND XCB_RANDR XRANDR DRM VADRM VAX11 VDPAU GIO DCONF EET DBUS SQLITE3 RPM EGL GLX OPENCL ELF IMAGE_LOGO ZLIB LUA LIBZFS WORDEXP THREADS LTO; do OFF="$OFF -DENABLE_$o=OFF"; done
# (the same bytes from the same sources: no build paths or dates in the program)
export SOURCE_DATE_EPOCH=0
# (RelWithDebInfo without -g: fastfetch's Release strips the symbols and leaves out the unwind tables)
cmake -S "$src" -B "$out/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG" -DCMAKE_C_COMPILER="$MINGW/bin/clang" -DCMAKE_C_COMPILER_TARGET=x86_64-linux-musl -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_AR="$MINGW/bin/llvm-ar" -DCMAKE_RANLIB="$MINGW/bin/llvm-ranlib" \
  -DCMAKE_C_FLAGS="--sysroot=$MUSL -isystem $T/$HEADERS/include -ffile-prefix-map=$out=. -fno-stack-protector" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -fuse-ld=bfd -nostdlib $MUSL/lib/crt1.o $MUSL/lib/crti.o -Wl,--build-id=none" -DCMAKE_C_STANDARD_LIBRARIES="-lc $MUSL/lib/crtn.o" \
  -DBINARY_LINK_TYPE=static -DIS_MUSL=ON -DBUILD_FLASHFETCH=OFF -DSET_TWEAK=OFF -DINSTALL_LICENSE=OFF $OFF > "$out/cmake.log" 2>&1 || { tail -20 "$out/cmake.log" >&2; exit 1; }
cmake --build "$out/build" --target fastfetch -j "$(nproc)" > "$out/make.log" 2>&1 || { grep -m8 -B2 -A6 'error' "$out/make.log" >&2; exit 1; }
cp "$out/build/fastfetch" "$out/fastfetch"
echo "$out/fastfetch"
