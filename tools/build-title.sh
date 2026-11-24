#!/bin/bash
# build-title.sh <image.exe> <out dir>: lift a title, compile it with the runtime into <out dir>/title.wasm
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS (parallel compiles), OPT (default -O2)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); exe=$1; out=$2; mkdir -p "$out"
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
JOBS=${JOBS:-$(nproc)}
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
"$root/target/release/lifter" "$exe" "$out/title.c" --split "${SPLIT:-8}"
# the import table: every IMP(dll, name) the runtime defines
grep -ho 'IMP([a-z0-9_]*, *[A-Za-z0-9_]*)' "$root"/rt/*.c | sort -u | sed -E 's/IMP\(([a-z0-9_]+), *([A-Za-z0-9_]+)\)/\1 \2/' > "$out/imports.txt"
{ echo '#include "rt.h"'
  awk '{print "void imp_"$1"_"$2"(Cpu*);"}' "$out/imports.txt"
  echo 'typedef struct { const char *name; ImportFn fn; } ImportDef;'
  echo 'const ImportDef rt_import_table[] = {'
  awk '{print "  {\""$1".dll!"$2"\", imp_"$1"_"$2"},"}' "$out/imports.txt"
  echo '};'
  echo "const unsigned rt_import_table_count = sizeof rt_import_table / sizeof rt_import_table[0];"
} > "$out/imports.gen.c"
CF="$TARGET -mllvm -wasm-enable-sjlj ${OPT:--O2} -fno-strict-aliasing -I$root/rt -Wno-everything"
# lifted code: freestanding, no libc
for f in "$out"/title*.c; do [ "$f.o" -nt "$f" ] && [ "$f.o" -nt "$root/rt/cpu.h" ] || echo "$f"; done | xargs -r -P "$JOBS" -I{} sh -c '"$0"/clang $1 -ffreestanding -c "{}" -o "{}.o"' "$W" "$CF"
# XMA2 / Opus decoding: FFmpeg's decoders when tools/build-ffmpeg-audio.sh has built them (else silent voices)
FF="$root/out/ffmpeg"; AUDIO_LIBS=""
if [ -f "$FF/lib/libavcodec.a" ]; then AUDIO_LIBS="$FF/lib/libavcodec.a $FF/lib/libswresample.a $FF/lib/libavutil.a -lm"; fi
for f in "$root"/rt/*.c; do b=$(basename "$f" .c); X=""; [ "$b" = audio_codec ] && [ -n "$AUDIO_LIBS" ] && X="-DSW_AUDIO_CODECS -I$FF/include"
  "$W/clang" $CF $X -c "$f" -o "$out/rt_$b.o" & done
"$W/clang" $CF -c "$out/imports.gen.c" -o "$out/imports.gen.o"
wait
"$W/clang" $TARGET -pthread "$out"/title*.c.o "$out"/rt_*.o "$out/imports.gen.o" $AUDIO_LIBS -o "$out/title.wasm" -mllvm -wasm-enable-sjlj -lsetjmp \
  -Wl,--import-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,--export=wasi_thread_start,--stack-first,-z,stack-size=8388608 \
  -Wl,--export=__heap_base
