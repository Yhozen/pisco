#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/doom/build.sh: our console Doom. Fetches doomgeneric (pinned) and Freedoom into out/doom, builds
#   out/doom/game/doom.exe   the title (x86-64 PE, llvm-mingw), with freedoom1.wad and shaders/ beside it
#   out/doom/native/doom     the same doomgeneric for Linux, printing the same frame hashes (the reference)
# Environment: MINGW (llvm-mingw folder), DXC (dxc command, default ~/winsdk/dxc/dxc-docker or ~/tools/dxc/dxc), OUT (default out/doom)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
out=${OUT:-$root/out/doom}; mkdir -p "$out"
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}
DXC=${DXC:-$([ -x "$HOME/winsdk/dxc/dxc-docker" ] && echo "$HOME/winsdk/dxc/dxc-docker" || echo "$HOME/tools/dxc/dxc")}   # tools/setup-toolchain.sh dxc
# the reference is built by clang too: the title's compiler family, so both read Doom's undefined behaviour alike
NCC=${NCC:-$(command -v clang || echo /usr/lib64/llvm20/bin/clang)}
DG_COMMIT=e257a09527062bfb986b6e5e0780839b6cbd5b71
FREEDOOM=https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
[ -d "$out/doomgeneric/.git" ] || git clone -q https://github.com/ozkl/doomgeneric.git "$out/doomgeneric"
git -C "$out/doomgeneric" checkout -q "$DG_COMMIT"
if [ ! -f "$out/freedoom1.wad" ]; then
  [ -f "$out/freedoom.zip" ] || curl -sSL -o "$out/freedoom.zip" "$FREEDOOM"
  unzip -o -j -q "$out/freedoom.zip" 'freedoom-0.13.0/freedoom1.wad' -d "$out"
fi
# sources, with one change: the configuration (and saves) go where the platform says
src=$out/src; rm -rf "$src"; mkdir -p "$src"; cp "$out"/doomgeneric/doomgeneric/*.[ch] "$src/"
python3 - "$src/m_config.c" <<'EOF'
import sys, re
p = sys.argv[1]; s = open(p).read()
s2 = re.sub(r"static char \*GetDefaultConfigDir\(void\)\n\{.*?\n\}", "extern char *dg_save_dir;\nstatic char *GetDefaultConfigDir(void)\n{\n    return M_StringDuplicate(dg_save_dir);\n}", s, count=1, flags=re.S)
assert s2 != s, "m_config.c: GetDefaultConfigDir not found"
open(p, "w").write(s2)
EOF
# and no SDL: the sound module is the platform's (XAudio2)
sed -i 's|#include <SDL_mixer.h>|/* SDL_mixer: not used, the platform has the sound module */|' "$src/i_sound.c"
DOOM_SRC="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric"
# shaders: DXIL with the root signature inside
gen=$out/gen; mkdir -p "$gen"
cp "$here/blit.hlsl" "$gen/blit.hlsl"
"$DXC" -T vs_6_0 -E vs_main "$gen/blit.hlsl" -Fh "$gen/vs_blit.h" -Vn g_vs_main >/dev/null
"$DXC" -T ps_6_0 -E ps_main "$gen/blit.hlsl" -Fh "$gen/ps_blit.h" -Vn g_ps_main >/dev/null
# import libraries for the console's DLLs
B=$MINGW/bin
for d in d3d12_xs:D3D12XboxCreateDevice gameinput:GameInputCreate xaudio2_9:XAudio2Create "xgameruntime:XGameRuntimeInitialize XAsyncGetStatus XUserAddAsync XUserAddResult XGameSaveFilesGetFolderWithUiAsync XGameSaveFilesGetFolderWithUiResult"; do
  dll=${d%%:*}; { echo "LIBRARY $dll.dll"; echo EXPORTS; for f in ${d#*:}; do echo "$f"; done; } > "$gen/$dll.def"
  "$B/llvm-dlltool" -m i386:x86-64 -d "$gen/$dll.def" -l "$gen/lib$dll.a"
done
# the title
obj=$out/obj; mkdir -p "$obj" "$out/game"
CF="-O2 -w -DFEATURE_SOUND -DNORMALUNIX=0 -I$src -I$gen -I$here"
for f in $DOOM_SRC; do echo "$f"; done | xargs -P "$(nproc)" -I{} "$B/x86_64-w64-mingw32-clang" $CF -c "$src/{}.c" -o "$obj/{}.o"
"$B/x86_64-w64-mingw32-clang" $CF -c "$here/doom_xbox.c" -o "$obj/doom_xbox.o"
"$B/x86_64-w64-mingw32-clang" -o "$out/game/doom.exe" "$obj"/*.o -L"$gen" -ld3d12_xs -lgameinput -lxaudio2_9 -lxgameruntime
cp "$out/freedoom1.wad" "$out/game/"
node "$root/tools/shader-prep.mjs" "$out/game/doom.exe"
# the reference: the same doomgeneric on Linux, the same frame hashes
nat=$out/native; mkdir -p "$nat/obj"
for f in $DOOM_SRC; do echo "$f"; done | xargs -P "$(nproc)" -I{} "$NCC" -O2 -w -I"$src" -c "$src/{}.c" -o "$nat/obj/{}.o"
"$NCC" -O2 -w -I"$src" -c "$here/doom_native.c" -o "$nat/obj/doom_native.o"
"$NCC" -o "$nat/doom" "$nat"/obj/*.o -lm
echo "built $out/game/doom.exe and $nat/doom"
