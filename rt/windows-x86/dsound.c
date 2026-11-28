// SPDX-License-Identifier: GPL-2.0-or-later
// DirectSound for Windows desktop apps: one device, the runtime's one sound output (mixer.c), and sound buffers
// in guest memory that the mixer plays. An app (Miles Sound System, which mixes its own voices into one looping
// buffer) writes into a buffer between Lock and Unlock and reads where playback is with GetCurrentPosition; the
// mixer moves the play position as it takes frames (48 kHz, with a virtual clock as the app's clock moves), so the
// positions an app sees follow what was really mixed. Every buffer is mixed in software: no hardware buffers, no
// 3D (IDirectSound3DBuffer is refused, as a device without it refuses it), no capture.
// Formats: PCM, 8 or 16 bits, mono or stereo, at any rate (resampled linearly to 48 kHz).
#include <math.h>
#include "com.h"
#include "win32.h"
#include "../mixer.h"
#include "dsound_layout.gen.h"

#define DS_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_UNSUPPORTED 0x80004001u
#define DSERR_INVALIDCALL 0x88780032u
#define CLASS_E_NOAGGREGATION 0x80040110u
#define IID_IUnknown_text "00000000-0000-0000-c000-000000000046"
#define IID_IDirectSound_text "279afa83-4981-11ce-a521-0020af0be560"
#define IID_IDirectSoundBuffer_text "279afa85-4981-11ce-a521-0020af0be560"
enum { DSBCAPS_PRIMARYBUFFER = 1, DSBCAPS_STATIC = 2, DSBCAPS_LOCSOFTWARE = 8, DSBCAPS_CTRLFREQUENCY = 0x20, DSBCAPS_CTRLPAN = 0x40, DSBCAPS_CTRLVOLUME = 0x80 };
enum { DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4, DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2 };

typedef struct { u32 vtable; s32 refs; u32 coop, hwnd; } Device;
typedef struct Buffer {
  u32 vtable; s32 refs;
  int primary;
  u32 flags, bytes; u8 *data;             // a secondary buffer's bytes (guest memory: Lock hands out pointers into them)
  u16 channels, bits; u32 rate, block;    // its format (WAVEFORMATEX); block: bytes a frame
  s32 volume, pan; u32 freq;              // hundredths of a decibel, -10000 left .. 10000 right, frames a second
  int playing, looping;
  double pos;                             // the play position, in frames
  struct Buffer *next;
} Buffer;
static Buffer *buffers;                   // the secondary buffers, for the mixer (mixer_lock)
static struct { u16 channels, bits; u32 rate; } primary_format = {2, 16, 22050};   // DirectSound's default primary format

// ---- the mixer's side ----
static float sample(const Buffer *b, u32 frame, int ch) {
  const u8 *p = b->data + (size_t)frame * b->block + (b->channels > 1 ? ch : 0) * (b->bits / 8);
  return b->bits == 16 ? (float)(s16)(p[0] | p[1] << 8) / 32768.0f : ((float)p[0] - 128.0f) / 128.0f;
}
static void mix(Cpu *c, float *out, u32 frames, void *ctx) {
  (void)c; (void)ctx;
  for (Buffer *b = buffers; b; b = b->next) {
    if (!b->playing || !b->bytes || !b->block) continue;
    u32 total = b->bytes / b->block;
    float amp = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float gl = amp * (b->pan > 0 ? powf(10.0f, (float)-b->pan / 2000.0f) : 1.0f), gr = amp * (b->pan < 0 ? powf(10.0f, (float)b->pan / 2000.0f) : 1.0f);
    double step = (double)(b->freq ? b->freq : b->rate) / MIXER_RATE;
    for (u32 n = 0; n < frames; n++) {
      u32 i = (u32)b->pos, j = i + 1;
      if (j >= total) j = b->looping ? 0 : i;
      float t = (float)(b->pos - i);
      float l = sample(b, i, 0) + (sample(b, j, 0) - sample(b, i, 0)) * t, r = sample(b, i, 1) + (sample(b, j, 1) - sample(b, i, 1)) * t;
      out[2 * n] += l * gl; out[2 * n + 1] += r * gr;
      b->pos += step;
      if (b->pos >= total) {
        if (b->looping) b->pos -= total * floor(b->pos / total);
        else { b->pos = 0; b->playing = 0; break; }   // a buffer played once stops at its end, back at the start
      }
    }
  }
}
static void start_output(void) { static int started; if (!started) { started = 1; mixer_add_source(mix, 0); mixer_start(); } }

// ---- DirectSoundCreate, DirectSoundEnumerateA ----
IMP(dsound, DirectSoundCreate) { // (device GUID or 0, IDirectSound **, outer)
  u64 out = ARG(1);
  if (!out) { RET(DSERR_INVALIDPARAM); return; }
  M32(out) = 0;
  if (ARG(2)) { RET(CLASS_E_NOAGGREGATION); return; }
  Device *d = (Device *)GP(com_new("dsound", &IDirectSound_interface, sizeof(Device)));
  d->refs = 1; M32(out) = (u32)(uptr)d;
  start_output();
  RET(DS_OK);
}
// (callback, context): the primary sound driver (no GUID), then the one device. Callback: BOOL (GUID *, description, module, context)
IMP(dsound, DirectSoundEnumerateA) {
  static u64 strings;
  if (!strings) { strings = (uptr)calloc(1, 128); strcpy(GSTR(strings), "Primary Sound Driver"); strcpy(GSTR(strings + 32), "PISCO sound output"); strcpy(GSTR(strings + 64), "pisco.drv"); M32(strings + 96) = 0x5C150000u; M32(strings + 100) = 0x0001; }
  if (!ARG(0)) { RET(DSERR_INVALIDPARAM); return; }
  if (rt_guest_call(c, ARG(0), 0, strings, strings + 120, ARG(1)) & 1) rt_guest_call(c, ARG(0), strings + 96, strings + 32, strings + 64, ARG(1));
  RET(DS_OK);
}

// ---- IDirectSound ----
#define DEV ((Device *)GP(ARG(0)))
IMP(dsound, IDirectSound_QueryInterface) {
  if (!ARG(2)) { RET(E_POINTER); return; }
  if (com_is_iid(ARG(1), IID_IDirectSound_text) || com_is_iid(ARG(1), IID_IUnknown_text)) { DEV->refs++; M32(ARG(2)) = (u32)ARG(0); RET(DS_OK); return; }
  M32(ARG(2)) = 0; RET(E_NOINTERFACE);
}
IMP(dsound, IDirectSound_AddRef) { RET(++DEV->refs); }
IMP(dsound, IDirectSound_Release) { Device *d = DEV; u32 n = (u32)--d->refs; if (!n) { d->vtable = 0; free(d); } RET(n); }
IMP(dsound, IDirectSound_SetCooperativeLevel) { DEV->hwnd = (u32)ARG(1); DEV->coop = (u32)ARG(2); RET(DS_OK); }
IMP(dsound, IDirectSound_Compact) { RET(DS_OK); }
IMP(dsound, IDirectSound_Initialize) { RET(DS_OK); }
IMP(dsound, IDirectSound_GetSpeakerConfig) { if (ARG(1)) M32(ARG(1)) = 4 | (5u << 16); RET(DS_OK); }   // DSSPEAKER_STEREO, the wide geometry (20 degrees)
IMP(dsound, IDirectSound_SetSpeakerConfig) { RET(DS_OK); }
// GetCaps(DSCAPS *): a device that mixes everything in software (no hardware buffers), at any rate from 100 to 200000
IMP(dsound, IDirectSound_GetCaps) {
  u64 p = ARG(1);
  if (!p || M32(p) < 96) { RET(DSERR_INVALIDPARAM); return; }
  memset((void *)GP(p + 4), 0, 92);
  M32(p + 4) = 0x0F | 0x10 | 0xF00;   // DSCAPS_PRIMARY{MONO,STEREO,8BIT,16BIT}, CONTINUOUSRATE, SECONDARY{MONO,STEREO,8BIT,16BIT}
  M32(p + 8) = 100; M32(p + 12) = 200000; M32(p + 16) = 1;   // secondary rates, primary buffers
  RET(DS_OK);
}
static Buffer *new_buffer(void) {
  Buffer *b = (Buffer *)GP(com_new("dsound", &IDirectSoundBuffer_interface, sizeof(Buffer)));
  b->refs = 1;
  return b;
}
// a WAVEFORMATEX: tag (PCM 1), channels, rate, bytes a second, block, bits
static int read_format(u64 f, u16 *channels, u16 *bits, u32 *rate) {
  if (!f || M16(f) != 1) return 0;
  *channels = M16(f + 2); *rate = M32(f + 4); *bits = M16(f + 14);
  return (*channels == 1 || *channels == 2) && (*bits == 8 || *bits == 16) && *rate >= 100 && *rate <= 200000;
}
// CreateSoundBuffer(DSBUFFERDESC *, IDirectSoundBuffer **, outer). DSBUFFERDESC: size, flags, bytes, reserved, format
IMP(dsound, IDirectSound_CreateSoundBuffer) {
  u64 desc = ARG(1), out = ARG(2);
  if (!desc || !out || (M32(desc) != 20 && M32(desc) != 36)) { RET(DSERR_INVALIDPARAM); return; }
  M32(out) = 0;
  u32 flags = M32(desc + 4), bytes = M32(desc + 8);
  if (flags & DSBCAPS_PRIMARYBUFFER) {
    Buffer *b = new_buffer(); b->primary = 1; b->flags = flags; b->playing = 1; b->looping = 1;
    M32(out) = (u32)(uptr)b; RET(DS_OK); return;
  }
  u16 channels, bits; u32 rate;
  if (!read_format(M32(desc + 16), &channels, &bits, &rate)) { com_unsure("DirectSound: a buffer of a format that is not 8- or 16-bit PCM in mono or stereo"); RET(DSERR_BADFORMAT); return; }
  if (bytes < 4 || bytes > 0x0FFFFFFF) { RET(DSERR_INVALIDPARAM); return; }
  if (flags & 0x10) { RET(DSERR_UNSUPPORTED); return; }   // DSBCAPS_CTRL3D: no 3D here
  if (rt_trace) rt_log("DirectSound: a buffer of %u bytes, %u Hz, %u bits, %u channels, flags 0x%x", bytes, rate, bits, channels, flags);
  Buffer *b = new_buffer();
  b->flags = (flags & ~0x4u) | DSBCAPS_LOCSOFTWARE; b->bytes = bytes; b->data = calloc(1, bytes);
  if (bits == 8) memset(b->data, 128, bytes);   // silence
  b->channels = channels; b->bits = bits; b->rate = rate; b->block = channels * bits / 8; b->freq = rate;
  pthread_mutex_lock(&mixer_lock); b->next = buffers; buffers = b; pthread_mutex_unlock(&mixer_lock);
  M32(out) = (u32)(uptr)b; RET(DS_OK);
}
IMP(dsound, IDirectSound_DuplicateSoundBuffer) { com_unsure("DirectSound: DuplicateSoundBuffer is not implemented"); if (ARG(2)) M32(ARG(2)) = 0; RET(DSERR_UNSUPPORTED); }

// ---- IDirectSoundBuffer ----
#define BUF ((Buffer *)GP(ARG(0)))
IMP(dsound, IDirectSoundBuffer_QueryInterface) {
  if (!ARG(2)) { RET(E_POINTER); return; }
  if (com_is_iid(ARG(1), IID_IDirectSoundBuffer_text) || com_is_iid(ARG(1), IID_IUnknown_text)) { BUF->refs++; M32(ARG(2)) = (u32)ARG(0); RET(DS_OK); return; }
  M32(ARG(2)) = 0; RET(E_NOINTERFACE);   // IDirectSound3DBuffer, IDirectSoundNotify: not here
}
IMP(dsound, IDirectSoundBuffer_AddRef) { RET(++BUF->refs); }
IMP(dsound, IDirectSoundBuffer_Release) {
  Buffer *b = BUF; u32 n = (u32)--b->refs;
  if (!n) {
    pthread_mutex_lock(&mixer_lock);
    for (Buffer **p = &buffers; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
    pthread_mutex_unlock(&mixer_lock);
    free(b->data); b->vtable = 0; free(b);
  }
  RET(n);
}
IMP(dsound, IDirectSoundBuffer_GetCaps) { // DSBCAPS: size 20, flags, bytes, transfer rate, CPU overhead
  Buffer *b = BUF; u64 p = ARG(1);
  if (!p || M32(p) < 20) { RET(DSERR_INVALIDPARAM); return; }
  M32(p + 4) = b->flags; M32(p + 8) = b->primary ? 4096 : b->bytes; M32(p + 12) = 0; M32(p + 16) = 0; RET(DS_OK);
}
// The write cursor runs 10 ms ahead of the play cursor: where it is safe to write
static u32 play_cursor(const Buffer *b) { return b->bytes ? ((u32)b->pos * b->block) % b->bytes : 0; }
static u32 write_cursor(const Buffer *b) { u32 lead = (b->freq ? b->freq : b->rate) / 100 * b->block; return b->bytes ? (play_cursor(b) + (b->playing ? lead : 0)) % b->bytes : 0; }
IMP(dsound, IDirectSoundBuffer_GetCurrentPosition) { // (&play, &write)
  Buffer *b = BUF;
  if (b->primary) { if (ARG(1)) M32(ARG(1)) = 0; if (ARG(2)) M32(ARG(2)) = 0; RET(DS_OK); return; }
  mixer_clock_moved();   // what is due by now is mixed: the position is the app's clock's
  pthread_mutex_lock(&mixer_lock);
  u32 pc = play_cursor(b), wc = write_cursor(b);
  pthread_mutex_unlock(&mixer_lock);
  if (ARG(1)) M32(ARG(1)) = pc; if (ARG(2)) M32(ARG(2)) = wc;
  RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_GetFormat) { // (WAVEFORMATEX *, size, &written)
  Buffer *b = BUF; u64 f = ARG(1);
  u16 ch = b->primary ? primary_format.channels : b->channels, bits = b->primary ? primary_format.bits : b->bits; u32 rate = b->primary ? primary_format.rate : b->rate;
  if (ARG(3)) M32(ARG(3)) = 18;
  if (!f) { RET(DS_OK); return; }
  if ((u32)ARG(2) < 16) { RET(DSERR_INVALIDPARAM); return; }
  M16(f) = 1; M16(f + 2) = ch; M32(f + 4) = rate; M32(f + 8) = rate * ch * bits / 8; M16(f + 12) = (u16)(ch * bits / 8); M16(f + 14) = bits;
  if ((u32)ARG(2) >= 18) M16(f + 16) = 0;
  RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_SetFormat) {
  Buffer *b = BUF; u16 ch, bits; u32 rate;
  if (!b->primary) { RET(DSERR_INVALIDCALL); return; }
  if (!read_format(ARG(1), &ch, &bits, &rate)) { RET(DSERR_BADFORMAT); return; }
  primary_format.channels = ch; primary_format.bits = bits; primary_format.rate = rate; RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_GetVolume) { if (ARG(1)) M32(ARG(1)) = (u32)BUF->volume; RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_GetPan) { if (ARG(1)) M32(ARG(1)) = (u32)BUF->pan; RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_GetFrequency) { if (ARG(1)) M32(ARG(1)) = BUF->freq; RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_SetVolume) { s32 v = (s32)ARG(1); if (rt_trace) rt_log("DirectSound: %s buffer volume %d", BUF->primary ? "the primary" : "a", v); if (v > 0 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } pthread_mutex_lock(&mixer_lock); BUF->volume = v; pthread_mutex_unlock(&mixer_lock); RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_SetPan) { s32 v = (s32)ARG(1); if (v > 10000 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } pthread_mutex_lock(&mixer_lock); BUF->pan = v; pthread_mutex_unlock(&mixer_lock); RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_SetFrequency) { // 0: the buffer's own rate
  Buffer *b = BUF; u32 f = (u32)ARG(1);
  if (b->primary) { RET(DSERR_INVALIDCALL); return; }
  if (f && (f < 100 || f > 200000)) { RET(DSERR_INVALIDPARAM); return; }
  pthread_mutex_lock(&mixer_lock); b->freq = f ? f : b->rate; pthread_mutex_unlock(&mixer_lock); RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_GetStatus) { Buffer *b = BUF; if (ARG(1)) M32(ARG(1)) = b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0) : 0; RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_Initialize) { RET(DSERR_INVALIDCALL); }   // made by CreateSoundBuffer: already initialised
// Lock(offset, bytes, &p1, &n1, &p2, &n2, flags): the bytes asked for, as one or two pieces (when they wrap)
IMP(dsound, IDirectSoundBuffer_Lock) {
  Buffer *b = BUF; u32 off = (u32)ARG(1), n = (u32)ARG(2), flags = (u32)ARG(7);
  if (b->primary) { RET(DSERR_INVALIDCALL); return; }
  if (flags & DSBLOCK_FROMWRITECURSOR) { mixer_clock_moved(); pthread_mutex_lock(&mixer_lock); off = write_cursor(b); pthread_mutex_unlock(&mixer_lock); }
  if (flags & DSBLOCK_ENTIREBUFFER) n = b->bytes;
  if (off >= b->bytes || !n || n > b->bytes || !ARG(3) || !ARG(4)) { RET(DSERR_INVALIDPARAM); return; }
  u32 first = n < b->bytes - off ? n : b->bytes - off;
  M32(ARG(3)) = (u32)(uptr)(b->data + off); M32(ARG(4)) = first;
  if (ARG(5)) M32(ARG(5)) = first < n ? (u32)(uptr)b->data : 0;
  if (ARG(6)) M32(ARG(6)) = n - first;
  RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_Unlock) { RET(DS_OK); }   // the bytes are where the mixer reads them
IMP(dsound, IDirectSoundBuffer_Play) { // (reserved, priority, flags)
  Buffer *b = BUF;
  if (b->primary) { RET(DS_OK); return; }
  mixer_clock_moved();
  pthread_mutex_lock(&mixer_lock); b->playing = 1; b->looping = ((u32)ARG(3) & DSBPLAY_LOOPING) != 0; pthread_mutex_unlock(&mixer_lock);
  RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_Stop) { Buffer *b = BUF; mixer_clock_moved(); pthread_mutex_lock(&mixer_lock); if (!b->primary) b->playing = 0; pthread_mutex_unlock(&mixer_lock); RET(DS_OK); }
IMP(dsound, IDirectSoundBuffer_SetCurrentPosition) {
  Buffer *b = BUF; u32 p = (u32)ARG(1);
  if (b->primary) { RET(DSERR_INVALIDCALL); return; }
  if (p >= b->bytes) { RET(DSERR_INVALIDPARAM); return; }
  pthread_mutex_lock(&mixer_lock); b->pos = p / b->block; pthread_mutex_unlock(&mixer_lock); RET(DS_OK);
}
IMP(dsound, IDirectSoundBuffer_Restore) { RET(DS_OK); }   // a buffer here is never lost
