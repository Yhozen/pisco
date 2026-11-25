#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-title.sh <image.exe> <out dir>: lift a title, compile it with the runtime into <out dir>/title.wasm
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS (parallel compiles), OPT (default -O2)
# A 32-bit title (the lifter says so in <out dir>/title.env) gets the runtime built for a 32-bit guest
# (-DGUEST_BITS=32), the stdcall argument sizes of its imports (tools/stdcall-sizes.sh, needs llvm-mingw's
# import libraries), and its own memory layout: see "the memory layout" below.
# BASE picks the runtime (a base of bases/, which lists its files; `pisco sources` reads them): windows-x86,
# windows-x64 or xbox-series; default: windows-x86 for a 32-bit title, windows-x64 for a 64-bit one. A title links
# only its base's files and every library of the base (LICENSING.md); with RECIPE, the recipe's base and libraries.
# RECIPE=<title.toml>: the title's recipe (CPU features, registry values); tools/recipe-gen.py reads it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); exe=$1; out=$2; mkdir -p "$out"
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
JOBS=${JOBS:-$(nproc)}
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
# DLL_DIR=<folder>: the DLLs the title imports from its system folder (an image's C:\WINDOWS\system32), lifted with it
"$root/target/release/lifter" "$exe" "$out/title.c" --split "${SPLIT:-8}" ${DLL_DIR:+--dll-dir "$DLL_DIR=c:/WINDOWS/system32"} $([ -n "${RECIPE:-}" ] && python3 "$root/tools/recipe-gen.py" --lifter-options "$RECIPE")
GUEST_BITS=64; IMAGE_LOW=0; IMAGE_HIGH=0; FIXED_BASE=0
[ -f "$out/title.env" ] && . "$out/title.env"
[ -z "${PROFILE:-}" ] || { echo "build-title.sh: PROFILE is gone; BASE=windows-x86, windows-x64 or xbox-series" >&2; exit 2; }
PISCO=$root/target/release/pisco; [ -x "$PISCO" ] || { echo "build-title.sh: $PISCO is not built (cargo build --release -p pisco)" >&2; exit 2; }
if [ -n "${RECIPE:-}" ] && [ -z "${BASE:-}" ]; then RT_FILES=$("$PISCO" sources "$(dirname "$RECIPE")")
else BASE=${BASE:-$([ "$GUEST_BITS" = 32 ] && echo windows-x86 || echo windows-x64)}; RT_FILES=$("$PISCO" sources "base/$BASE:1" --all-libraries); fi
RT_FILES=$(for f in $RT_FILES; do echo "$root/$f"; done)
python3 "$root/tools/recipe-gen.py" ${RECIPE:+"$RECIPE"} > "$out/recipe.gen.c"
# the import table: every IMP(dll, name) the runtime defines, and for a 32-bit title the stdcall sizes
"$root/tools/gen-imports.sh" "$GUEST_BITS" "$out" "$out/imports.gen.c" $RT_FILES
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
echo "$RT_FILES" | grep -q "/audio_codec\.c$" || AUDIO_LIBS=""
rm -f "$out"/rt_*.o   # another base's objects must not be linked
for f in $RT_FILES; do b=$(echo "${f#$root/rt/}" | tr / _ | sed "s/\.c$//"); X=""; [ "$(basename "$f" .c)" = audio_codec ] && [ -n "$AUDIO_LIBS" ] && X="-DSW_AUDIO_CODECS -I$FF/include"
  "$W/clang" $CF $X -c "$f" -o "$out/rt_$b.o" & done
"$W/clang" $CF -c "$out/imports.gen.c" -o "$out/imports.gen.o"
"$W/clang" $CF -c "$out/recipe.gen.c" -o "$out/recipe.gen.o"
wait
"$W/clang" $TARGET -pthread "$out"/title*.c.o "$out"/rt_*.o "$out/imports.gen.o" "$out/recipe.gen.o" $AUDIO_LIBS -o "$out/title.wasm" -mllvm -wasm-enable-sjlj -lsetjmp \
  -Wl,--import-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,--export=wasi_thread_start,$LAYOUT \
  -Wl,--export=__heap_base
