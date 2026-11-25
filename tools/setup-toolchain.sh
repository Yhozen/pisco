#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# setup-toolchain.sh [step...]: installs what the builds and tests need, outside the repository.
# Steps (default: all of them); a step whose result exists is skipped:
#   wasi   wasi-sdk 34 in ~/tools                          tools/build-title.sh
#   mingw  llvm-mingw 20261117 in ~/tools                  fixtures cxx, dll, doom, hello32, x87
#   rust   the Windows targets of the Rust fixtures
#   naga   naga-cli (cargo install)                        tools/shader-prep.mjs, checks of generated WGSL
#   dxil   dxil-spirv in ~/scratch/deps, target/dxil2spv   tools/shader-prep.mjs
#   wine   portable Wine in ~/opt                          tools/oracle-wine, and dxc below
#   dxc    Microsoft's dxc in ~/tools/dxc                  fixtures/doom (its Linux build needs glibc 2.38,
#                                                          so the Windows build runs under Wine)
#   upx    UPX in ~/tools/upx                              tools/oracle-wine: unpacking a packed DLL before +snoop
#   reuse  the REUSE tool in ~/tools/reuse-venv             `reuse lint`: every file has a licence (LICENSING.md)
#   innoextract  innoextract 1.9 in ~/tools/innoextract     `pisco build`: a recipe's `extract = "innosetup"` (its
#                                                          static Linux build, checked by sha256)
# Environment: TOOLS (default ~/tools), DEPS (default ~/scratch/deps)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); T=${TOOLS:-$HOME/tools}; D=${DEPS:-$HOME/scratch/deps}; mkdir -p "$T" "$D"
WASI=wasi-sdk-34.0-x86_64-linux
MINGW=llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64
DXIL_SPIRV_COMMIT=7c048638ab59bcdbb1c43c19af2c4a057038435f
NAGA_VERSION=30.0.2
DXC_ZIP=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
. "$root/tools/oracle-wine/env.sh"
untar() { # url, the folder under $T it unpacks to
  [ -d "$T/$2" ] && { echo "have $T/$2"; return; }
  echo "fetching $1"; local tmp; tmp=$(mktemp -d "$T/.fetch.XXXXXX")
  curl -fsSL -o "$tmp/archive" "$1"; tar -xf "$tmp/archive" -C "$tmp"; rm "$tmp/archive"
  mv "$tmp/$2" "$T/$2"; rmdir "$tmp"
}
UPX=upx-5.3.0-amd64_linux
for step in ${*:-wasi mingw rust naga dxil wine dxc upx reuse innoextract}; do case $step in
  innoextract)
    if [ -x "$T/innoextract/bin/amd64/innoextract" ]; then echo "have $T/innoextract"; else
      tmp=$(mktemp -d "$T/.fetch.XXXXXX"); curl -fsSL -o "$tmp/ie.tar.xz" https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-linux.tar.xz
      echo "008efe5011476ccc4aae17c3e22038b5a1bc5c7aad2b9d4d869537bf3874d21f  $tmp/ie.tar.xz" | sha256sum -c --quiet
      tar -xf "$tmp/ie.tar.xz" -C "$tmp"; mv "$tmp/innoextract-1.9-linux" "$T/innoextract"; rm -rf "$tmp"
    fi ;;
  upx)   [ -x "$T/upx/upx" ] && echo "have $T/upx" || { untar "https://github.com/upx/upx/releases/download/v5.3.0/$UPX.tar.xz" "$UPX"; mv "$T/$UPX" "$T/upx"; } ;;
  reuse) [ -x "$T/reuse-venv/bin/reuse" ] && echo "have reuse" || { python3 -m venv "$T/reuse-venv"; "$T/reuse-venv/bin/pip" install -q reuse; } ;;
  wasi)  untar "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/$WASI.tar.gz" "$WASI" ;;
  mingw) untar "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/$MINGW.tar.xz" "$MINGW" ;;
  rust)  rustup target add x86_64-pc-windows-msvc i686-pc-windows-msvc ;;
  naga)  command -v naga >/dev/null && echo "have naga" || cargo install naga-cli --version "$NAGA_VERSION" --locked ;;
  dxil)
    if [ ! -f "$D/dxil-spirv/build/libdxil-spirv-c-shared.so" ]; then
      [ -d "$D/dxil-spirv/.git" ] || git clone -q https://github.com/HansKristian-Work/dxil-spirv "$D/dxil-spirv"
      git -C "$D/dxil-spirv" checkout -q "$DXIL_SPIRV_COMMIT"
      git -C "$D/dxil-spirv" submodule update -q --init --recursive
      cmake -S "$D/dxil-spirv" -B "$D/dxil-spirv/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
      cmake --build "$D/dxil-spirv/build" -j "$(nproc)" --target dxil-spirv-c-shared
    fi
    mkdir -p "$root/target"; "$root/tools/dxil-wgsl/build.sh" ;;
  wine)  "$root/tools/oracle-wine/setup.sh" ;;
  dxc)
    if [ ! -f "$T/dxc/bin/x64/dxc.exe" ]; then
      mkdir -p "$T/dxc"; curl -fsSL -o "$T/dxc/dxc.zip" "$DXC_ZIP"; unzip -q -o "$T/dxc/dxc.zip" -d "$T/dxc" || [ $? -eq 1 ]; rm "$T/dxc/dxc.zip"   # 1: a warning (its paths use backslashes)
    fi
    cat > "$T/dxc/dxc" <<WRAP
#!/bin/bash
# dxc: the Windows dxc under Wine. Arguments that are absolute paths get the Z: drive (dxc reads /x as an option).
. "$root/tools/oracle-wine/env.sh"
export WINEPREFIX=\${ORACLE_HOME}/prefix-tools
[ -d "\$WINEPREFIX" ] || { wineboot -i >/dev/null 2>&1; wineserver -w; }
args=(); for a in "\$@"; do case \$a in /*) [ -e "\$(dirname "\$a")" ] && a="Z:\$a" ;; esac; args+=("\$a"); done
exec wine "$T/dxc/bin/x64/dxc.exe" "\${args[@]}"
WRAP
    chmod +x "$T/dxc/dxc"; echo "have $T/dxc/dxc" ;;
  *) echo "unknown step: $step" >&2; exit 2 ;;
esac; done
