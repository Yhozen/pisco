#!/usr/bin/env bash
# Build Microsoft's SimpleTriangleDesktop (Xbox-GDK-Samples) for Windows x64 Release with the real
# MSVC toolchain (cl.exe, rc.exe, link.exe) running under Wine on Ubuntu 24.04 x86_64.
# Run on a throwaway VM (written for a boat.dev sandbox). Downloading MSVC/SDK/GDK means accepting
# Microsoft's licence terms. Usage:
#   build-gdk-sample-msvc-wine.sh            # phase A: MSVC under Wine -> $W/out-msvc
#   build-gdk-sample-msvc-wine.sh clang      # phase B: tools/build-gdk-sample.sh (clang-cl) -> $W/out-clang
#                                            #   needs build-gdk-sample.sh next to this script
# Timings go to $W/timings.txt. msvc-wine's msbuild wrapper is not used: MSBuild.exe needs .NET
# Framework (Wine Mono / dotnet48), which this prefix skips; it exited 255 with no output.
set -euo pipefail
W=${W:-$HOME/gdk}
mkdir -p "$W"; cd "$W"
HERE=$(cd "$(dirname "$0")" && pwd)
SAMPLES_REV=b1e77b6ac10f95b07296025fda171ca2ddc42dbe   # Xbox-GDK-Samples commit we built locally
GDK_VER=2610.0.9661
PIX_VER=1.0.240308001
DXC_URL=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz
export DEBIAN_FRONTEND=noninteractive WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="

T=$W/timings.txt
phase() { # phase <name> <function>
  local s e; s=$(date +%s.%N); echo "=== $1"; "$2"; e=$(date +%s.%N)
  awk -v n="$1" -v s="$s" -v e="$e" 'BEGIN{printf "%-28s %8.1f s\n", n, e-s}' | tee -a "$T"
}

# ---- shared downloads (GDK, PIX, Linux dxc, samples) -------------------------------------------
nuget() { # nuget <id> <ver> <dir>
  [ -d "$3" ] && return
  curl -fsSL -o "$3.nupkg" "https://api.nuget.org/v3-flatcontainer/$1/$2/$1.$2.nupkg"
  mkdir -p "$3"; unzip -q -o "$3.nupkg" -d "$3"
}
downloads() {
  nuget microsoft.gdk.windows "$GDK_VER" "$W/microsoft.gdk.windows"
  nuget winpixeventruntime "$PIX_VER" "$W/pix"
  if [ ! -x "$W/dxc/bin/dxc" ]; then
    mkdir -p "$W/dxc"; curl -fsSL "$DXC_URL" | tar -xz -C "$W/dxc"; chmod +x "$W/dxc/bin/dxc"
  fi
  if [ ! -d "$W/samples" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/microsoft/Xbox-GDK-Samples.git "$W/samples"
    git -C "$W/samples" sparse-checkout set --no-cone /Samples/IntroGraphics/SimpleTriangleDesktop/ \
      /Kits/ATGTelemetry/ /Kits/DirectXTK12/ /Kits/ATGTK/
    git -C "$W/samples" checkout -q "$SAMPLES_REV"
  fi
}
GDK=$W/microsoft.gdk.windows/native/261000/windows
PIX=$W/pix
SRC_REL=samples/Samples/IntroGraphics/SimpleTriangleDesktop
KITS_REL=samples/Kits

# ---- phase A: MSVC under Wine ------------------------------------------------------------------
wine_install() {
  sudo apt-get update -qq
  # Ubuntu's Wine 9 (64-bit only) is enough for the command-line tools; msvc-wine's own CI uses it.
  sudo apt-get install -y -qq --no-install-recommends wine wine64 python3 msitools ca-certificates \
    winbind unzip git >/dev/null
  wine64 wineboot -i >/dev/null 2>&1 || wine wineboot -i >/dev/null 2>&1
  wine --version
}
msvc_install() {
  [ -d "$W/msvc-wine" ] || git clone -q https://github.com/mstorsjo/msvc-wine "$W/msvc-wine"
  # x64 host + x64 target only; the default picks the newest MSVC and Windows SDK.
  python3 "$W/msvc-wine/vsdownload.py" --accept-license --host-arch x64 --architecture x64 \
    --dest "$W/msvc" >"$W/vsdownload.log" 2>&1 || { tail -30 "$W/vsdownload.log"; return 1; }
  "$W/msvc-wine/install.sh" "$W/msvc" >"$W/install.log" 2>&1 || { tail -30 "$W/install.log"; return 1; }
  ls "$W/msvc/bin/x64"
}
msvc_build() {
  local B=$W/msvc/bin/x64 OUT=$W/out-msvc
  rm -rf "$OUT"; mkdir -p "$OUT/obj"
  # The wrappers run under Wine; relative paths with '/' are read the same by cl, rc and link.
  local INC=(-I"$SRC_REL" -I"$KITS_REL/ATGTelemetry/GDK" -I"$KITS_REL/DirectXTK12/Inc"
             -I"$KITS_REL/DirectXTK12/Src" -I"$KITS_REL/ATGTK"
             -I"${GDK#$W/}/include" -I"pix/Include/WinPixEventRuntime")
  local SRCS=("$SRC_REL/SimpleTriangleDesktop.cpp" "$SRC_REL/Main.cpp" "$SRC_REL/DeviceResources.cpp"
              "$KITS_REL/ATGTelemetry/GDK/ATGTelemetry.cpp" "$KITS_REL/ATGTK/StringUtil.cpp"
              "$KITS_REL/DirectXTK12/Src/GamePad.cpp" "$KITS_REL/DirectXTK12/Src/Keyboard.cpp"
              "$KITS_REL/DirectXTK12/Src/Mouse.cpp" "$KITS_REL/DirectXTK12/Src/GraphicsMemory.cpp"
              "$KITS_REL/DirectXTK12/Src/LinearAllocator.cpp")
  # /FIpch.h resolves next to each source first, so DirectXTK12 files get DirectXTK12/Src/pch.h.
  "$B/cl" /nologo /MP /c /O2 /MD /GS /EHsc /std:c++17 /Zc:__cplusplus /Z7 /W3 \
    /D_GAMING_DESKTOP /DWIN32 /DNDEBUG /D_WINDOWS /DUNICODE /D_UNICODE \
    /FIpch.h "${INC[@]}" /Fo"out-msvc/obj/" "${SRCS[@]}"

  # Shaders: the Windows SDK's dxc.exe under Wine, else the Linux dxc release.
  local DXCEXE; DXCEXE=$(find "$W/msvc/kits/10/bin" -path '*x64/dxc.exe' | sort | tail -1 || true)
  if [ -n "$DXCEXE" ] && wine "$DXCEXE" -T vs_6_0 -E main -Fo out-msvc/VertexShader.cso "$SRC_REL/VertexShader.hlsl" &&
     wine "$DXCEXE" -T ps_6_0 -E main -Fo out-msvc/PixelShader.cso "$SRC_REL/PixelShader.hlsl"; then
    echo "shaders: Windows SDK dxc.exe under Wine ($DXCEXE)" | tee "$OUT/shader-compiler.txt"
  else
    "$W/dxc/bin/dxc" -T vs_6_0 -E main -Fo "$OUT/VertexShader.cso" "$SRC_REL/VertexShader.hlsl"
    "$W/dxc/bin/dxc" -T ps_6_0 -E main -Fo "$OUT/PixelShader.cso" "$SRC_REL/PixelShader.hlsl"
    echo "shaders: Linux dxc v1.9.2609" | tee "$OUT/shader-compiler.txt"
  fi

  # rc.exe reads the UTF-16 resource.rc as is.
  "$B/rc" /nologo /I"$SRC_REL" /fo out-msvc/obj/resource.res "$SRC_REL/resource.rc"

  "$B/link" /nologo /OUT:out-msvc/SimpleTriangleDesktop.exe /SUBSYSTEM:WINDOWS /MACHINE:X64 \
    /DEBUG /OPT:REF /OPT:ICF /LIBPATH:"${GDK#$W/}/lib/x64" /LIBPATH:pix/bin/x64 \
    /MANIFEST:EMBED /MANIFESTINPUT:"$SRC_REL/settings.manifest" \
    out-msvc/obj/*.obj out-msvc/obj/resource.res \
    libHttpClient.lib xgameruntime.lib gameinput.lib d3d12.lib dxgi.lib dxguid.lib uuid.lib kernel32.lib \
    user32.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib runtimeobject.lib \
    WinPixEventRuntime.lib

  cp "$SRC_REL/MicrosoftGameConfig.mgc" "$OUT/MicrosoftGame.config"
  cp "$GDK/bin/x64/libHttpClient.dll" "$GDK/bin/x64/XCurl.dll" "$PIX/bin/x64/WinPixEventRuntime.dll" "$OUT/"
  cp -r "$SRC_REL/Assets" "$OUT/"
  "$B/cl" 2>&1 | grep -m1 Version | tee "$OUT/compiler-version.txt"
  ls -la "$OUT"
}

# ---- phase B: the clang-cl cross build (tools/build-gdk-sample.sh) -----------------------------
clang_install() {
  sudo apt-get update -qq
  sudo apt-get install -y -qq --no-install-recommends clang-20 lld-20 llvm-20 unzip git >/dev/null
  if [ ! -x "$W/xwin-bin/xwin" ]; then
    local url; url=$(curl -fsSL https://api.github.com/repos/Jake-Shadle/xwin/releases/latest |
      grep -o 'https://[^"]*x86_64-unknown-linux-musl.tar.gz' | head -1)
    mkdir -p "$W/xwin-bin"; curl -fsSL "$url" | tar -xz -C "$W/xwin-bin" --strip-components=1
  fi
  # Ubuntu's clang-20 package ships no clang-cl name; clang picks its cl driver from argv[0].
  mkdir -p "$W/llvm-bin"
  ln -sf /usr/lib/llvm-20/bin/clang "$W/llvm-bin/clang-cl"
  ln -sf /usr/lib/llvm-20/bin/lld-link "$W/llvm-bin/lld-link"
  ln -sf /usr/lib/llvm-20/bin/llvm-rc "$W/llvm-bin/llvm-rc"
}
xwin_splat() {
  [ -d "$W/xwin" ] || "$W/xwin-bin/xwin" --accept-license --arch x86_64 --cache-dir "$W/xwin-cache" \
    splat --output "$W/xwin" >"$W/xwin.log" 2>&1
}
clang_build() {
  rm -rf "$W/out-clang"
  WINSDK=$W XWIN=$W/xwin GDK=$GDK PIX=$PIX DXC=$W/dxc/bin/dxc SAMPLES=$W/samples LLVM=$W/llvm-bin \
    bash "$HERE/build-gdk-sample.sh" "$W/out-clang"
  ls -la "$W/out-clang"
}

if [ "${1:-msvc}" = clang ]; then
  phase "B clang/lld/xwin install" clang_install
  phase "B GDK/PIX/dxc/samples" downloads
  phase "B xwin splat" xwin_splat
  phase "B compile+link" clang_build
else
  phase "A wine install" wine_install
  phase "A MSVC download+install" msvc_install
  phase "A GDK/PIX/dxc/samples" downloads
  phase "A compile+link" msvc_build
fi
