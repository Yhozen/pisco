// SPDX-License-Identifier: GPL-2.0-or-later
// XMA1 / XMA2 / Opus to interleaved PCM16 over FFmpeg's libavcodec (declarations and contract: audio_codec.h).
// Built with -DSW_AUDIO_CODECS -I out/ffmpeg/include, linked with out/ffmpeg/lib/{libavcodec,libswresample,
// libavutil}.a (tools/build-ffmpeg-audio.sh). Without SW_AUDIO_CODECS only failing stubs are compiled.
#include "audio_codec.h"

#ifndef SW_AUDIO_CODECS

AudioDec *adec_open(int codec, int channels, int rate, const void *extradata, int extradata_size) {
  (void)codec, (void)channels, (void)rate, (void)extradata, (void)extradata_size;
  return 0;
}
int adec_decode(AudioDec *dec, const void *packet, int bytes, short *pcm_out, int max_frames) {
  (void)dec, (void)packet, (void)bytes, (void)pcm_out, (void)max_frames;
  return -1;
}
int adec_finish(AudioDec *dec) { (void)dec; return -1; }
void adec_reset(AudioDec *dec) { (void)dec; }
int adec_channels(const AudioDec *dec) { (void)dec; return 0; }
int adec_rate(const AudioDec *dec) { (void)dec; return 0; }
void adec_close(AudioDec *dec) { (void)dec; }

#else

#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>

struct AudioDec {
  AVCodecContext *ctx;
  AVPacket *pkt;
  AVFrame *frame;
  int channels, rate;
  short *buf; // decoded, interleaved: frames [pos, len) are unread
  int len, pos, cap;
};

static short to_s16(float f) {
  f *= 32768.0f;
  return f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : (short)__builtin_lrintf(f);
}

static int reserve(AudioDec *d, int frames) {
  if (d->pos && d->pos == d->len) d->pos = d->len = 0;
  if (d->len + frames <= d->cap) return 0;
  if (d->pos) { // compact first
    memmove(d->buf, d->buf + (size_t)d->pos * d->channels, (size_t)(d->len - d->pos) * d->channels * sizeof(short));
    d->len -= d->pos, d->pos = 0;
    if (d->len + frames <= d->cap) return 0;
  }
  int cap = d->cap ? d->cap : 4096;
  while (cap < d->len + frames) cap *= 2;
  short *b = realloc(d->buf, (size_t)cap * d->channels * sizeof(short));
  if (!b) return -1;
  d->buf = b, d->cap = cap;
  return 0;
}

// appends one decoded AVFrame to the buffer as interleaved PCM16
static int append(AudioDec *d, const AVFrame *f) {
  int n = f->nb_samples, ch = d->channels, fch = f->ch_layout.nb_channels;
  if (n <= 0) return 0;
  if (reserve(d, n)) return AVERROR(ENOMEM);
  short *o = d->buf + (size_t)d->len * ch;
  for (int c = 0; c < ch; c++) {
    int sc = c < fch ? c : fch - 1; // never happens with the configured decoders; keeps us in bounds
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: { const float *s = (const float *)f->extended_data[sc]; for (int i = 0; i < n; i++) o[i * ch + c] = to_s16(s[i]); break; }
    case AV_SAMPLE_FMT_FLT:  { const float *s = (const float *)f->extended_data[0];  for (int i = 0; i < n; i++) o[i * ch + c] = to_s16(s[i * fch + sc]); break; }
    case AV_SAMPLE_FMT_S16P: { const short *s = (const short *)f->extended_data[sc]; for (int i = 0; i < n; i++) o[i * ch + c] = s[i]; break; }
    case AV_SAMPLE_FMT_S16:  { const short *s = (const short *)f->extended_data[0];  for (int i = 0; i < n; i++) o[i * ch + c] = s[i * fch + sc]; break; }
    default: return AVERROR(ENOSYS);
    }
  }
  d->len += n;
  return 0;
}

// moves every frame the decoder has ready into the buffer
static int receive_all(AudioDec *d) {
  for (;;) {
    int r = avcodec_receive_frame(d->ctx, d->frame);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
    if (r < 0) return r;
    r = append(d, d->frame);
    av_frame_unref(d->frame);
    if (r < 0) return r;
  }
}

void adec_close(AudioDec *d) {
  if (!d) return;
  avcodec_free_context(&d->ctx);
  av_packet_free(&d->pkt);
  av_frame_free(&d->frame);
  free(d->buf);
  free(d);
}

AudioDec *adec_open(int codec, int channels, int rate, const void *extradata, int extradata_size) {
  enum AVCodecID id = codec == ADEC_XMA2 ? AV_CODEC_ID_XMA2 : codec == ADEC_XMA1 ? AV_CODEC_ID_XMA1
                    : codec == ADEC_OPUS ? AV_CODEC_ID_OPUS : AV_CODEC_ID_NONE;
  const AVCodec *c = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : 0;
  if (!c || channels <= 0 || rate <= 0 || extradata_size < 0 || (extradata_size && !extradata)) return 0;
  const unsigned char *ed = extradata;
  if (id == AV_CODEC_ID_XMA2 && extradata_size == 18 + 34) ed += 18, extradata_size = 34; // whole XMA2WAVEFORMATEX
  if (id != AV_CODEC_ID_OPUS && extradata_size <= 0) return 0;
  av_log_set_level(AV_LOG_QUIET);
  AudioDec *d = calloc(1, sizeof *d);
  if (!d) return 0;
  d->ctx = avcodec_alloc_context3(c);
  d->pkt = av_packet_alloc();
  d->frame = av_frame_alloc();
  if (!d->ctx || !d->pkt || !d->frame) goto fail;
  av_channel_layout_default(&d->ctx->ch_layout, channels);
  d->ctx->sample_rate = rate;
  d->ctx->thread_count = 1;
  if (extradata_size > 0) {
    d->ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata) goto fail;
    memcpy(d->ctx->extradata, ed, (size_t)extradata_size);
    d->ctx->extradata_size = extradata_size;
  }
  if (avcodec_open2(d->ctx, c, 0) < 0) goto fail;
  d->channels = d->ctx->ch_layout.nb_channels;
  d->rate = d->ctx->sample_rate;
  if (d->channels <= 0) goto fail;
  return d;
fail:
  adec_close(d);
  return 0;
}

int adec_decode(AudioDec *d, const void *packet, int bytes, short *pcm_out, int max_frames) {
  if (!d || max_frames < 0 || (max_frames && !pcm_out)) return -1;
  int err = 0;
  if (packet && bytes > 0) {
    // the decoders read past the end (bit readers): copy into a padded packet
    if (av_new_packet(d->pkt, bytes) < 0) return AVERROR(ENOMEM);
    memcpy(d->pkt->data, packet, (size_t)bytes);
    err = avcodec_send_packet(d->ctx, d->pkt);
    if (err == AVERROR(EAGAIN)) { // output pending inside the decoder: take it, then retry once
      err = receive_all(d);
      if (!err) err = avcodec_send_packet(d->ctx, d->pkt);
    }
    av_packet_unref(d->pkt);
    if (!err) err = receive_all(d);
  }
  int n = d->len - d->pos;
  if (n > max_frames) n = max_frames;
  if (n > 0) {
    memcpy(pcm_out, d->buf + (size_t)d->pos * d->channels, (size_t)n * d->channels * sizeof(short));
    d->pos += n;
  }
  return n > 0 || err >= 0 ? n : err;
}

int adec_finish(AudioDec *d) {
  if (!d) return -1;
  int err = avcodec_send_packet(d->ctx, 0); // enter draining
  if (err >= 0 || err == AVERROR_EOF) err = receive_all(d);
  avcodec_flush_buffers(d->ctx); // ready for the next stream
  return err < 0 ? err : d->len - d->pos;
}

void adec_reset(AudioDec *d) {
  if (!d) return;
  avcodec_flush_buffers(d->ctx);
  d->len = d->pos = 0;
}

int adec_channels(const AudioDec *d) { return d ? d->channels : 0; }
int adec_rate(const AudioDec *d) { return d ? d->rate : 0; }

#endif
