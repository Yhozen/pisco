#!/bin/bash
# tests/audio/run.sh: builds the minimal FFmpeg (tools/build-ffmpeg-audio.sh, skipped when already built), links
# rt/audio_codec.c + audio_codec_test.c into a wasm32-wasip1-threads program the way tools/build-title.sh links
# a title (shared imported memory, -pthread), runs it under Node's WASI and passes when the test prints PASS.
# Also checks rt/audio_codec.c still compiles without SW_AUDIO_CODECS (the stubs).
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd); out=$root/out/audio-test; mkdir -p "$out"
W=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}/bin
ff=$root/out/ffmpeg
"$root/tools/build-ffmpeg-audio.sh"
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
CF="$TARGET -mllvm -wasm-enable-sjlj -O2 -fno-strict-aliasing -I$root/rt -Wall -Wextra -Werror"
"$W/clang" $CF -c "$root/rt/audio_codec.c" -o "$out/audio_codec_stub.o"
"$W/clang" $CF -DSW_AUDIO_CODECS -I"$ff/include" -c "$root/rt/audio_codec.c" -o "$out/audio_codec.o"
"$W/clang" $CF -c "$root/tests/audio/audio_codec_test.c" -o "$out/audio_codec_test.o"
# --export-memory as well as --import-memory: Node's WASI looks for an exported memory
"$W/clang" $TARGET -pthread "$out/audio_codec_test.o" "$out/audio_codec.o" \
  "$ff/lib/libavcodec.a" "$ff/lib/libswresample.a" "$ff/lib/libavutil.a" -lm -o "$out/audio_codec_test.wasm" \
  -mllvm -wasm-enable-sjlj -lsetjmp \
  -Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,-z,stack-size=1048576
ls -l "$out/audio_codec_test.wasm"
node --no-warnings - "$out/audio_codec_test.wasm" <<'JS'
const fs = require('fs'), { WASI } = require('wasi');
const wasi = new WASI({ version: 'preview1', args: ['audio_codec_test'], env: {}, returnOnExit: true });
const memory = new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true });
const mod = new WebAssembly.Module(fs.readFileSync(process.argv[2]));
const inst = new WebAssembly.Instance(mod, {
  env: { memory },
  wasi: { 'thread-spawn': () => -1 }, // the decoders spawn no threads (thread_count = 1)
  wasi_snapshot_preview1: wasi.wasiImport,
});
process.exitCode = wasi.start(inst);
JS
