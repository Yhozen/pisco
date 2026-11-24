// SPDX-License-Identifier: GPL-2.0-or-later
// Sound files to 16-bit PCM (audio_decode.h). WAV: uncompressed PCM, 8 bits (unsigned) or 16 (signed), any rate and
// channel count; every WAV in Narbacular Drop's File.ore is PCM (69 at 44,100 Hz / 16 bits, one at 11,025 Hz /
// 8 bits), so ADPCM is not decoded and such a file is refused. Ogg Vorbis with stb_vorbis, MP3 with minimp3
// (third_party/, unchanged), each decoding as it is read.
#include <stdlib.h>
#include <string.h>
#include "audio_decode.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define get_bits vorbis_get_bits   // a name both decoders use for a static function of their own
#include "third_party/stb_vorbis.c"
#undef get_bits
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"

struct AudioFile {
  int format, channels, rate;
  const uint8_t *data; uint32_t size;
  // WAV
  uint32_t pcm_at, pcm_bytes, bits, block; uint64_t wav_frame;
  // Ogg
  stb_vorbis *ogg;
  // MP3: the decoder, where it is in the file, and the frame it decoded last (what of it is not read yet)
  mp3dec_t mp3; uint32_t mp3_at; int16_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; int mp3_have, mp3_used; uint64_t mp3_length;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_open(AudioFile *f) {
  const uint8_t *d = f->data; uint32_t n = f->size, at = 12; int have_fmt = 0;
  while (at + 8 <= n) {
    uint32_t id = le32(d + at), len = le32(d + at + 4), body = at + 8;
    if (id == 0x20746d66 && len >= 16 && body + 16 <= n) { // "fmt "
      uint16_t tag = le16(d + body);
      if (tag == 0xFFFE && len >= 40) tag = le16(d + body + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format
      f->channels = le16(d + body + 2); f->rate = (int)le32(d + body + 4); f->block = le16(d + body + 12); f->bits = le16(d + body + 14);
      if (tag != 1 || (f->bits != 8 && f->bits != 16) || !f->channels || f->block != f->channels * f->bits / 8) return 0;
      have_fmt = 1;
    } else if (id == 0x61746164 && have_fmt) { // "data"
      f->pcm_at = body; f->pcm_bytes = body + len <= n ? len : n - body; f->pcm_bytes -= f->pcm_bytes % f->block;
      return 1;
    }
    at = body + len + (len & 1);
  }
  return 0;
}
static int mp3_frame(AudioFile *f) { // decodes the next frame; 0 at the end
  for (;;) {
    mp3dec_frame_info_t info; memset(&info, 0, sizeof info);
    if (f->mp3_at >= f->size) return 0;
    int got = mp3dec_decode_frame(&f->mp3, f->data + f->mp3_at, (int)(f->size - f->mp3_at), f->mp3_pcm, &info);
    if (!info.frame_bytes) return 0;
    f->mp3_at += (uint32_t)info.frame_bytes;
    if (got > 0) {
      if (!f->channels) { f->channels = info.channels; f->rate = info.hz; }
      if (info.channels != f->channels) continue;   // a frame that does not fit the stream: skipped
      f->mp3_have = got; f->mp3_used = 0; return 1;
    }
  }
}

AudioFile *audio_open(const uint8_t *data, uint32_t size) {
  AudioFile *f = calloc(1, sizeof *f); f->data = data; f->size = size;
  if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) { f->format = AUDIO_WAV; if (wav_open(f)) return f; }
  else if (size >= 4 && !memcmp(data, "OggS", 4)) {
    int err = 0; f->ogg = stb_vorbis_open_memory(data, (int)size, &err, 0);
    if (f->ogg) { stb_vorbis_info i = stb_vorbis_get_info(f->ogg); f->format = AUDIO_OGG; f->channels = i.channels; f->rate = (int)i.sample_rate; return f; }
  } else if (size >= 4 && (!memcmp(data, "ID3", 3) || (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0))) {
    f->format = AUDIO_MP3; mp3dec_init(&f->mp3);
    if (mp3_frame(f)) return f;
  }
  free(f); return 0;
}
int audio_format(const AudioFile *f) { return f->format; }
int audio_channels(const AudioFile *f) { return f->channels; }
int audio_rate(const AudioFile *f) { return f->rate; }

uint32_t audio_read(AudioFile *f, int16_t *out, uint32_t frames) {
  uint32_t done = 0, ch = (uint32_t)f->channels;
  if (f->format == AUDIO_WAV) {
    uint64_t total = f->pcm_bytes / f->block;
    while (done < frames && f->wav_frame < total) {
      const uint8_t *p = f->data + f->pcm_at + f->wav_frame * f->block;
      for (uint32_t c = 0; c < ch; c++) out[done * ch + c] = f->bits == 8 ? (int16_t)((p[c] - 128) << 8) : (int16_t)le16(p + 2 * c);
      done++; f->wav_frame++;
    }
  } else if (f->format == AUDIO_OGG) {
    while (done < frames) {
      int got = stb_vorbis_get_samples_short_interleaved(f->ogg, (int)ch, out + done * ch, (int)((frames - done) * ch));
      if (got <= 0) break;
      done += (uint32_t)got;
    }
  } else if (f->format == AUDIO_MP3) {
    while (done < frames) {
      if (f->mp3_used == f->mp3_have && !mp3_frame(f)) break;
      uint32_t n = (uint32_t)(f->mp3_have - f->mp3_used); if (n > frames - done) n = frames - done;
      memcpy(out + done * ch, f->mp3_pcm + f->mp3_used * ch, n * ch * sizeof(int16_t));
      done += n; f->mp3_used += (int)n;
    }
  }
  return done;
}
int audio_seek(AudioFile *f, uint64_t frame) {
  if (f->format == AUDIO_WAV) { if (frame > f->pcm_bytes / f->block) return 0; f->wav_frame = frame; return 1; }
  if (f->format == AUDIO_OGG) return frame ? stb_vorbis_seek_frame(f->ogg, (unsigned)frame) : stb_vorbis_seek_start(f->ogg);
  if (f->format == AUDIO_MP3) { // from the start, then decoded up to the frame
    mp3dec_init(&f->mp3); f->mp3_at = 0; f->mp3_have = f->mp3_used = 0;
    while (frame) {
      if (f->mp3_used == f->mp3_have && !mp3_frame(f)) return 0;
      uint32_t n = (uint32_t)(f->mp3_have - f->mp3_used); if (n > frame) n = (uint32_t)frame;
      f->mp3_used += (int)n; frame -= n;
    }
    return 1;
  }
  return 0;
}
uint64_t audio_length(AudioFile *f) {
  if (f->format == AUDIO_WAV) return f->pcm_bytes / f->block;
  if (f->format == AUDIO_OGG) return stb_vorbis_stream_length_in_samples(f->ogg);
  if (f->format == AUDIO_MP3) {
    if (!f->mp3_length) { // counted once with a decoder of its own
      AudioFile g = *f; mp3dec_init(&g.mp3); g.mp3_at = 0; g.mp3_have = g.mp3_used = 0;
      while (mp3_frame(&g)) f->mp3_length += (uint64_t)g.mp3_have;
    }
    return f->mp3_length;
  }
  return 0;
}
void audio_close(AudioFile *f) { if (!f) return; if (f->ogg) stb_vorbis_close(f->ogg); free(f); }
