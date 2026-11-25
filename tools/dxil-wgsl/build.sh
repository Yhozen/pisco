#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build.sh: builds dxil2spv against a dxil-spirv build (DXIL_SPIRV, default ~/scratch/deps/dxil-spirv; its build/ folder)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); D=${DXIL_SPIRV:-$HOME/scratch/deps/dxil-spirv}
out=${1:-$here/../../target/dxil2spv}
g++ -O2 -std=c++17 -I"$D" "$here/dxil2spv.cpp" -o "$out" -L"$D/build" -ldxil-spirv-c-shared -Wl,-rpath,"$D/build"
echo "$out"
