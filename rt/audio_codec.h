// Compressed audio to PCM16 (XMA1, XMA2, Opus) over a minimal FFmpeg libavcodec (tools/build-ffmpeg-audio.sh).
// Independent of the rest of the runtime. Compiled for real only with -DSW_AUDIO_CODECS (and FFmpeg's include
// dir / libs); otherwise every function fails (adec_open returns NULL), so builds without FFmpeg keep working.
// One AudioDec is used by one thread at a time; different AudioDecs may be used from different threads.
#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#define ADEC_XMA1 0x0165 // WAVE_FORMAT_XMA
#define ADEC_XMA2 0x0166 // WAVE_FORMAT_XMA2
#define ADEC_OPUS 0x704F // the tag XAudio2 / Media Foundation use for Opus

typedef struct AudioDec AudioDec;

// Opens a decoder. codec: ADEC_XMA1 / ADEC_XMA2 / ADEC_OPUS. channels, rate: the stream's (Opus always decodes
// at 48000 Hz, whatever rate says). extradata:
//   XMA2: the 34-byte XMA2WAVEFORMATEX extension (NumStreams u16, ChannelMask u32, SamplesEncoded u32,
//         BytesPerBlock u32, PlayBegin, PlayLength, LoopBegin, LoopLength u32, LoopCount u8, EncoderVersion u8,
//         BlockCount u16), or the whole 52-byte XMA2WAVEFORMATEX (the 18-byte WAVEFORMATEX in front is skipped).
//   XMA1: the XMAWAVEFORMAT (8 + 20 * NumStreams bytes).
//   Opus: optional "OpusHead" (19+ bytes; gives the pre-skip, gain and channel mapping); NULL for 1-2 channels.
// Returns NULL on failure.
AudioDec *adec_open(int codec, int channels, int rate, const void *extradata, int extradata_size);

// Feeds one packet (XMA: one or more whole 2048-byte packets; Opus: one raw Opus packet) if packet != NULL and
// bytes > 0, then copies up to max_frames decoded frames of interleaved PCM16 (adec_channels() samples each)
// to pcm_out. Frames that do not fit stay buffered: call again with packet = NULL to read them.
// Returns the number of frames written (0 is normal: FFmpeg's XMA decoder holds back ~4096 frames until it has
// more input or adec_finish), or < 0 on a decode error when no frames could be returned (the packet is
// dropped; the decoder stays usable).
int adec_decode(AudioDec *dec, const void *packet, int bytes, short *pcm_out, int max_frames);

// End of stream: drains the frames the decoder still holds into the buffer (read them with
// adec_decode(dec, NULL, 0, ...)), then makes the decoder ready for a new stream (a loop, the next buffer).
// Returns the number of frames now buffered, or < 0.
int adec_finish(AudioDec *dec);

// Discards all decoder state and buffered frames (seek).
void adec_reset(AudioDec *dec);

// Output format: channels per frame and sample rate. 0 for a NULL decoder.
int adec_channels(const AudioDec *dec);
int adec_rate(const AudioDec *dec);

void adec_close(AudioDec *dec);

#endif
