// SPDX-License-Identifier: GPL-2.0-or-later
// FMOD 3 (fmod.dll) for apps whose recipe says the runtime answers for it ([dll] "fmod.dll" = "builtin"): the
// FSOUND functions Narbacular Drop imports, on the runtime's sound output (mixer.c). Sounds come from the app's
// files through its own file callbacks when it set them (FSOUND_File_SetCallbacks: called on the app's thread with
// rt_guest_call, the whole file read when it is loaded or opened), else from the app's file system; they are
// decoded by audio_decode.c (WAV, Ogg Vorbis, MP3). FMOD's functions are stdcall, exported under decorated names
// (_FSOUND_Init@12); the ones that return a truth value return it as a signed char, in AL.
//
// Measured on the oracle (Wine with a sound device, +snoop of the shipped fmod.dll: tools/oracle-wine/README.md):
// - every call returns TRUE here except FSOUND_Reverb_SetProperties (FALSE: no EAX);
// - channel handles are (use count << 12) | channel. Software channels are 0 .. the count given to FSOUND_Init - 1,
//   hardware ones follow (32 on: FMOD's DirectSound buffers). A sound made with FSOUND_HW3D or FSOUND_HW2D takes
//   the lowest free hardware channel, any other the lowest free software one; reserved channels are never taken.
//   A channel's use count grows when a sound starts on it and when FSOUND_StopSound stops one, not when a sound
//   ends by itself (0x1034, then 0x2034 for the next sound on channel 0x34; 0x1020, stopped, then 0x3020);
// - FSOUND_GetFrequency of a new channel is the file's rate.
// From FMOD 3's documentation: volumes 0-255, FSOUND_SetVolume scaled by FSOUND_SetSFXMasterVolume and
// FSOUND_SetVolumeAbsolute not; a sound's minimum distance is the distance factor when it was made (the factor is
// "units per metre"); priorities 0-255; with more than one listener the closest one is heard and there is no panning.
// Guessed (FMOD hands 3D channels to DirectSound, and the oracle's DirectSound is Wine's; not measured sound by
// sound): attenuation as DirectSound's, (minimum distance / distance) ^ rolloff factor between the minimum and
// maximum distances; equal-power panning by the cosine of the angle between the listener's right and the sound
// (-3 dB on each side in the middle); no Doppler shift, no reverb.
// Measured against Wine's recording (tools/oracle-wine/audio-compare.py): 2D sounds, stereo ones too, are panned with
// equal power (FSOUND_SetPan, 128 the middle): with 0 dB in the middle the first level's music, a stereo MP3 at
// volume 128 and most of what is heard there, came out 2.2 dB louder than under Wine.
// The mix: each channel resampled linearly from its frequency to the output's 48 kHz, summed. Streams that
// decode to at most 8 MB are decoded once when opened (shared by every stream of the same file), the others as they play.
#include <math.h>
#include <stdarg.h>
#include "win32.h"
#include "../mixer.h"
#include "../vfs.h"
#include "audio_decode.h"

enum { LOOP_OFF = 1, LOOP_NORMAL = 2, LOOP_BIDI = 4, HW3D = 0x1000, MODE_2D = 0x2000, LOADMEMORY = 0x8000, HW2D = 0x80000 };
enum { FREE = -1, ALL = -3 };
#define HW_FIRST 32        // FMOD's first hardware channel under the oracle (after its 32 software channels)
#define HW_COUNT 64        // how many hardware channels DirectSound gave it: at least 21 were used at once there
#define CHANNELS (HW_FIRST + HW_COUNT)
#define SMAGIC 0x464d4f44u

typedef struct Pcm { char name[96]; u32 file_size; s16 *data; u64 frames; int channels, rate, refs; struct Pcm *next; } Pcm;   // decoded whole
typedef struct Sound {
  u32 magic; int stream; u32 mode; char name[96];
  Pcm *pcm;                                  // decoded whole (every sample; short streams)
  u8 *file; u32 file_size; AudioFile *dec;   // decoded as it plays (long streams)
  int channels, rate;
  float min_distance, max_distance;
  s32 loop_count; u64 loop_start, loop_end;  // streams: FSOUND_Stream_SetLoopCount / SetLoopPoints (end 0: the end)
  int channel;                               // streams: the channel playing it, or -1
} Sound;
typedef struct Channel {
  int active, reserved; u32 count;
  Sound *snd;
  u64 at;                   // the next source frame to read
  float a[2], b[2]; double frac; int primed, ended;   // the two source frames the output falls between
  s16 buf[2 * 1024]; u32 buf_n, buf_at;   // decoded frames of a long stream not read yet
  s32 loops_left;
  int freq, volume, absolute, pan, paused, priority; u32 loop;
  int is3d, placed; float pos[3], vel[3];
  float gl, gr;             // 3D gains, worked out at FSOUND_Update
} Channel;

static Channel ch[CHANNELS]; static int software = 32, initialised;
static int sfx_master = 255;
static float distance_factor = 1, rolloff = 1;
static struct { float pos[3], vel[3], front[3], top[3]; int set; } listener[8]; static int listener_current, listener_count = 1;
static u32 cb_open, cb_close, cb_read, cb_seek, cb_tell;
static Pcm *pcms;
static int tracing = -1;

// ---- the trace (SW_FMOD_TRACE=1): one line a call, as tools/oracle-wine/fsound-compare.py reads it ----
static void trace(const char *fmt, ...) {
  if (tracing < 0) { const char *e = getenv("SW_FMOD_TRACE"); tracing = e && *e && *e != '0'; }
  if (!tracing) return;
  char line[256]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  rt_log("fmod: %s", line);
}
static const char *guest_string(u64 p) { return p ? (const char *)GP(p) : ""; }

// ---- files ----
// The whole of a file the app names: through its callbacks, or from its file system. Returns a malloc'd copy.
static u8 *load_file(Cpu *c, u64 name, u32 mode, u32 offset, u32 length, u32 *size) {
  if (mode & LOADMEMORY) { // the data itself, `length` bytes
    u8 *d = malloc(length ? length : 1); memcpy(d, (void *)GP(name + offset), length); *size = length; return d;
  }
  u32 cap = 1 << 16, n = 0; u8 *d = malloc(cap);
  if (cb_open) {
    u32 h = (u32)rt_guest_call(c, cb_open, name, 0, 0, 0);
    if (!h) { free(d); return 0; }
    if (offset) rt_guest_call(c, cb_seek, h, offset, 0, 0);
    for (;;) {
      u32 want = cap - n; if (length && want > length - n) want = length - n;
      if (!want) { if (length && n == length) break; cap *= 2; d = realloc(d, cap); continue; }
      s32 got = (s32)rt_guest_call(c, cb_read, (u64)(uptr)(d + n), want, h, 0);
      if (got <= 0) break;
      n += (u32)got;
      if ((u32)got < want) break;
    }
    rt_guest_call(c, cb_close, h, 0, 0, 0);
  } else {
    u32 err = 0; VFile *f = vfs_open(guest_string(name), 0x80000000u, 3, &err);
    if (!f) { free(d); return 0; }
    u64 total = vfs_size(f); *vfs_pos(f) = offset;
    n = (u32)(total > offset ? total - offset : 0); if (length && n > length) n = length;
    d = realloc(d, n ? n : 1); n = (u32)vfs_read(f, d, n); vfs_close(f);
  }
  *size = n; return d;
}
// A decoded copy of a whole file, shared by the sounds made from the same file
static Pcm *decode_whole(const char *name, u8 *file, u32 size, u64 limit) {
  for (Pcm *p = pcms; p; p = p->next) if (p->file_size == size && !strcmp(p->name, name)) { p->refs++; return p; }
  AudioFile *f = audio_open(file, size); if (!f) return 0;
  u64 frames = audio_length(f); int chs = audio_channels(f);
  if (!frames || frames * (u64)chs * 2 > limit) { audio_close(f); return 0; }
  Pcm *p = calloc(1, sizeof *p); snprintf(p->name, sizeof p->name, "%s", name); p->file_size = size;
  p->channels = chs; p->rate = audio_rate(f); p->data = malloc(frames * (u64)chs * 2 + 2);
  p->frames = audio_read(f, p->data, (u32)frames); audio_close(f);
  p->refs = 1; p->next = pcms; pcms = p;
  return p;
}
static void pcm_release(Pcm *p) {
  if (!p || --p->refs) return;
  for (Pcm **q = &pcms; *q; q = &(*q)->next) if (*q == p) { *q = p->next; break; }
  free(p->data); free(p);
}
static Sound *sound_of(u64 p) { Sound *s = (Sound *)GP(p); return p && s->magic == SMAGIC ? s : 0; }
static Sound *sound_new(Cpu *c, u64 name, u32 mode, u32 offset, u32 length, int stream) {
  u32 size = 0; u8 *file = load_file(c, name, mode, offset, length, &size);
  const char *label = mode & LOADMEMORY ? "(memory)" : guest_string(name);
  if (!file) { rt_log("fmod: %s: cannot read it", label); return 0; }
  Sound *s = calloc(1, sizeof *s); s->magic = SMAGIC; s->stream = stream; s->mode = mode; s->channel = -1; s->loop_count = -1;
  snprintf(s->name, sizeof s->name, "%s", label);
  // samples: decoded now; streams: decoded now when short (a loop), else as they play
  s->pcm = decode_whole(s->name, file, size, stream ? 8u << 20 : ~0ull);
  if (s->pcm) { free(file); s->channels = s->pcm->channels; s->rate = s->pcm->rate; }
  else if (stream && (s->dec = audio_open(file, size))) { s->file = file; s->file_size = size; s->channels = audio_channels(s->dec); s->rate = audio_rate(s->dec); }
  else { rt_log("fmod: %s: not a sound file audio_decode.c reads", label); free(file); free(s); return 0; }
  s->min_distance = distance_factor; s->max_distance = 1e9f;
  return s;
}
static void sound_free(Sound *s) { s->magic = 0; pcm_release(s->pcm); audio_close(s->dec); free(s->file); free(s); }

// ---- channels ----
static Channel *channel_of(s32 h) { // a valid handle -> its channel
  if (h < 0) return 0;
  u32 i = (u32)h & 0xFFF; if (i >= CHANNELS) return 0;
  Channel *k = &ch[i]; if (i < HW_FIRST && i >= (u32)software) return 0;
  return k->count == ((u32)h >> 12) ? k : 0;
}
static s32 handle_of(Channel *k) { return (s32)((k->count << 12) | (u32)(k - ch)); }
static void stop(Channel *k) { if (k->snd && k->snd->stream && k->snd->channel == (int)(k - ch)) k->snd->channel = -1; k->active = 0; k->snd = 0; }
// The channel for a new sound: the one asked for, or the lowest free one of its kind, or the one of lowest priority
static Channel *channel_for(s32 want, Sound *s) {
  if (want >= 0) { u32 i = (u32)want & 0xFFF; return i < CHANNELS && (i < (u32)software || i >= HW_FIRST) ? &ch[i] : 0; }
  int hw = (s->mode & (HW3D | HW2D)) != 0; u32 lo = hw ? HW_FIRST : 0, hi = hw ? CHANNELS : (u32)software;
  for (u32 i = lo; i < hi; i++) if (!ch[i].active && !ch[i].reserved) return &ch[i];
  Channel *low = 0;
  for (u32 i = lo; i < hi; i++) if (!ch[i].reserved && (!low || ch[i].priority < low->priority)) low = &ch[i];
  return low && low->priority <= 128 ? low : 0;
}
static s32 play(Cpu *c, s32 want, Sound *s, int paused) {
  (void)c;
  Channel *k = channel_for(want, s);
  if (!k) return -1;
  if (k->active) stop(k);
  if (s->stream && s->channel >= 0) stop(&ch[s->channel]);   // a stream plays once at a time
  k->active = 1; k->count++; k->snd = s; k->at = 0; k->primed = 0; k->ended = 0; k->frac = 0; k->buf_n = k->buf_at = 0;
  k->freq = s->rate; k->volume = 255; k->absolute = 0; k->pan = 128; k->paused = paused; k->priority = 128;
  k->loop = s->mode & (LOOP_OFF | LOOP_NORMAL | LOOP_BIDI) ? s->mode & (LOOP_OFF | LOOP_NORMAL | LOOP_BIDI) : LOOP_OFF;
  k->loops_left = s->stream ? s->loop_count : -1;
  k->is3d = !(s->mode & (MODE_2D | HW2D)); k->placed = 0; k->gl = k->gr = 1;
  memset(k->pos, 0, sizeof k->pos); memset(k->vel, 0, sizeof k->vel);
  if (s->stream) { s->channel = (int)(k - ch); if (s->dec) audio_seek(s->dec, 0); }
  return handle_of(k);
}

// ---- 3D ----
static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void gains_3d(Channel *k) {
  if (!k->is3d) { k->gl = k->gr = 1; return; }
  int best = 0; float best_d = -1, d3[3];
  for (int i = 0; i < listener_count && i < 8; i++) { // the closest listener
    float v[3] = {k->pos[0] - listener[i].pos[0], k->pos[1] - listener[i].pos[1], k->pos[2] - listener[i].pos[2]};
    float d = sqrtf(dot(v, v)); if (best_d < 0 || d < best_d) { best_d = d; best = i; memcpy(d3, v, sizeof v); }
  }
  float min = k->snd ? k->snd->min_distance : 1, max = k->snd ? k->snd->max_distance : 1e9f, d = best_d;
  if (d > max) d = max; if (d < min) d = min;
  float att = min > 0 ? powf(min / d, rolloff) : 1;
  float side = 0;   // -1 the listener's left .. 1 its right: the cosine of the angle between its right (top x front) and the sound
  if (listener_count == 1 && best_d > 0) {
    const float *f = listener[best].front, *t = listener[best].top;
    float right[3] = {t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0]}, rl = sqrtf(dot(right, right));
    if (rl > 0) { side = dot(right, d3) / (rl * best_d); if (side > 1) side = 1; if (side < -1) side = -1; }
  }
  k->gl = att * sqrtf((1 - side) / 2); k->gr = att * sqrtf((1 + side) / 2);   // equal power: -3 dB each in the middle
}

// ---- the mix (the mixer's source) ----
static int next_frame(Channel *k, float *out) { // the next source frame; 0 at the end
  Sound *s = k->snd; int chs = s->channels;
  for (int tries = 0; tries < 2; tries++) {
    u64 end = s->pcm ? s->pcm->frames : ~0ull;
    if (s->stream && s->loop_end && s->loop_end < end) end = s->loop_end + 1;
    if (s->pcm) {
      if (k->at < end) { const s16 *p = s->pcm->data + k->at * (u64)chs; out[0] = p[0] / 32768.0f; out[1] = chs > 1 ? p[1] / 32768.0f : out[0]; k->at++; return 1; }
    } else {
      if (k->buf_at == k->buf_n && k->at < end) { k->buf_n = audio_read(s->dec, k->buf, (u32)(sizeof k->buf / sizeof k->buf[0]) / (u32)chs); k->buf_at = 0; }
      if (k->buf_at < k->buf_n && k->at < end) { const s16 *p = k->buf + k->buf_at * (u32)chs; out[0] = p[0] / 32768.0f; out[1] = chs > 1 ? p[1] / 32768.0f : out[0]; k->buf_at++; k->at++; return 1; }
    }
    // the end: loop, or stop
    if (k->loop & (LOOP_NORMAL | LOOP_BIDI) && k->loops_left != 0) {
      if (k->loops_left > 0) k->loops_left--;
      u64 start = s->stream ? s->loop_start : 0;
      k->at = start; k->buf_n = k->buf_at = 0; if (s->dec) audio_seek(s->dec, start);
      continue;
    }
    return 0;
  }
  return 0;
}
static void mix(Cpu *c, float *out, u32 frames, void *ctx) {
  (void)c; (void)ctx;
  for (int i = 0; i < CHANNELS; i++) {
    Channel *k = &ch[i];
    if (!k->active || k->paused || !k->snd || (i >= software && i < HW_FIRST)) continue;
    float amp = k->volume / 255.0f * (k->absolute ? 1.0f : sfx_master / 255.0f);
    float gl = amp * k->gl, gr = amp * k->gr;
    if (!k->is3d) { // FSOUND_SetPan: 0 left, 128 middle, 255 right; equal power, for stereo sounds too
      float p = k->pan / 255.0f * (float)M_PI / 2; gl *= cosf(p); gr *= sinf(p);
    }
    double step = (double)k->freq / MIXER_RATE;
    if (!k->primed) { k->primed = 1; if (!next_frame(k, k->a)) { stop(k); continue; } if (!next_frame(k, k->b)) { k->b[0] = k->a[0]; k->b[1] = k->a[1]; k->ended = 1; } }
    for (u32 n = 0; n < frames; n++) {
      float t = (float)k->frac, l = k->a[0] + (k->b[0] - k->a[0]) * t, r = k->a[1] + (k->b[1] - k->a[1]) * t;
      out[2 * n] += l * gl; out[2 * n + 1] += r * gr;
      k->frac += step;
      while (k->frac >= 1) {
        k->frac -= 1;
        if (k->ended) { stop(k); break; }
        k->a[0] = k->b[0]; k->a[1] = k->b[1];
        if (!next_frame(k, k->b)) { k->b[0] = k->a[0]; k->b[1] = k->a[1]; k->ended = 1; }
      }
      if (!k->active) break;
    }
  }
}

// ---- set-up ----
// (window): the window DirectSound would attach to
IMP(fmod, FSOUND_SetHWND) { trace("SetHWND(%08x) = 1", (u32)ARG(0)); RET(1); }
IMP(fmod, FSOUND_SetMinHardwareChannels) { trace("SetMinHardwareChannels(%08x) = 1", (u32)ARG(0)); RET(1); }
// (pool, pool length, allocate, reallocate, free): FMOD would take its memory from the pool. Ours is the runtime's.
IMP(fmod, FSOUND_SetMemorySystem) { trace("SetMemorySystem(%08x,%08x,%08x,%08x,%08x) = 1", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4)); RET(1); }
IMP(fmod, FSOUND_Init) { // (mix rate, software channels, flags)
  u32 n = (u32)ARG(1);
  software = n < 1 ? 1 : n > HW_FIRST ? HW_FIRST : (int)n;
  if (!initialised) { initialised = 1; mixer_add_source(mix, 0); mixer_start(); }
  trace("Init(%08x,%08x,%08x) = 1", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2));
  RET(1);
}
IMP(fmod, FSOUND_Close) {
  trace("Close() = 0");
  pthread_mutex_lock(&mixer_lock);
  for (int i = 0; i < CHANNELS; i++) if (ch[i].active) stop(&ch[i]);
  pthread_mutex_unlock(&mixer_lock);
}
// (open, close, read, seek, tell): the app's file functions, stdcall
IMP(fmod, FSOUND_File_SetCallbacks) {
  cb_open = (u32)ARG(0); cb_close = (u32)ARG(1); cb_read = (u32)ARG(2); cb_seek = (u32)ARG(3); cb_tell = (u32)ARG(4);
  if (!cb_open || !cb_close || !cb_read || !cb_seek || !cb_tell) cb_open = cb_close = cb_read = cb_seek = cb_tell = 0;   // all or none
  trace("File_SetCallbacks(%08x,%08x,%08x,%08x,%08x) = 1", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4));
  RET(1);
}
IMP(fmod, FSOUND_SetSFXMasterVolume) { pthread_mutex_lock(&mixer_lock); sfx_master = (int)(s32)ARG(0) < 0 ? 0 : (int)ARG(0) > 255 ? 255 : (int)ARG(0); pthread_mutex_unlock(&mixer_lock); trace("SetSFXMasterVolume(%08x) = 0", (u32)ARG(0)); }
IMP(fmod, FSOUND_Reverb_SetProperties) { trace("Reverb_SetProperties(%08x) = 0", (u32)ARG(0)); RET(0); }   // no reverb (no EAX on the oracle either)
static float farg(Cpu *c, int n) { u32 v = (u32)ARG(n); float f; memcpy(&f, &v, 4); return f; }
IMP(fmod, FSOUND_3D_SetDistanceFactor) { distance_factor = farg(c, 0); trace("3D_SetDistanceFactor(%08x) = 0", (u32)ARG(0)); }
IMP(fmod, FSOUND_3D_SetDopplerFactor) { trace("3D_SetDopplerFactor(%08x) = 0", (u32)ARG(0)); }
IMP(fmod, FSOUND_3D_SetRolloffFactor) { pthread_mutex_lock(&mixer_lock); rolloff = farg(c, 0); pthread_mutex_unlock(&mixer_lock); trace("3D_SetRolloffFactor(%08x) = 0", (u32)ARG(0)); }

// ---- samples and streams ----
// Sample_Load(index, name or data, mode, offset, length) -> the sample
IMP(fmod, FSOUND_Sample_Load) {
  Sound *s = sound_new(c, ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), 0);
  trace("Sample_Load(%08x,%08x \"%s\",%08x,%08x,%08x) = %08x", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2) & LOADMEMORY ? "" : guest_string(ARG(1)), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), (u32)(uptr)s);
  RET((u32)(uptr)s);
}
// Stream_Open(name or data, mode, offset, length) -> the stream
IMP(fmod, FSOUND_Stream_Open) {
  Sound *s = sound_new(c, ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), 1);
  trace("Stream_Open(%08x \"%s\",%08x,%08x,%08x) = %08x", (u32)ARG(0), (u32)ARG(1) & LOADMEMORY ? "" : guest_string(ARG(0)), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)(uptr)s);
  RET((u32)(uptr)s);
}
IMP(fmod, FSOUND_Stream_Close) {
  Sound *s = sound_of(ARG(0));
  trace("Stream_Close(%08x) = %d", (u32)ARG(0), s ? 1 : 0);
  if (!s || !s->stream) { RET(0); return; }
  pthread_mutex_lock(&mixer_lock); if (s->channel >= 0) stop(&ch[s->channel]); sound_free(s); pthread_mutex_unlock(&mixer_lock);
  RET(1);
}
IMP(fmod, FSOUND_Stream_SetLoopCount) { // (stream, count): how many more times it loops; -1 for ever
  Sound *s = sound_of(ARG(0)); trace("Stream_SetLoopCount(%08x,%08x) = %d", (u32)ARG(0), (u32)ARG(1), s ? 1 : 0);
  if (!s) { RET(0); return; }
  pthread_mutex_lock(&mixer_lock); s->loop_count = (s32)ARG(1); if (s->channel >= 0) ch[s->channel].loops_left = s->loop_count; pthread_mutex_unlock(&mixer_lock);
  RET(1);
}
IMP(fmod, FSOUND_Stream_SetLoopPoints) { // (stream, first frame, last frame)
  Sound *s = sound_of(ARG(0)); trace("Stream_SetLoopPoints(%08x,%08x,%08x) = %d", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), s ? 1 : 0);
  if (!s) { RET(0); return; }
  pthread_mutex_lock(&mixer_lock); s->loop_start = (u32)ARG(1); s->loop_end = (u32)ARG(2); pthread_mutex_unlock(&mixer_lock);
  RET(1);
}
// PlaySoundEx(channel, sample, dsp unit, start paused) -> the channel's handle, or -1
IMP(fmod, FSOUND_PlaySoundEx) {
  Sound *s = sound_of(ARG(1)); s32 h = -1;
  pthread_mutex_lock(&mixer_lock); if (s && !s->stream) h = play(c, (s32)ARG(0), s, (s8)ARG(3) != 0); pthread_mutex_unlock(&mixer_lock);
  trace("PlaySoundEx(%08x,%08x,%08x,%08x) = %08x", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)h);
  RET((u32)h);
}
IMP(fmod, FSOUND_Stream_PlayEx) {
  Sound *s = sound_of(ARG(1)); s32 h = -1;
  pthread_mutex_lock(&mixer_lock); if (s && s->stream) h = play(c, (s32)ARG(0), s, (s8)ARG(3) != 0); pthread_mutex_unlock(&mixer_lock);
  trace("Stream_PlayEx(%08x,%08x,%08x,%08x) = %08x", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)h);
  RET((u32)h);
}

// ---- channel settings: each (handle or FSOUND_ALL, value) and TRUE if the handle was valid ----
#define EACH(name, body) \
  s32 h = (s32)ARG(0); int ok = 0; \
  pthread_mutex_lock(&mixer_lock); \
  for (int i = 0; i < CHANNELS; i++) { Channel *k = &ch[i]; if (h == ALL ? !k->active : k != channel_of(h)) continue; ok = 1; body; } \
  pthread_mutex_unlock(&mixer_lock); \
  trace(name "(%08x,%08x) = %d", (u32)ARG(0), (u32)ARG(1), ok); RET(ok);
IMP(fmod, FSOUND_StopSound) {
  s32 h = (s32)ARG(0); int ok = 0;
  pthread_mutex_lock(&mixer_lock);
  for (int i = 0; i < CHANNELS; i++) { Channel *k = &ch[i]; if (h == ALL ? !k->active : k != channel_of(h)) continue; ok = 1; if (k->active) { stop(k); k->count++; } }
  pthread_mutex_unlock(&mixer_lock);
  trace("StopSound(%08x) = %d", (u32)ARG(0), ok); RET(ok);
}
IMP(fmod, FSOUND_SetVolume) { int v = (int)(s32)ARG(1); v = v < 0 ? 0 : v > 255 ? 255 : v; EACH("SetVolume", { k->volume = v; k->absolute = 0; }) }
IMP(fmod, FSOUND_SetVolumeAbsolute) { int v = (int)(s32)ARG(1); v = v < 0 ? 0 : v > 255 ? 255 : v; EACH("SetVolumeAbsolute", { k->volume = v; k->absolute = 1; }) }
IMP(fmod, FSOUND_SetPan) { int v = (int)(s32)ARG(1); v = v < 0 ? 128 : v > 255 ? 255 : v; EACH("SetPan", { k->pan = v; }) }   // FSOUND_STEREOPAN (-1): the middle
IMP(fmod, FSOUND_SetPaused) { int v = (s8)ARG(1) != 0; EACH("SetPaused", { if (!v && k->is3d && !k->placed) gains_3d(k); k->paused = v; }) }
IMP(fmod, FSOUND_SetFrequency) { int v = (int)(s32)ARG(1); EACH("SetFrequency", { if (v > 0) k->freq = v; }) }
IMP(fmod, FSOUND_SetLoopMode) { u32 v = (u32)ARG(1); EACH("SetLoopMode", { k->loop = v; }) }
IMP(fmod, FSOUND_SetPriority) { int v = (int)(s32)ARG(1); EACH("SetPriority", { k->priority = v; }) }
IMP(fmod, FSOUND_SetReserved) { int v = (s8)ARG(1) != 0; EACH("SetReserved", { k->reserved = v; }) }
// SetCurrentPosition(channel, frame)
IMP(fmod, FSOUND_SetCurrentPosition) {
  u32 v = (u32)ARG(1);
  EACH("SetCurrentPosition", { k->at = v; k->primed = 0; k->ended = 0; k->frac = 0; k->buf_n = k->buf_at = 0; if (k->snd && k->snd->dec) audio_seek(k->snd->dec, v); })
}
IMP(fmod, FSOUND_GetFrequency) {
  pthread_mutex_lock(&mixer_lock); Channel *k = channel_of((s32)ARG(0)); s32 f = k ? k->freq : 0; pthread_mutex_unlock(&mixer_lock);
  trace("GetFrequency(%08x) = %08x", (u32)ARG(0), (u32)f); RET((u32)f);
}

// ---- 3D ----
// 3D_SetAttributes(channel, position or null, velocity or null)
IMP(fmod, FSOUND_3D_SetAttributes) {
  s32 h = (s32)ARG(0); u64 p = ARG(1), v = ARG(2); int ok = 0;
  pthread_mutex_lock(&mixer_lock);
  Channel *k = channel_of(h);
  if (k) { ok = 1; if (p) memcpy(k->pos, (void *)GP(p), 12); if (v) memcpy(k->vel, (void *)GP(v), 12); if (!k->placed) gains_3d(k); }
  pthread_mutex_unlock(&mixer_lock);
  trace("3D_SetAttributes(%08x,%08x,%08x) = %d", (u32)h, (u32)p, (u32)v, ok); RET(ok);
}
// 3D_Listener_SetAttributes(position, velocity, front x, y, z, top x, y, z): the current listener
IMP(fmod, FSOUND_3D_Listener_SetAttributes) {
  pthread_mutex_lock(&mixer_lock);
  int i = listener_current & 7;
  if (ARG(0)) memcpy(listener[i].pos, (void *)GP(ARG(0)), 12);
  if (ARG(1)) memcpy(listener[i].vel, (void *)GP(ARG(1)), 12);
  for (int j = 0; j < 3; j++) { listener[i].front[j] = farg(c, 2 + j); listener[i].top[j] = farg(c, 5 + j); }
  listener[i].set = 1;
  pthread_mutex_unlock(&mixer_lock);
  trace("3D_Listener_SetAttributes(%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x) = 0", (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(6), (u32)ARG(7));
}
// 3D_Listener_SetCurrent(current, how many): returns nothing
IMP(fmod, FSOUND_3D_Listener_SetCurrent) {
  pthread_mutex_lock(&mixer_lock);
  listener_current = (int)ARG(0); listener_count = (int)ARG(1) < 1 ? 1 : (int)ARG(1) > 8 ? 8 : (int)ARG(1);
  pthread_mutex_unlock(&mixer_lock);
  trace("3D_Listener_SetCurrent(%08x,%08x) = 0", (u32)ARG(0), (u32)ARG(1));
}
// Update: what changed in 3D takes effect
IMP(fmod, FSOUND_Update) {
  pthread_mutex_lock(&mixer_lock);
  for (int i = HW_FIRST; i < CHANNELS; i++) if (ch[i].active && ch[i].is3d) { gains_3d(&ch[i]); ch[i].placed = 1; }
  for (int i = 0; i < software; i++) if (ch[i].active && ch[i].is3d) { gains_3d(&ch[i]); ch[i].placed = 1; }
  pthread_mutex_unlock(&mixer_lock);
  trace("Update() = 0");
}
