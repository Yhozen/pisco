#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-title.sh <image.exe> <out dir>: lift a title, compile it with the runtime into <out dir>/title.wasm
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS (parallel compiles), OPT (default -O2)
# A 32-bit title (the lifter says so in <out dir>/title.env) gets the runtime built for a 32-bit guest
# (-DGUEST_BITS=32), the stdcall argument sizes of its imports (tools/stdcall-sizes.sh, needs llvm-mingw's
# import libraries), and its own memory layout: see "the memory layout" below.
# BASE picks the runtime (a base of bases/, which lists its files; `pisco sources` reads them): windows-x86,
# windows-x64, xbox-series, linux-x64, ps5 or microvm-x64; default: linux-x64 for an ELF (the lifter says so too: OS=linux), ps5 for a
# PS5 executable (OS=ps5), microvm-x64 for a machine's image such as a kernel (OS=microvm),
# windows-x86 for a 32-bit title, windows-x64 for another 64-bit one. A title links
# only its base's files and every library of the base (LICENSING.md); with RECIPE, the recipe's base and libraries.
# RECIPE=<title.toml>: the title's recipe (CPU features, registry values); tools/recipe-gen.py reads it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); exe=$1; out=$2; mkdir -p "$out"
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
JOBS=${JOBS:-$(nproc)}
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
# DLL_DIR=<folder>: the DLLs the title imports from its system folder (an image's C:\WINDOWS\system32), lifted with it
"$root/target/release/lifter" "$exe" "$out/title.c" --split "${SPLIT:-8}" ${TRACE_PC:+--trace-pc} ${DLL_DIR:+--dll-dir "$DLL_DIR=c:/WINDOWS/system32"} $([ -n "${RECIPE:-}" ] && python3 "$root/tools/recipe-gen.py" --lifter-options "$RECIPE")
GUEST_BITS=64; IMAGE_LOW=0; IMAGE_HIGH=0; FIXED_BASE=0; OS=windows
[ -f "$out/title.env" ] && . "$out/title.env"
[ -z "${PROFILE:-}" ] || { echo "build-title.sh: PROFILE is gone; BASE=windows-x86, windows-x64, xbox-series or linux-x64" >&2; exit 2; }
PISCO=$root/target/release/pisco; [ -x "$PISCO" ] || { echo "build-title.sh: $PISCO is not built (cargo build --release -p pisco)" >&2; exit 2; }
if [ -n "${RECIPE:-}" ] && [ -z "${BASE:-}" ]; then RT_FILES=$("$PISCO" sources "$(dirname "$RECIPE")")
else BASE=${BASE:-$([ "$OS" = linux ] && echo linux-x64 || { [ "$OS" = ps5 ] && echo ps5; } || { [ "$OS" = microvm ] && echo microvm-x64; } || { [ "$GUEST_BITS" = 32 ] && echo windows-x86 || echo windows-x64; })}; RT_FILES=$("$PISCO" sources "base/$BASE:1" --all-libraries); fi
RT_FILES=$(for f in $RT_FILES; do echo "$root/$f"; done)
python3 "$root/tools/recipe-gen.py" ${RECIPE:+"$RECIPE"} > "$out/recipe.gen.c"
# the import table: every IMP(dll, name) the runtime defines, and for a 32-bit title the stdcall sizes
"$root/tools/gen-imports.sh" "$GUEST_BITS" "$out" "$out/imports.gen.c" $RT_FILES
CF="$TARGET -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything -DGUEST_BITS=$GUEST_BITS"
[ "$OS" = linux ] && CF="$CF -DGUEST_LINUX=1"   # the x87 at its full precision (rt/cpu.h fpx), in the lifted code and the runtime alike
# a machine's image (rt/microvm-x64): memory through its MMU (rt/cpu.h), jumps between functions as tail calls
# (and its programs are Linux programs: the x87 at its full precision, as a Linux title's)
[ "$OS" = microvm ] && CF="$CF -DGUEST_MACHINE=1 -DGUEST_LINUX=1 -mtail-call"
# The memory layout. By default the runtime's stack, data and heap come first and the images sit above them
# (the lifter moves them there). An image that has to load low (a 32-bit title's exe at 0x400000, often with no
# relocations) gets a gap instead: the runtime's stack below it, the runtime's data and heap above it.
LAYOUT="--stack-first,-z,stack-size=8388608"
# (a Linux program that is not position-independent loads low too: 0x400000 unless its link said otherwise)
if [ "$GUEST_BITS" = 32 ] || { [ "$OS" = linux ] && [ $((IMAGE_LOW)) -lt $((0x10000000)) ]; }; then
  [ $((IMAGE_LOW)) -ge $((0x100000)) ] || { echo "build-title.sh: an image at $IMAGE_LOW leaves no room for the runtime's stack below it" >&2; exit 1; }
  stack=$(( IMAGE_LOW < 0x800000 ? IMAGE_LOW : 0x800000 )); stack=$(( stack & ~0xFFFF ))
  LAYOUT="--stack-first,-z,stack-size=$stack,--global-base=$(( (IMAGE_HIGH + 0xFFFFF) & ~0xFFFFF ))"
fi
# PROGRAMS="<elf>...": a machine's programs (its kernel's initramfs), each lifted on its own under its name (lifter
# --user; they may share addresses), in <out>/prog-<name>/, and listed in programs.gen.c for rt/microvm-x64/run.c
progs=""; list=""; decl=""
for p in ${PROGRAMS:-}; do
  n=$(basename "$p" | tr -c 'a-zA-Z0-9_\n' '_' | sed 's/^\([0-9]\)/_\1/'); mkdir -p "$out/prog-$n"
  "$root/target/release/lifter" "$p" "$out/prog-$n/title.c" --split "${SPLIT:-8}" --user "$n" > "$out/prog-$n/lifter.log" 2>&1 || { echo "build-title.sh: the lifter failed on $p ($out/prog-$n/lifter.log)" >&2; exit 1; }
  progs="$progs $out/prog-$n"; decl="$decl extern const ProgramDef ${n}_program;"; list="$list &${n}_program,"
done
[ -z "$progs" ] || printf '#include "cpu.h"\n%s\nconst ProgramDef *const rt_programs[] = {%s};\nconst unsigned rt_program_count = %d;\n' "$decl" "$list" "$(echo $progs | wc -w)" > "$out/programs.gen.c"
[ -n "$progs" ] || rm -f "$out/programs.gen.c" "$out/programs.gen.o"
# SIDE_PROGRAMS="<elf>...": a machine's programs each in a module of its own (<out>/<name>.wasm, a wasm side module:
# position-independent, it imports this module's memory, table and functions; the host loads it, SW_PROGRAMS). The
# title's module exports what they import
side_exports=""
for p in ${SIDE_PROGRAMS:-}; do
  n=$(basename "$p" | tr -c 'a-zA-Z0-9_\n' '_' | sed 's/^\([0-9]\)/_\1/'); sd="$out/side-$n"; mkdir -p "$sd"
  "$root/target/release/lifter" "$p" "$sd/title.c" --split "${SPLIT:-8}" --user "$n" > "$sd/lifter.log" 2>&1 || { echo "build-title.sh: the lifter failed on $p ($sd/lifter.log)" >&2; exit 1; }
  for f in "$sd"/title*.c; do [ "$f.o" -nt "$f" ] && [ "$f.o" -nt "$root/rt/cpu.h" ] || echo "$f"; done | xargs -r -P "$JOBS" -I{} sh -c '"$0"/clang $1 -fPIC -ffreestanding -c "{}" -o "{}.o"' "$W" "$CF"
  "$W/wasm-ld" -shared --experimental-pic --shared-memory --export="${n}_program" --unresolved-symbols=import-dynamic -o "$out/$n.wasm" "$sd"/title*.c.o
  side_exports="$side_exports $(node -e 'const m = new WebAssembly.Module(require("fs").readFileSync(process.argv[1])); console.log(WebAssembly.Module.imports(m).filter((i) => (i.module === "env" && i.kind === "function") || i.module === "GOT.mem").map((i) => i.name).join(" "))' "$out/$n.wasm")"
done
SIDE_LINK=""; [ -z "${SIDE_PROGRAMS:-}" ] || SIDE_LINK="-Wl,--export-table,--growable-table,--export=__stack_pointer$(for x in $(echo $side_exports | tr ' ' '\n' | sort -u); do printf ',--export=%s' "$x"; done)"
# lifted code: freestanding, no libc
for f in "$out"/title*.c $(for d in $progs; do ls "$d"/title*.c; done); do [ "$f.o" -nt "$f" ] && [ "$f.o" -nt "$root/rt/cpu.h" ] || echo "$f"; done | xargs -r -P "$JOBS" -I{} sh -c '"$0"/clang $1 -ffreestanding -c "{}" -o "{}.o"' "$W" "$CF"
# XMA2 / Opus decoding: FFmpeg's decoders when tools/build-ffmpeg-audio.sh has built them (else silent voices)
FF="$root/out/ffmpeg"; AUDIO_LIBS=""
if [ -f "$FF/lib/libavcodec.a" ]; then AUDIO_LIBS="$FF/lib/libavcodec.a $FF/lib/libswresample.a $FF/lib/libavutil.a -lm"; fi
echo "$RT_FILES" | grep -q "/audio_codec\.c$" || AUDIO_LIBS=""
rm -f "$out"/rt_*.o   # another base's objects must not be linked
for f in $RT_FILES; do b=$(echo "${f#$root/rt/}" | tr / _ | sed "s/\.c$//"); X=""; [ "$(basename "$f" .c)" = audio_codec ] && [ -n "$AUDIO_LIBS" ] && X="-DSW_AUDIO_CODECS -I$FF/include"
  "$W/clang" $CF $X -c "$f" -o "$out/rt_$b.o" & done
"$W/clang" $CF -c "$out/imports.gen.c" -o "$out/imports.gen.o"
"$W/clang" $CF -c "$out/recipe.gen.c" -o "$out/recipe.gen.o"
[ ! -f "$out/programs.gen.c" ] || "$W/clang" $CF -c "$out/programs.gen.c" -o "$out/programs.gen.o"
wait
"$W/clang" $TARGET -pthread "$out"/title*.c.o $(for d in $progs; do ls "$d"/title*.c.o; done) $([ -f "$out/programs.gen.o" ] && echo "$out/programs.gen.o") "$out"/rt_*.o "$out/imports.gen.o" "$out/recipe.gen.o" $AUDIO_LIBS -o "$out/title.wasm" -mllvm -wasm-enable-sjlj -lsetjmp \
  -Wl,--import-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,--export=wasi_thread_start,--export=rt_last_pc,$LAYOUT \
  -Wl,--export=__heap_base $SIDE_LINK
