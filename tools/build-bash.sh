#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-bash.sh <dir>: bash 5.2.37 (GPL-3.0-or-later) as <dir>/build/bash-5.2.37/bash (the path is printed): static,
# built with musl (tools/setup-toolchain.sh musl linux-headers), not position-independent, no readline, no job
# control, no bash malloc. The source is fetched into <dir>/src and checked by sha256. Nothing of it is in the
# repository. tools/build-neofetch.sh and tools/build-linux.sh use it.
# Environment: MUSL (musl's folder), TOOLS (default ~/tools).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); dir=$(mkdir -p "$1" && cd "$1" && pwd)
T=${TOOLS:-$HOME/tools}; MUSL=${MUSL:-$T/musl-1.2.5}; HEADERS=$T/linux-headers-6.6-r0/include
[ -x "$MUSL/bin/musl-gcc" ] || { echo "build-bash.sh: no musl in $MUSL (tools/setup-toolchain.sh musl)" >&2; exit 2; }
"$root/tools/setup-toolchain.sh" linux-headers > /dev/null
b=$dir/build/bash-5.2.37/bash
if [ ! -x "$b" ]; then
  f=$dir/src/bash-5.2.37.tar.gz; mkdir -p "$dir/src" "$dir/build"
  [ -f "$f" ] || { curl -fsSL -o "$f.part" https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz
    echo "9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff  $f.part" | sha256sum -c --quiet; mv "$f.part" "$f"; }
  export SOURCE_DATE_EPOCH=0
  CF="-O2 -fno-stack-protector -isystem $HEADERS -ffile-prefix-map=$dir=."
  rm -rf "$dir/build/bash-5.2.37"; tar -xzf "$f" -C "$dir/build"
  (cd "$dir/build/bash-5.2.37" && CC="$MUSL/bin/musl-gcc" CFLAGS="$CF" LDFLAGS="-static -no-pie" ./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
     --without-bash-malloc --enable-static-link --disable-nls --without-curses --disable-readline --disable-job-control > configure.log 2>&1 \
   && make -j "$(nproc)" > make.log 2>&1) || { echo "build-bash.sh: bash did not build (see $dir/build/bash-5.2.37)" >&2; exit 1; }
fi
echo "$b"
