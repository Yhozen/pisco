#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile Microsoft's SimplePBR (Xbox-GDK-Samples, Samples/Graphics/SimplePBR) for Windows x64
# (the desktop "Release|x64" configuration, _GAMING_DESKTOP) from Linux, the way build-gdk-sample.sh
# builds SimpleTriangleDesktop: clang-cl + lld-link against an `xwin splat` tree, the
# Microsoft.GDK.Windows NuGet package, WinPixEventRuntime, and Linux dxc.
#
# SimplePBR.vcxproj compiles the sample and ATGTK sources and links DirectXTK12.lib, built from
# Kits/DirectXTK12/DirectXTK12_GDKX_2022.vcxproj (Release|x64). DirectXTK12's shaders are compiled
# like `CompileShaders.cmd dxil` into $OBJ/dxtk12/Shaders/Compiled (never into the samples checkout).
# Everything goes under $OUT; nothing in $SAMPLES is written.
set -euo pipefail
W=${WINSDK:-$HOME/winsdk}
XWIN=${XWIN:-$W/xwin}
GDK=${GDK:-$W/microsoft.gdk.windows/native/261000/windows}
PIX=${PIX:-$W/pix}
DXC=${DXC:-$W/dxc/dxc-docker}  # Linux dxc needs glibc 2.38; dxc-docker runs it in Ubuntu 24.04 (paths under $HOME)
SAMPLES=${SAMPLES:-$W/samples}
LLVM=${LLVM:-/usr/lib64/llvm20/bin}
JOBS=${JOBS:-$(nproc)}
OUT=$(realpath -m "${1:-$(cd "$(dirname "$0")/.." && pwd)/out/gdk-samples/pbr-local}")
SRC=$SAMPLES/Samples/Graphics/SimplePBR
KITS=$SAMPLES/Kits
TK=$KITS/DirectXTK12
OBJ=$OUT/obj
mkdir -p "$OBJ/sample" "$OBJ/dxtk12/Shaders/Compiled" "$OBJ/dxtk12/Shaders/pdb"

# Windows headers are included with inconsistent case; give the few mismatches a link.
CASEFIX=$OBJ/casefix; mkdir -p "$CASEFIX"
for pair in Shlobj.h:ShlObj.h Shlwapi.h:shlwapi.h ${CASEFIX_EXTRA:-}; do
  ln -sf "$XWIN/sdk/include/um/${pair#*:}" "$CASEFIX/${pair%%:*}"
done

SYSINC=(-imsvc "$CASEFIX" -imsvc "$XWIN/crt/include" -imsvc "$XWIN/sdk/include/ucrt" -imsvc "$XWIN/sdk/include/um"
        -imsvc "$XWIN/sdk/include/shared" -imsvc "$XWIN/sdk/include/winrt" -imsvc "$GDK/include"
        -imsvc "$PIX/Include/WinPixEventRuntime")
# Release|x64 ClCompile of both projects: /O2 /Oi /Gy /fp:fast /GS /MD /std:c++17 /Zc:__cplusplus, Unicode.
BASE=(--target=x86_64-pc-windows-msvc /nologo /std:c++17 /EHsc /O2 /Oi /Gy /fp:fast /MD /GS /Zc:__cplusplus /Z7
      /DUNICODE /D_UNICODE -fms-compatibility -fms-extensions ${WARN:--Wno-everything})
# SimplePBR.vcxproj Release|x64
APP_FLAGS=(/D_GAMING_DESKTOP /D_WINDOWS /DWIN32 /DNDEBUG
           -I "$SRC" -I "$TK/Inc" -I "$KITS/ATGTK" -I "$KITS/ATGTelemetry/GDK" -I "$KITS/ATGTK/Skybox")
# DirectXTK12_GDKX_2022.vcxproj Release|x64 (generated shader headers first, ahead of the source tree)
TK_FLAGS=(/D_WIN32_WINNT=0x0A00 /DNDEBUG /DUSING_GAMEINPUT /D_GAMING_DESKTOP /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
          /DWIN32 /D_WINDOWS /D_LIB
          -I "$OBJ/dxtk12/Shaders/Compiled" -I "$TK/Inc" -I "$TK/Src")

# --- DirectXTK12 shaders: CompileShaders.cmd dxil ---------------------------------------------
# :CompileShaderdxil  -> dxc "%1.fx"   %FXCOPTS% /T%2_6_0 -HV 2021 /E%3 /Fh..\%1_%3.inc /Vn%1_%3
# :CompileComputeShaderdxil -> dxc "%1.hlsl" %FXCOPTS% /Tcs_6_0 -HV 2021 /E%2 /Fh..\%1_%2.inc /Vn%1_%2
# FXCOPTS=/nologo /WX /Ges /Zi /Zpc /Qstrip_reflect /Qstrip_debug. The list is read from the .cmd
# itself (the PC/DXIL part, i.e. everything before :skipxboxonly's Xbox-only HDR10 variants).
TKSH=$TK/Src/Shaders
FXCOPTS=(-nologo -WX -Ges -Zi -Zpc -Qstrip_reflect -Qstrip_debug -HV 2021)
jobs_file=$OBJ/dxtk12/Shaders/jobs.txt
tr -d '\r' < "$TKSH/CompileShaders.cmd" | awk '/^if %1\.==\. goto skipxboxonly/ {exit}
     /^call :CompileShader%1 /        {print $3".fx", $4"_6_0", $5, $3"_"$5}
     /^call :CompileComputeShader%1 / {print $3".hlsl", "cs_6_0", $4, $3"_"$4}' > "$jobs_file"
echo "DirectXTK12: $(wc -l < "$jobs_file") shaders"
# dxc-docker starts one container per call; run the whole list in one container of the same image
# and mounts instead (xargs -P for parallelism). Up-to-date headers are skipped.
SHOUT=$OBJ/dxtk12/Shaders
list=$SHOUT/dxc-commands.txt; : > "$list"
while read -r file prof entry name; do
  inc=$SHOUT/Compiled/$name.inc
  [ -s "$inc" ] && [ "$inc" -nt "$TKSH/$file" ] && continue
  echo "/dxc/bin/dxc $TKSH/$file ${FXCOPTS[*]} -T $prof -E $entry -Fh $inc -Fd $SHOUT/pdb/$name.pdb -Vn $name" >> "$list"
done < "$jobs_file"
if [ -s "$list" ]; then
  docker run --rm -u "$(id -u):$(id -g)" -v "$HOME:$HOME" -v "$W/dxc:/dxc:ro" ubuntu:24.04 \
    bash -c "xargs -P $JOBS -d '\\n' -I{} sh -c '{}' < '$list'" > "$SHOUT/dxc.log" 2>&1 \
    || { cat "$SHOUT/dxc.log"; echo "DirectXTK12 shader compilation failed" >&2; exit 1; }
fi
missing=0
while read -r _ _ _ name; do [ -s "$SHOUT/Compiled/$name.inc" ] || { echo "missing $name.inc" >&2; missing=1; }; done < "$jobs_file"
[ "$missing" = 0 ]

# --- Sample FxCompile items: ATGTK Skybox shaders -> $(OutDir)%(Filename).cso, which SkyboxEffect.cpp
# reads with DX::ReadData(L"SkyboxEffect_{VS,PS}.cso"). Release|x64: ShaderModel 6.0, entry main,
# EnableDebuggingInformation (/Zi) with /Fd beside it (kept in obj/).
for st in VS:vs_6_0 PS:ps_6_0; do
  "$DXC" -T "${st#*:}" -E main -Zi -Fd "$OBJ/SkyboxEffect_${st%%:*}.pdb" -Fo "$OUT/SkyboxEffect_${st%%:*}.cso" \
    "$KITS/ATGTK/Skybox/SkyboxEffect_${st%%:*}.hlsl"
done

# --- C++ ------------------------------------------------------------------------------------
TK_SRCS=("$TK"/Src/*.cpp "$TK"/Audio/*.cpp)
TK_SRCS=("${TK_SRCS[@]/*XboxDDSTextureLoader.cpp/}")   # ExcludedFromBuild for Release|x64
APP_SRCS=("$SRC/SimplePBR.cpp" "$SRC/Main.cpp" "$SRC/DeviceResources.cpp" "$SRC/pch.cpp"
          "$KITS/ATGTK/OrbitCamera.cpp" "$KITS/ATGTK/RenderTexture.cpp" "$KITS/ATGTK/StringUtil.cpp"
          "$KITS/ATGTK/Skybox/SkyboxEffect.cpp" "$KITS/ATGTelemetry/GDK/ATGTelemetry.cpp")
LOG=$OBJ/compile.log; : > "$LOG"
run_compile() {  # <dir> <src...> ; flags in FLAGS array
  local dir=$1; shift
  local pids=() fails=0 n=0
  for s in "$@"; do
    [ -n "$s" ] || continue
    local o="$dir/$(basename "${s%.cpp}").obj"
    ( "$LLVM/clang-cl" "${BASE[@]}" "${SYSINC[@]}" "${FLAGS[@]}" /c "$s" /Fo"$o" >"$o.log" 2>&1 ) &
    pids+=($!); n=$((n+1))
    if (( n % JOBS == 0 )); then for p in "${pids[@]}"; do wait "$p" || fails=$((fails+1)); done; pids=(); fi
  done
  for p in "${pids[@]}"; do wait "$p" || fails=$((fails+1)); done
  cat "$dir"/*.obj.log >> "$LOG" 2>/dev/null || true
  return $fails
}
mkdir -p "$OBJ/dxtk12/obj"
FLAGS=("${TK_FLAGS[@]}"); run_compile "$OBJ/dxtk12/obj" "${TK_SRCS[@]}" || { cat "$LOG"; echo "DirectXTK12 compile failed" >&2; exit 1; }
"$LLVM/llvm-lib" /nologo /OUT:"$OBJ/DirectXTK12.lib" "$OBJ"/dxtk12/obj/*.obj
FLAGS=("${APP_FLAGS[@]}"); run_compile "$OBJ/sample" "${APP_SRCS[@]}" || { cat "$LOG"; echo "SimplePBR compile failed" >&2; exit 1; }
echo "compiler diagnostics: $(grep -c ': warning:' "$LOG" || true) warnings, $(grep -c ': error:' "$LOG" || true) errors (log: $LOG)"

# Manifest: the vcxproj generates one with EnableDpiAwareness=PerMonitorHighDPIAware.
cat > "$OBJ/dpi.manifest" <<'EOF'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
    </windowsSettings>
  </application>
</assembly>
EOF

# Link: SimplePBR.vcxproj Release|x64 AdditionalDependencies (+ DirectXTK12.lib, WinPixEventRuntime.lib).
"$LLVM/lld-link" /nologo /OUT:"$OUT/SimplePBR.exe" /SUBSYSTEM:WINDOWS /MACHINE:X64 /DEBUG /OPT:REF /OPT:ICF \
  /LIBPATH:"$XWIN/crt/lib/x86_64" /LIBPATH:"$XWIN/sdk/lib/um/x86_64" /LIBPATH:"$XWIN/sdk/lib/ucrt/x86_64" \
  /LIBPATH:"$GDK/lib/x64" /LIBPATH:"$PIX/bin/x64" \
  /MANIFEST:EMBED /MANIFESTINPUT:"$OBJ/dpi.manifest" \
  "$OBJ"/sample/*.obj "$OBJ/DirectXTK12.lib" \
  Microsoft.Xbox.Services.143.C.lib PlayFabCore.lib PlayFabServices.lib libHttpClient.lib XCurl.lib appnotify.lib \
  winhttp.lib crypt32.lib uuid.lib xgameruntime.lib gameinput.lib d3d12.lib dxgi.lib dxguid.lib kernel32.lib \
  user32.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib runtimeobject.lib WinPixEventRuntime.lib \
  2>&1 | tee "$OBJ/link.log"
[ "${PIPESTATUS[0]}" = 0 ]

# --- Runtime files (Release|x64 DestinationFolders: $(OutDir)Media\...) -----------------------
cp "$SRC/MicrosoftGameConfig.mgc" "$OUT/MicrosoftGame.config"
mkdir -p "$OUT/Assets"; cp "$SRC"/Assets/*.png "$OUT/Assets/"
copy_media() { local rel=$1; mkdir -p "$OUT/$(dirname "$rel")"; cp "$SAMPLES/$rel" "$OUT/$rel"; }
for f in SegoeUI_18 SegoeUI_36 XboxOneControllerLegend XboxOneControllerLegendSmall; do copy_media "Media/Fonts/$f.spritefont"; done
for f in Stonewall_Ref_irradiance Stonewall_Ref_radiance; do copy_media "Media/HDR/$f.dds"; done
copy_media Media/PBR/XboxOrb/XboxOrb.sdkmesh
for m in Floor ToyRobot WoodBlocks; do copy_media "Media/PBR/ToyRobot/$m/$m.sdkmesh"; done
for f in Floor/Floor_BaseColor.dds Floor/Floor_Normal.DDS Floor/Floor_RMA.DDS \
         ToyRobot/ToyRobot_BaseColor.dds ToyRobot/Toyrobot_Normal.DDS ToyRobot/Toyrobot_RMA.DDS \
         WoodBlocks/WoodBlocks_BaseColor.dds WoodBlocks/WoodBlocks_Normal.DDS WoodBlocks/WoodBlocks_RMA.DDS; do
  copy_media "Media/PBR/ToyRobot/$f"
done
# DLLs next to the exe, only those it imports (ReferenceCopyLocalPaths + the PIX NuGet targets).
imports=$("$LLVM/llvm-objdump" -p "$OUT/SimplePBR.exe" | awk '/DLL Name:/ {print tolower($3)}')
for dll in "$GDK/bin/x64/libHttpClient.dll" "$GDK/bin/x64/XCurl.dll" "$PIX/bin/x64/WinPixEventRuntime.dll"; do
  name=$(basename "$dll" | tr A-Z a-z)
  if grep -qx "$name" <<<"$imports"; then cp "$dll" "$OUT/"; fi
done
# SimplePBR.vcxproj copies XCurl.dll beside libHttpClient.dll for x64 (ReferenceCopyLocalPaths).
[ -f "$OUT/libHttpClient.dll" ] && cp "$GDK/bin/x64/XCurl.dll" "$OUT/"
echo "built $OUT/SimplePBR.exe"
