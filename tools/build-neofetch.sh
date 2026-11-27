#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-neofetch.sh [out dir, default out/neofetch]: a small Linux for neofetch (docs/neofetch.md), in <out>/root, the
# root of the title (recipes/neofetch):
#   bin/bash       bash 5.2.37 (GPL-3.0-or-later), static, no readline, no job control
#   bin/busybox    busybox 1.36.1 (GPL-2.0-only), static, its default configuration but no `tc` (tools/build-busybox.sh);
#                  every applet in bin/
#                  and usr/bin/env as a script that runs it (#!/bin/busybox: busybox takes the applet from its path)
#   bin/sh         bash again
#   usr/bin/neofetch  neofetch 7.1.0 (MIT), as it is
# Each is fetched and checked by sha256, and built with musl (tools/setup-toolchain.sh musl linux-headers), not
# position-independent, with its symbols. Nothing of them is in the repository.
# Environment: MUSL (musl's folder), TOOLS (default ~/tools).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mkdir -p "${1:-$root/out/neofetch}" && cd "${1:-$root/out/neofetch}" && pwd)
T=${TOOLS:-$HOME/tools}; MUSL=${MUSL:-$T/musl-1.2.5}; HEADERS=$T/linux-headers-6.6-r0/include
[ -x "$MUSL/bin/musl-gcc" ] || { echo "build-neofetch.sh: no musl in $MUSL (tools/setup-toolchain.sh musl)" >&2; exit 2; }
"$root/tools/setup-toolchain.sh" linux-headers > /dev/null
fetch() { # url, sha256, file: fetched once into <out>/src
  mkdir -p "$out/src"; [ -f "$out/src/$3" ] && return
  curl -fsSL -o "$out/src/$3.part" "$1"; echo "$2  $out/src/$3.part" | sha256sum -c --quiet; mv "$out/src/$3.part" "$out/src/$3"
}
fetch https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz 9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff bash-5.2.37.tar.gz
fetch https://github.com/dylanaraps/neofetch/archive/refs/tags/7.1.0.tar.gz 58a95e6b714e41efc804eca389a223309169b2def35e57fa934482a6b47c27e7 neofetch-7.1.0.tar.gz
export SOURCE_DATE_EPOCH=0
CF="-O2 -fno-stack-protector -isystem $HEADERS -ffile-prefix-map=$out=."
if [ ! -x "$out/build/bash-5.2.37/bash" ]; then
  rm -rf "$out/build/bash-5.2.37"; mkdir -p "$out/build"; tar -xzf "$out/src/bash-5.2.37.tar.gz" -C "$out/build"
  (cd "$out/build/bash-5.2.37" && CC="$MUSL/bin/musl-gcc" CFLAGS="$CF" LDFLAGS="-static -no-pie" ./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
     --without-bash-malloc --enable-static-link --disable-nls --without-curses --disable-readline --disable-job-control > configure.log 2>&1 \
   && make -j "$(nproc)" > make.log 2>&1) || { echo "build-neofetch.sh: bash did not build (see $out/build/bash-5.2.37)" >&2; exit 1; }
fi
"$root/tools/build-busybox.sh" "$out" > /dev/null   # busybox, into the same folders
[ -f "$out/build/neofetch-7.1.0/neofetch" ] || tar -xzf "$out/src/neofetch-7.1.0.tar.gz" -C "$out/build"
R=$out/root; rm -rf "$R"; mkdir -p "$R/bin" "$R/usr/bin"
cp "$out/build/bash-5.2.37/bash" "$R/bin/bash"; cp "$R/bin/bash" "$R/bin/sh"
cp "$out/build/busybox-1.36.1/busybox_unstripped" "$R/bin/busybox"
for a in $("$R/bin/busybox" --list); do [ -e "$R/bin/$a" ] || printf '#!/bin/busybox\n' > "$R/bin/$a"; done
printf '#!/bin/busybox\n' > "$R/usr/bin/env"
cp "$out/build/neofetch-7.1.0/neofetch" "$R/usr/bin/neofetch"
chmod 755 "$R"/bin/* "$R"/usr/bin/*
echo "$R"
