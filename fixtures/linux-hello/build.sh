#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/linux-hello/build.sh [out dir, default out/linux-hello]: hello, a Linux x86-64 program statically linked
# with musl (tools/setup-toolchain.sh musl), not position-independent, with its symbols.
# Environment: MUSL (musl's folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:-$here/../../out/linux-hello}; mkdir -p "$out"
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
"$MUSL/bin/musl-gcc" -O2 -static -no-pie "$here/hello.c" -o "$out/hello" -lm
echo "$out/hello"
