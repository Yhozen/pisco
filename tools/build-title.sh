#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-title.sh <image.exe> <out dir>: lift a title, compile it with the runtime into <out dir>/title.wasm
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS (parallel compiles), OPT (default -O2)
# A 32-bit title (the lifter says so in <out dir>/title.env) gets the runtime built for a 32-bit guest
# (-DGUEST_BITS=32), the stdcall argument sizes of its imports (tools/stdcall-sizes.sh, needs llvm-mingw's
# import libraries), and its own memory layout: see "the memory layout" below.
# PROFILE picks the runtime's parts (default: by the title's width). `console`: rt/*.c, the Xbox and D3D12 side.
# `win32`: Windows desktop titles: the core of rt/ plus rt/win32/ (user32, gdi32, the registry, Direct3D 9, ...),
# without the console's libraries. A title links only its profile (LICENSING.md).
# RECIPE=<title.toml>: the title's recipe (CPU features, registry values); tools/recipe-gen.py reads it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); exe=$1; out=$2; mkdir -p "$out"
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
JOBS=${JOBS:-$(nproc)}
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
"$root/target/release/lifter" "$exe" "$out/title.c" --split "${SPLIT:-8}" $([ -n "${RECIPE:-}" ] && python3 "$root/tools/recipe-gen.py" --lifter-options "$RECIPE")
GUEST_BITS=64; IMAGE_LOW=0; IMAGE_HIGH=0; FIXED_BASE=0
[ -f "$out/title.env" ] && . "$out/title.env"
PROFILE=${PROFILE:-$([ "$GUEST_BITS" = 32 ] && echo win32 || echo console)}
case $PROFILE in
  win32) RT_FILES=$(ls "$root"/rt/*.c "$root"/rt/win32/*.c | grep -v -E '/rt/(gdk|d3d12|dxgi|dstorage|xaudio2|audio_codec)\.c$') ;;
  console) RT_FILES=$(ls "$root"/rt/*.c) ;;
  *) echo "build-title.sh: unknown PROFILE $PROFILE" >&2; exit 2 ;;
esac
python3 "$root/tools/recipe-gen.py" ${RECIPE:+"$RECIPE"} > "$out/recipe.gen.c"
# the import table: every IMP(dll, name) the runtime defines
grep -ho 'IMP([a-z0-9_]*, *[A-Za-z0-9_]*)' $RT_FILES | sort -u | sed -E 's/IMP\(([a-z0-9_]+), *([A-Za-z0-9_]+)\)/\1 \2/' > "$out/imports.txt"
{ echo '#include "rt.h"'
  awk '{print "void imp_"$1"_"$2"(Cpu*);"}' "$out/imports.txt"
  echo 'typedef struct { const char *name; ImportFn fn; } ImportDef;'
  echo 'const ImportDef rt_import_table[] = {'
  awk '{print "  {\""$1".dll!"$2"\", imp_"$1"_"$2"},"}' "$out/imports.txt"
  echo '};'
  echo "const unsigned rt_import_table_count = sizeof rt_import_table / sizeof rt_import_table[0];"
  # 32-bit: what each import removes from the stack, for the runtime's and the title's own imports
  echo 'typedef struct { const char *name; int bytes; } StdcallDef;'
  echo 'const StdcallDef rt_stdcall_table[] = {'
  if [ "$GUEST_BITS" = 32 ]; then
    { awk '{print $1".dll!"$2}' "$out/imports.txt"; grep -ho '"[^"]*![^"]*",' "$out"/title*.c | tr -d '",'; } | "$root/tools/stdcall-sizes.sh" | awk '{print "  {\""$1"\", "$2"},"}'
  fi
  echo '  {0, 0}};'
  echo "const unsigned rt_stdcall_table_count = sizeof rt_stdcall_table / sizeof rt_stdcall_table[0] - 1;"
} > "$out/imports.gen.c"
CF="$TARGET -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=$GUEST_BITS"
# The memory layout. By default the runtime's stack, data and heap come first and the images sit above them
# (the lifter moves them there). An image that has to load low (a 32-bit title's exe at 0x400000, often with no
# relocations) gets a gap instead: the runtime's stack below it, the runtime's data and heap above it.
LAYOUT="--stack-first,-z,stack-size=8388608"
if [ "$GUEST_BITS" = 32 ]; then
  [ $((IMAGE_LOW)) -ge $((0x100000)) ] || { echo "build-title.sh: a 32-bit image at $IMAGE_LOW leaves no room for the runtime's stack below it" >&2; exit 1; }
  stack=$(( IMAGE_LOW < 0x800000 ? IMAGE_LOW : 0x800000 )); stack=$(( stack & ~0xFFFF ))
  LAYOUT="--stack-first,-z,stack-size=$stack,--global-base=$(( (IMAGE_HIGH + 0xFFFFF) & ~0xFFFFF ))"
fi
# lifted code: freestanding, no libc
for f in "$out"/title*.c; do [ "$f.o" -nt "$f" ] && [ "$f.o" -nt "$root/rt/cpu.h" ] || echo "$f"; done | xargs -r -P "$JOBS" -I{} sh -c '"$0"/clang $1 -ffreestanding -c "{}" -o "{}.o"' "$W" "$CF"
# XMA2 / Opus decoding: FFmpeg's decoders when tools/build-ffmpeg-audio.sh has built them (else silent voices)
FF="$root/out/ffmpeg"; AUDIO_LIBS=""
if [ -f "$FF/lib/libavcodec.a" ]; then AUDIO_LIBS="$FF/lib/libavcodec.a $FF/lib/libswresample.a $FF/lib/libavutil.a -lm"; fi
[ "$PROFILE" = console ] || AUDIO_LIBS=""
rm -f "$out"/rt_*.o   # another profile's objects must not be linked
for f in $RT_FILES; do b=$(basename "$f" .c); X=""; [ "$b" = audio_codec ] && [ -n "$AUDIO_LIBS" ] && X="-DSW_AUDIO_CODECS -I$FF/include"
  "$W/clang" $CF $X -c "$f" -o "$out/rt_$b.o" & done
"$W/clang" $CF -c "$out/imports.gen.c" -o "$out/imports.gen.o"
"$W/clang" $CF -c "$out/recipe.gen.c" -o "$out/recipe.gen.o"
wait
"$W/clang" $TARGET -pthread "$out"/title*.c.o "$out"/rt_*.o "$out/imports.gen.o" "$out/recipe.gen.o" $AUDIO_LIBS -o "$out/title.wasm" -mllvm -wasm-enable-sjlj -lsetjmp \
  -Wl,--import-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,--export=wasi_thread_start,$LAYOUT \
  -Wl,--export=__heap_base
