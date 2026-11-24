#!/bin/bash
# build-ffmpeg-audio.sh: a minimal FFmpeg for wasm32 (wasi-sdk) with only the XMA1/XMA2 (wmaprodec.c) and Opus
# audio decoders, for rt/audio_codec.c (built with -DSW_AUDIO_CODECS).
#   out/ffmpeg/lib/{libavcodec,libswresample,libavutil}.a   link in this order
#   out/ffmpeg/include/                                       headers (-Iout/ffmpeg/include)
# FFmpeg is LGPL-2.1-or-later; it is configured without --enable-gpl / --enable-nonfree, so it stays LGPL.
# Its source is fetched (pinned tarball, sha256 below) into out/ffmpeg/, never committed. libswresample is
# needed because FFmpeg's Opus decoder resamples SILK (8/12/16 kHz) to 48 kHz with it (configure:
# opus_decoder_deps="swresample").
# Environment: WASI_SDK (default ~/tools/wasi-sdk-34.0-x86_64-linux), JOBS (parallel make), FORCE=1 rebuilds.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
WASI_SDK=${WASI_SDK:-$HOME/tools/wasi-sdk-34.0-x86_64-linux}
W=$WASI_SDK/bin
JOBS=${JOBS:-$(nproc)}
VER=7.1.5
SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f # ffmpeg-7.1.5.tar.xz, GPG-verified (key FCF986EA15E6E293A5644F10B4322F04D67658D8)
URL=https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
top=$root/out/ffmpeg
dl=$top/dl tar=$top/dl/ffmpeg-$VER.tar.xz src=$top/src build=$top/build prefix=$top
stamp=$top/.built-$VER

if [ -z "${FORCE:-}" ] && [ -f "$stamp" ] && [ -f "$prefix/lib/libavcodec.a" ]; then
  echo "ffmpeg $VER already built in $prefix (FORCE=1 to rebuild)"; exit 0
fi

# 1. fetch and verify
mkdir -p "$dl"
if ! { [ -f "$tar" ] && echo "$SHA256  $tar" | sha256sum -c --status; }; then
  rm -f "$tar.part"
  curl -fSL --retry 3 -o "$tar.part" "$URL"
  echo "$SHA256  $tar.part" | sha256sum -c --status || { echo "sha256 mismatch for $URL" >&2; rm -f "$tar.part"; exit 1; }
  mv "$tar.part" "$tar"
fi

# 2. extract into a fresh directory (untrusted data: nothing from it runs except FFmpeg's own configure/make,
#    which is the code we are asked to build)
rm -rf "$src" "$build"; mkdir -p "$src" "$build"
tar -xJf "$tar" -C "$src" --strip-components=1 --no-same-owner

# 3. configure: the same target flags as the runtime (tools/build-title.sh), so the objects link into a
#    shared-memory title.wasm. Pthreads come from wasi-libc (wasm32-wasip1-threads): FFmpeg uses them for its
#    one-time init (ff_thread_once) and locks, so decoders may be opened from several runtime threads; the
#    wrapper still asks each decoder for one thread (no frame threads are spawned).
# No source patches are needed for 7.1.5 with wasi-sdk 34: configure's probes find most of wasi-libc's gaps by
# themselves (HAVE_MMAP, HAVE_GETRUSAGE, ... come out 0) and --disable-asm covers the rest. One define:
#  - -D_DEFAULT_SOURCE: FFmpeg compiles with -D_POSIX_C_SOURCE/-D_XOPEN_SOURCE, which hide wasi-libc's
#    arc4random_buf (declared for _BSD_SOURCE only), so av_random_bytes (libavutil/random_seed.c) would fall
#    back to get_generic_seed(), which calls clock(): wasi-libc has no clock() without
#    -D_WASI_EMULATED_PROCESS_CLOCKS + -lwasi-emulated-process-clocks, and the final link fails on it.
#    With the define, configure finds arc4random_buf (WASI random_get) and the clock() path is dead code.
# --target-os=none / --arch=wasm32 make configure print "unknown architecture wasm32", which is harmless.
TARGET="--target=wasm32-wasip1-threads -matomics -mbulk-memory -msimd128 -mnontrapping-fptoint -msign-ext"
CFLAGS="$TARGET -mllvm -wasm-enable-sjlj -fno-strict-aliasing -D_DEFAULT_SOURCE"
cd "$build"
"$src/configure" \
  --prefix="$prefix" \
  --enable-cross-compile --target-os=none --arch=wasm32 \
  --cc="$W/clang" --ar="$W/llvm-ar" --ranlib="$W/llvm-ranlib" --nm="$W/llvm-nm" --strip="$W/llvm-strip" \
  --extra-cflags="$CFLAGS" --extra-ldflags="$TARGET" \
  --optflags="-O2" \
  --disable-everything --disable-autodetect \
  --disable-programs --disable-doc --disable-avformat --disable-avdevice --disable-swscale --disable-avfilter \
  --disable-network --disable-asm --disable-inline-asm --enable-pthreads --disable-w32threads --disable-os2threads \
  --disable-runtime-cpudetect --disable-debug --disable-shared --enable-static --enable-small \
  --enable-avcodec --enable-avutil --enable-swresample \
  --enable-decoder=xma1,xma2,opus \
  > "$top/configure.log" 2>&1 || { tail -40 "$top/configure.log"; echo "configure failed; see $build/ffbuild/config.log" >&2; exit 1; }

# 4. build and install (libs + headers only)
make -j"$JOBS" > "$top/make.log" 2>&1 || { grep -E 'error|Error' "$top/make.log" | head -30; echo "make failed; see $top/make.log" >&2; exit 1; }
rm -rf "$prefix/lib" "$prefix/include"
make install-libs install-headers > "$top/install.log" 2>&1
grep -E '^(CONFIG_(XMA1|XMA2|OPUS|WMAPRO)_DECODER)=' "$build/ffbuild/config.mak" || true
ls -l "$prefix"/lib/*.a
touch "$stamp"
