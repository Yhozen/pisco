#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile Microsoft's SimpleTriangleDesktop (Xbox-GDK-Samples) for Windows x64 from Linux.
# Needs: clang20 (clang-cl, lld-link, llvm-rc), an `xwin splat` tree, the Microsoft.GDK.Windows
# NuGet package, WinPixEventRuntime, and Linux dxc. Paths are overridable via env.
set -euo pipefail
W=${WINSDK:-$HOME/winsdk}
XWIN=${XWIN:-$W/xwin}
GDK=${GDK:-$W/microsoft.gdk.windows/native/261000/windows}
PIX=${PIX:-$W/pix}
DXC=${DXC:-$W/dxc/dxc-docker}  # Linux dxc needs glibc 2.38; dxc-docker runs it in Ubuntu 24.04
SAMPLES=${SAMPLES:-$W/samples}
LLVM=${LLVM:-/usr/lib64/llvm20/bin}
OUT=${1:-$(cd "$(dirname "$0")/.." && pwd)/out/gdk-samples/local}
SRC=$SAMPLES/Samples/IntroGraphics/SimpleTriangleDesktop
KITS=$SAMPLES/Kits
OBJ=$OUT/obj; mkdir -p "$OBJ"
# Windows headers are included with inconsistent case; give the few mismatches a link.
CASEFIX=$OBJ/casefix; mkdir -p "$CASEFIX"
for pair in Shlobj.h:ShlObj.h ${CASEFIX_EXTRA:-}; do
  ln -sf "$XWIN/sdk/include/um/${pair#*:}" "$CASEFIX/${pair%%:*}"
done

INC=(-imsvc "$CASEFIX" -imsvc "$XWIN/crt/include" -imsvc "$XWIN/sdk/include/ucrt" -imsvc "$XWIN/sdk/include/um"
     -imsvc "$XWIN/sdk/include/shared" -imsvc "$XWIN/sdk/include/winrt" -imsvc "$GDK/include"
     -imsvc "$PIX/Include/WinPixEventRuntime"
     -I "$SRC" -I "$KITS/ATGTelemetry/GDK" -I "$KITS/DirectXTK12/Inc" -I "$KITS/DirectXTK12/Src" -I "$KITS/ATGTK")
CFLAGS=(--target=x86_64-pc-windows-msvc /nologo /std:c++17 /EHsc /O2 /MD /GS /Zc:__cplusplus /Z7
        /D_GAMING_DESKTOP /DWIN32 /DNDEBUG /D_WINDOWS /DUNICODE /D_UNICODE
        -Wno-everything -fms-compatibility -fms-extensions)

cc() { "$LLVM/clang-cl" "${CFLAGS[@]}" "${INC[@]}" "$@"; }

SRCS=("$SRC/SimpleTriangleDesktop.cpp" "$SRC/Main.cpp" "$SRC/DeviceResources.cpp"
      "$KITS/ATGTelemetry/GDK/ATGTelemetry.cpp" "$KITS/ATGTK/StringUtil.cpp"
      "$KITS/DirectXTK12/Src/GamePad.cpp" "$KITS/DirectXTK12/Src/Keyboard.cpp" "$KITS/DirectXTK12/Src/Mouse.cpp"
      "$KITS/DirectXTK12/Src/GraphicsMemory.cpp" "$KITS/DirectXTK12/Src/LinearAllocator.cpp")
pids=()
for s in "${SRCS[@]}"; do
  case "$s" in *DirectXTK12*) pch=(/FIpch.h -I "$KITS/DirectXTK12/Src");; *) pch=(/FIpch.h);; esac
  cc /c "$s" /Fo"$OBJ/$(basename "${s%.cpp}").obj" &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done

"$DXC" -T vs_6_0 -E main -Fo "$OUT/VertexShader.cso" "$SRC/VertexShader.hlsl"
"$DXC" -T ps_6_0 -E main -Fo "$OUT/PixelShader.cso" "$SRC/PixelShader.hlsl"

iconv -f UTF-16 -t UTF-8 "$SRC/resource.rc" | sed '1s/^\xEF\xBB\xBF//' > "$SRC/.resource.utf8.rc"
"$LLVM/llvm-rc" /nologo /I "$SRC" /I "$XWIN/sdk/include/um" /I "$XWIN/sdk/include/shared" \
  /FO "$OBJ/resource.res" "$SRC/.resource.utf8.rc" || echo "warning: resource.rc skipped"

RES=(); [ -f "$OBJ/resource.res" ] && RES=("$OBJ/resource.res")
"$LLVM/lld-link" /nologo /OUT:"$OUT/SimpleTriangleDesktop.exe" /SUBSYSTEM:WINDOWS /MACHINE:X64 /DEBUG /OPT:REF /OPT:ICF \
  /LIBPATH:"$XWIN/crt/lib/x86_64" /LIBPATH:"$XWIN/sdk/lib/um/x86_64" /LIBPATH:"$XWIN/sdk/lib/ucrt/x86_64" \
  /LIBPATH:"$GDK/lib/x64" /LIBPATH:"$PIX/bin/x64" \
  /MANIFEST:EMBED /MANIFESTINPUT:"$SRC/settings.manifest" \
  "$OBJ"/*.obj "${RES[@]}" \
  libHttpClient.lib xgameruntime.lib gameinput.lib d3d12.lib dxgi.lib dxguid.lib uuid.lib kernel32.lib user32.lib \
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib runtimeobject.lib WinPixEventRuntime.lib

cp "$SRC/MicrosoftGameConfig.mgc" "$OUT/MicrosoftGame.config"
cp "$GDK/bin/x64/libHttpClient.dll" "$GDK/bin/x64/XCurl.dll" "$PIX/bin/x64/WinPixEventRuntime.dll" "$OUT/" 2>/dev/null || true
cp -r "$SRC/Assets" "$OUT/" 2>/dev/null || true
echo "built $OUT/SimpleTriangleDesktop.exe"
