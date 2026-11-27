#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/avx/build.sh [out dir, default out/avx]: avx, a Linux x86-64 program statically linked with musl
# (tools/setup-toolchain.sh musl), built for a Zen 2 CPU (the PS5's): AVX, AVX2, FMA and BMI in every function.
# expected.txt is what it prints run natively on such a CPU. Environment: MUSL (musl's folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); out=${1:-$here/../../out/avx}; mkdir -p "$out"
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
"$MUSL/bin/musl-gcc" -O1 -march=znver2 -static -no-pie "$here/avx.c" -o "$out/avx"
echo "$out/avx"
