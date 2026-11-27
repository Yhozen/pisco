#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-busybox.sh <dir>: busybox 1.36.1 (GPL-2.0-only) as <dir>/build/busybox-1.36.1/busybox_unstripped (the path is
# printed): static, built with musl (tools/setup-toolchain.sh musl linux-headers), not position-independent, with
# its symbols; its default configuration but no `tc`. The source is fetched into <dir>/src and checked by sha256.
# Nothing of it is in the repository. tools/build-neofetch.sh and tools/build-linux.sh use it.
# Environment: MUSL (musl's folder), TOOLS (default ~/tools).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); dir=$(mkdir -p "$1" && cd "$1" && pwd)
T=${TOOLS:-$HOME/tools}; MUSL=${MUSL:-$T/musl-1.2.5}; HEADERS=$T/linux-headers-6.6-r0/include
[ -x "$MUSL/bin/musl-gcc" ] || { echo "build-busybox.sh: no musl in $MUSL (tools/setup-toolchain.sh musl)" >&2; exit 2; }
"$root/tools/setup-toolchain.sh" linux-headers > /dev/null
bb=$dir/build/busybox-1.36.1/busybox_unstripped
if [ ! -x "$bb" ]; then
  f=$dir/src/busybox-1.36.1.tar.bz2; mkdir -p "$dir/src" "$dir/build"
  [ -f "$f" ] || { curl -fsSL -o "$f.part" https://busybox.net/downloads/busybox-1.36.1.tar.bz2
    echo "b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314  $f.part" | sha256sum -c --quiet; mv "$f.part" "$f"; }
  export SOURCE_DATE_EPOCH=0
  CF="-O2 -fno-stack-protector -isystem $HEADERS -ffile-prefix-map=$dir=."
  rm -rf "$dir/build/busybox-1.36.1"; tar -xjf "$f" -C "$dir/build"
  (cd "$dir/build/busybox-1.36.1" && make defconfig > /dev/null 2>&1 \
   && sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/; s/^CONFIG_TC=y/# CONFIG_TC is not set/; s/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/; s/^CONFIG_PIE=y/# CONFIG_PIE is not set/' .config \
   && make -j "$(nproc)" CC="$MUSL/bin/musl-gcc" HOSTCC=gcc CONFIG_EXTRA_CFLAGS="$CF" EXTRA_LDFLAGS="-no-pie" busybox_unstripped > make.log 2>&1) \
   || { echo "build-busybox.sh: busybox did not build (see $dir/build/busybox-1.36.1/make.log)" >&2; exit 1; }
fi
echo "$bb"
