// SPDX-License-Identifier: GPL-2.0-or-later
// Sound files decoded to 16-bit PCM (audio_decode.c): WAV (PCM, 8 or 16 bits), Ogg Vorbis (stb_vorbis) and MP3
// (minimp3), from a whole file in memory. A translation unit of its own: the two decoders never see the runtime's
// headers (cpu.h's short register macros).
#pragma once
#include <stdint.h>

typedef struct AudioFile AudioFile;
enum { AUDIO_WAV = 1, AUDIO_OGG, AUDIO_MP3 };
// The format by content (RIFF/WAVE, OggS, an MP3 frame or ID3 tag); 0 if none of the three. The data stays the
// caller's and must outlive the decoder.
AudioFile *audio_open(const uint8_t *data, uint32_t size);
int audio_format(const AudioFile *f);
int audio_channels(const AudioFile *f);
int audio_rate(const AudioFile *f);
// Up to `frames` frames of interleaved PCM16; fewer only at the end
uint32_t audio_read(AudioFile *f, int16_t *out, uint32_t frames);
// Back to a frame from the start (0: the beginning); 0 if it could not
int audio_seek(AudioFile *f, uint64_t frame);
// The length in frames, decoding the whole file once if the format does not say (MP3); 0 if unknown
uint64_t audio_length(AudioFile *f);
void audio_close(AudioFile *f);
