// SPDX-License-Identifier: GPL-2.0-or-later
// tests/audio/audio_codec_test.c: decodes one XMA2 packet and one Opus packet through rt/gdk/audio_codec.c and prints
// frame counts, RMS and an FNV-1a checksum of the PCM16. Built and run by tests/audio/run.sh.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_codec.h"

// XMA2 mono 48 kHz, one 2048-byte packet (the rest is zero)
static const char xma_hex[] =
  "30 00 00 00 00 e7 00 0c 13 e3 ff ff fb 39 46 44 f6 55 a0 1c e0 01 82 7c 7f ff ff 67 28 c8 9d ca b4 03 9c 00 30 4f"
  "8f ff ff ec e5 19 13 d9 56 80 73 80 06 09 f1 ff ff fd 9c a3 22 77 2a d0 0e 70 00 c1 3e 3f ff ff b3 94 64 4f 65 5a"
  "01 ce 00 18 27 c7 ff ff f6 72 8c 89 dc ab 40 39 c0 03 04 f8 ff ff fe ce 51 91 3d 95 68 07 38 00 60 9f 1f ff ff d9"
  "ca 32 27 72 ad 00 e7 00 0c 13 e3 ff ff fb 39 46 44 f6 55 a0 1c e0 01 82 7c 7f ff ff 67 28 c8 9d ca b4 03 9c 00 30"
  "4f 8f ff ff ec e5 19 13 d9 56 80 73 80 06 09 f1 ff ff fd 9c a3 22 77 2a c0";
// Opus: Hybrid FB mono 20 ms (960 samples)
static const char opus_hex[] =
  "78 81 7b c6 11 76 f4 75 00 00 07 8e ca 69 db bb cc 48 dc 16 bd 9b d3 58 27 78 93 bc 37 44 5e cf 2d 97 00 69 80 11"
  "80 2c 03 38 ab 45 61 fd 89 b7 7e 0d 17 bf 20 a5 2b b5 ea 2b 5e d3 a7 48 d1 18 f2 0b 5c 01 4f 5f a1 96 21 ae de e0"
  "53 00 d6 65 65 3f 1f a6 cb b9 77 bf 97 62 c1 12 cf 8d c2 7d fd 00 80 ff fa c6 88 b6 de 10 b8 4f c6 23 6b ea 87 c9"
  "05";

static int unhex(const char *s, unsigned char *out, int cap) {
  int n = 0;
  for (;;) {
    while (*s == ' ') s++;
    if (!s[0] || !s[1] || n >= cap) return n;
    unsigned v;
    if (sscanf(s, "%2x", &v) != 1) return -1;
    out[n++] = (unsigned char)v, s += 2;
  }
}

static void put16(unsigned char *p, unsigned v) { p[0] = v, p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { put16(p, v), put16(p + 2, v >> 16); }

#define MAXF 65536
static short pcm[MAXF * 2];

// reads every buffered frame (in small chunks, to exercise the carry-over) and appends to pcm at *n
static int drain(AudioDec *d, int *n) {
  int r;
  while ((r = adec_decode(d, 0, 0, pcm + (size_t)*n * adec_channels(d), 700 < MAXF - *n ? 700 : MAXF - *n)) > 0) *n += r;
  return r;
}

static void report(const char *name, const short *s, int frames, int ch, int rate, unsigned *sum_out) {
  double acc = 0;
  unsigned h = 2166136261u;
  for (long i = 0; i < (long)frames * ch; i++) {
    acc += (double)s[i] * s[i];
    h = (h ^ (unsigned short)s[i]) * 16777619u;
  }
  double rms = frames ? sqrt(acc / ((double)frames * ch)) : 0;
  printf("%s: %d frames, %d ch, %d Hz, rms %.1f (%.2f dBFS), fnv1a %08x\n", name, frames, ch, rate, rms,
         rms > 0 ? 20 * log10(rms / 32768) : -999.0, h);
  if (sum_out) *sum_out = h;
}

int main(void) {
  int fails = 0;
  static unsigned char xma[2048], opus[256];
  if (unhex(xma_hex, xma, sizeof xma) != 177 || unhex(opus_hex, opus, sizeof opus) != 115) { puts("FAIL: bad hex"); return 1; }

  // XMA2: the 34-byte XMA2WAVEFORMATEX extension
  unsigned char ext[34] = {0};
  put16(ext + 0, 1);      // NumStreams
  put32(ext + 2, 4);      // ChannelMask (front center)
  put32(ext + 6, 6080);   // SamplesEncoded
  put32(ext + 10, 2048);  // BytesPerBlock
  ext[31] = 4;            // EncoderVersion (PlayBegin.. LoopCount 0)
  put16(ext + 32, 1);     // BlockCount
  AudioDec *x = adec_open(ADEC_XMA2, 1, 48000, ext, sizeof ext);
  if (!x) { puts("FAIL: adec_open(XMA2)"); return 1; }
  unsigned first = 0, again = 0;
  for (int pass = 0; pass < 2; pass++) { // the second pass checks the decoder restarts cleanly after adec_finish
    int n = 0, r = adec_decode(x, xma, sizeof xma, pcm, 0);
    printf("xma2 pass %d: adec_decode -> %d (frames held by the decoder until finish)\n", pass, r);
    if (r < 0) fails++;
    r = adec_finish(x);
    printf("xma2 pass %d: adec_finish -> %d buffered\n", pass, r);
    if (r < 0) fails++;
    drain(x, &n);
    report(pass ? "xma2 again" : "xma2", pcm, n, adec_channels(x), adec_rate(x), pass ? &again : &first);
    if (n < 5000 || n > 7000) { printf("FAIL: xma2 frame count %d not near 6080\n", n); fails++; }
  }
  if (first != again) { puts("FAIL: xma2 second pass differs"); fails++; }
  adec_close(x);

  // the whole 52-byte XMA2WAVEFORMATEX (WAVEFORMATEX in front) is accepted too
  unsigned char wfx[52] = {0};
  put16(wfx, 0x166), put16(wfx + 2, 1), put32(wfx + 4, 48000), put16(wfx + 16, 34);
  memcpy(wfx + 18, ext, sizeof ext);
  x = adec_open(ADEC_XMA2, 1, 48000, wfx, sizeof wfx);
  if (!x) { puts("FAIL: adec_open(XMA2, 52-byte format)"); fails++; }
  else {
    int n = 0; unsigned h;
    adec_decode(x, xma, sizeof xma, pcm, 0), adec_finish(x), drain(x, &n);
    report("xma2 (52-byte format)", pcm, n, adec_channels(x), adec_rate(x), &h);
    if (h != first) { puts("FAIL: differs from the 34-byte format"); fails++; }
    adec_close(x);
  }

  // Opus: one raw packet, no OpusHead
  AudioDec *o = adec_open(ADEC_OPUS, 1, 48000, 0, 0);
  if (!o) { puts("FAIL: adec_open(Opus)"); return 1; }
  int n = adec_decode(o, opus, 115, pcm, MAXF);
  printf("opus: adec_decode -> %d\n", n);
  if (n > 0) drain(o, &n);
  report("opus", pcm, n > 0 ? n : 0, adec_channels(o), adec_rate(o), 0);
  if (n != 960) { printf("FAIL: opus frame count %d, expected 960\n", n); fails++; }
  adec_close(o);

  if (adec_open(0x1234, 1, 48000, 0, 0)) { puts("FAIL: unknown codec opened"); fails++; }
  puts(fails ? "FAIL" : "PASS");
  return fails != 0;
}
