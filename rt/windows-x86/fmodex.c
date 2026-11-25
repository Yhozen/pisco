// SPDX-License-Identifier: GPL-2.0-or-later
// FMOD Ex (fmodex.dll) for titles whose recipe says the runtime answers for it ([dll] "fmodex.dll" = "builtin"): the
// functions Tag imports (docs/tag.md), on the runtime's sound output (mixer.c), with files decoded by audio_decode.c
// (WAV, Ogg Vorbis, MP3). FMOD Ex's C++ API is a set of non-virtual stdcall methods exported under their decorated
// names, `this` the first argument on the stack; its System, Sound and Channel pointers are opaque to the title.
// From FMOD Ex's documentation: volumes are floats from 0 to 1; a channel's handle stops being valid when its sound
// ends or is stopped, and calls on it then fail with FMOD_ERR_INVALID_HANDLE (isPlaying says false); playSound with
// FMOD_CHANNEL_FREE (-1) takes a free channel, FMOD_CHANNEL_REUSE (-2) the one the handle names; loop counts are -1
// (for ever), 0 (play once) or how many more times; 3D sounds fade with distance as FMOD's default "logarithmic"
// roll-off, min / (min + rolloff * (distance - min)) beyond the minimum distance (1 metre times the distance factor).
// Guessed, as for FMOD 3 (rt/windows-x86/fmod3.c): equal-power panning by the listener's right, no Doppler shift.
// The mix: each channel resampled linearly to the output's 48 kHz, summed. Files that decode to at most 8 MB are
// decoded once (shared by every sound made from the same file); longer streams are decoded as they play.
#include <math.h>
#include "../rt.h"
#include "../mixer.h"
#include "../vfs.h"
#include "audio_decode.h"

enum { FMOD_OK = 0, FMOD_ERR_FILE_NOTFOUND = 23, FMOD_ERR_FORMAT = 25, FMOD_ERR_INVALID_HANDLE = 36, FMOD_ERR_INVALID_PARAM = 37 };
enum { MODE_LOOP_OFF = 1, MODE_LOOP_NORMAL = 2, MODE_LOOP_BIDI = 4, MODE_2D = 8, MODE_3D = 0x10, MODE_CREATESTREAM = 0x80, MODE_OPENMEMORY = 0x800 };
enum { TIMEUNIT_MS = 1, TIMEUNIT_PCM = 2, TIMEUNIT_PCMBYTES = 4 };
enum { CHANNELS = 64 };

typedef struct Pcm { char name[96]; u32 file_size; s16 *data; u64 frames; int channels, rate, refs; struct Pcm *next; } Pcm;
typedef struct Sound {
  u32 magic; char name[96]; u32 mode; int loop_count;
  Pcm *pcm; AudioFile *dec; u8 *file; int channels, rate; u64 frames;
} Sound;
#define SMAGIC 0x46584E44u
typedef struct Channel {
  int active, paused, ended, primed, done; u32 count, mode; Sound *snd;   // done: played to its end, its handle still valid
  u64 at; double frac; float a[2], b[2]; s16 buf[4096]; u32 buf_n, buf_at;
  float volume, gl, gr, pos[3], vel[3]; int is3d, loops_left;
} Channel;
static Channel ch[CHANNELS]; static int channel_limit = 32, started;
static Pcm *pcms;
static float distance_factor = 1, rolloff = 1;
static struct { float pos[3], vel[3], front[3], top[3]; } listener = {{0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}};
static u32 system_obj;

static float farg(Cpu *c, int n) { u32 v = (u32)ARG(n); float f; memcpy(&f, &v, 4); return f; }
static void vec(u64 p, float *out) { if (p) { out[0] = MF32(p); out[1] = MF32(p + 4); out[2] = MF32(p + 8); } }

// ---- sounds ----
static Pcm *decode_whole(const char *name, const u8 *file, u32 size, u64 limit) {
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
// (name or data, mode, FMOD_CREATESOUNDEXINFO * or null, Sound **)
static u32 create(Cpu *c, u32 extra_mode) {
  u64 name = ARG(1), info = ARG(3), out = ARG(4); u32 mode = (u32)ARG(2) | extra_mode, size = 0; u8 *file;
  if (!name || !out) return FMOD_ERR_INVALID_PARAM;
  M32(out) = 0;
  if (mode & MODE_OPENMEMORY) { size = info ? M32(info + 4) : 0; if (!size) return FMOD_ERR_INVALID_PARAM; file = malloc(size); memcpy(file, (void *)GP(name), size); }
  else {
    u32 err = 0; VFile *f = vfs_open(GSTR(name), 0x80000000u, 3, &err);
    if (!f) { rt_log("fmodex: %s: not found", GSTR(name)); return FMOD_ERR_FILE_NOTFOUND; }
    size = (u32)vfs_size(f); file = malloc(size ? size : 1); size = (u32)vfs_read(f, file, size); vfs_close(f);
  }
  const char *label = mode & MODE_OPENMEMORY ? "(memory)" : GSTR(name);
  Sound *s = calloc(1, sizeof *s); s->magic = SMAGIC; s->mode = mode; s->loop_count = -1;
  // getName answers the file's name without its folders, as FMOD Ex does (Wine: "softsplat1.mp3" for
  // ".\\assets\\sounds\\effects\\softsplat1.mp3"): Tag looks a sound up by it before loading it again
  const char *base = label; for (const char *p = label; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
  snprintf(s->name, sizeof s->name, "%s", base);
  s->pcm = decode_whole(label, file, size, mode & MODE_CREATESTREAM ? 8u << 20 : ~0ull);
  if (s->pcm) { free(file); s->channels = s->pcm->channels; s->rate = s->pcm->rate; s->frames = s->pcm->frames; }
  else if ((s->dec = audio_open(file, size))) { s->file = file; s->channels = audio_channels(s->dec); s->rate = audio_rate(s->dec); s->frames = audio_length(s->dec); }
  else { rt_log("fmodex: %s: not a sound file audio_decode.c reads", label); free(file); free(s); return FMOD_ERR_FORMAT; }
  M32(out) = (u32)(uptr)s; return FMOD_OK;
}

// ---- channels ----
static Channel *channel_of(u64 h) {   // a handle that is still valid -> its channel
  u32 i = (u32)h & 0xFFF; if (!h || i >= CHANNELS) return 0;
  Channel *k = &ch[i]; return k->active && k->count == ((u32)h >> 12) ? k : 0;
}
static u32 handle_of(Channel *k) { return (k->count << 12) | (u32)(k - ch); }
static void stop(Channel *k) { k->active = 0; k->snd = 0; }
// A channel whose sound has played to its end keeps its handle, as Tag's fmodex.dll does under Wine (isPlaying
// answers false with FMOD_OK, never FMOD_ERR_INVALID_HANDLE, in a whole session): until Channel::stop, or until
// playSound takes the channel for another sound (ended channels go after the ones never used or stopped).
static void finish(Channel *k) { k->done = 1; }

// ---- 3D ----
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void gains(Channel *k) {
  if (!k->is3d) { k->gl = k->gr = 1; return; }
  float v[3] = {k->pos[0] - listener.pos[0], k->pos[1] - listener.pos[1], k->pos[2] - listener.pos[2]}, d = sqrtf(dot3(v, v));
  float min = distance_factor, max = 10000 * distance_factor, dd = d < min ? min : d > max ? max : d;
  float att = min / (min + rolloff * (dd - min));
  const float *f = listener.front, *t = listener.top;
  float right[3] = {t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0]}, rl = sqrtf(dot3(right, right)), side = 0;
  if (rl > 0 && d > 0) { side = dot3(right, v) / (rl * d); if (side > 1) side = 1; if (side < -1) side = -1; }
  k->gl = att * sqrtf((1 - side) / 2); k->gr = att * sqrtf((1 + side) / 2);
}

// ---- the mix ----
static int next_frame(Channel *k, float *out) {
  Sound *s = k->snd; int chs = s->channels;
  for (int tries = 0; tries < 2; tries++) {
    if (s->pcm) {
      if (k->at < s->pcm->frames) { const s16 *p = s->pcm->data + k->at * (u64)chs; out[0] = p[0] / 32768.0f; out[1] = chs > 1 ? p[1] / 32768.0f : out[0]; k->at++; return 1; }
    } else {
      if (k->buf_at == k->buf_n) { k->buf_n = audio_read(s->dec, k->buf, (u32)(sizeof k->buf / sizeof k->buf[0]) / (u32)chs); k->buf_at = 0; }
      if (k->buf_at < k->buf_n) { const s16 *p = k->buf + k->buf_at * (u32)chs; out[0] = p[0] / 32768.0f; out[1] = chs > 1 ? p[1] / 32768.0f : out[0]; k->buf_at++; k->at++; return 1; }
    }
    if ((k->mode & (MODE_LOOP_NORMAL | MODE_LOOP_BIDI)) && k->loops_left != 0) {   // the end: once more, or stop
      if (k->loops_left > 0) k->loops_left--;
      k->at = 0; k->buf_n = k->buf_at = 0; if (s->dec) audio_seek(s->dec, 0);
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
    if (!k->active || k->done || k->paused || !k->snd) continue;
    float gl = k->volume * k->gl, gr = k->volume * k->gr;
    if (!k->is3d) { gl *= (float)M_SQRT1_2; gr *= (float)M_SQRT1_2; }   // a 2D sound in the middle: equal power
    double step = (double)k->snd->rate / MIXER_RATE;
    if (!k->primed) { k->primed = 1; if (!next_frame(k, k->a)) { finish(k); continue; } if (!next_frame(k, k->b)) { k->b[0] = k->a[0]; k->b[1] = k->a[1]; k->ended = 1; } }
    for (u32 n = 0; n < frames; n++) {
      float t = (float)k->frac;
      out[2 * n] += (k->a[0] + (k->b[0] - k->a[0]) * t) * gl; out[2 * n + 1] += (k->a[1] + (k->b[1] - k->a[1]) * t) * gr;
      k->frac += step;
      while (k->frac >= 1) {
        k->frac -= 1;
        if (k->ended) { finish(k); break; }
        k->a[0] = k->b[0]; k->a[1] = k->b[1];
        if (!next_frame(k, k->b)) { k->b[0] = k->a[0]; k->b[1] = k->a[1]; k->ended = 1; }
      }
      if (k->done) break;
    }
  }
}

// ---- System ----
IMP(fmodex, FMOD_System_Create) { // (FMOD_SYSTEM **)
  if (!ARG(0)) { RET(FMOD_ERR_INVALID_PARAM); return; }
  if (!system_obj) system_obj = (u32)(uptr)calloc(1, 16);
  M32(ARG(0)) = system_obj; RET(FMOD_OK);
}
IMP(fmodex, System_init) { // (this, max channels, flags, extra driver data)
  s32 n = (s32)ARG(1); channel_limit = n < 1 ? 1 : n > CHANNELS ? CHANNELS : n;
  if (!started) { started = 1; mixer_add_source(mix, 0); mixer_start(); }
  RET(FMOD_OK);
}
IMP(fmodex, System_release) { pthread_mutex_lock(&mixer_lock); for (int i = 0; i < CHANNELS; i++) stop(&ch[i]); pthread_mutex_unlock(&mixer_lock); RET(FMOD_OK); }
IMP(fmodex, System_update) { RET(FMOD_OK); }   // the mixer runs on its own (or with the title's clock)
IMP(fmodex, System_set3DSettings) { // (this, Doppler scale, distance factor, roll-off scale)
  pthread_mutex_lock(&mixer_lock);
  float df = farg(c, 2), r = farg(c, 3); if (df > 0) distance_factor = df; if (r >= 0) rolloff = r;
  for (int i = 0; i < CHANNELS; i++) if (ch[i].active) gains(&ch[i]);
  pthread_mutex_unlock(&mixer_lock); RET(FMOD_OK);
}
IMP(fmodex, System_set3DListenerAttributes) { // (this, listener, position, velocity, forward, up): any may be null
  if ((s32)ARG(1) != 0) { RET(FMOD_OK); return; }   // one listener
  pthread_mutex_lock(&mixer_lock);
  vec(ARG(2), listener.pos); vec(ARG(3), listener.vel); vec(ARG(4), listener.front); vec(ARG(5), listener.top);
  for (int i = 0; i < CHANNELS; i++) if (ch[i].active) gains(&ch[i]);
  pthread_mutex_unlock(&mixer_lock); RET(FMOD_OK);
}
IMP(fmodex, System_createSound) { RET(create(c, 0)); }
IMP(fmodex, System_createStream) { RET(create(c, MODE_CREATESTREAM)); }
IMP(fmodex, System_playSound) { // (this, channel index: -1 free, -2 reuse *out; Sound *, paused, Channel **)
  s32 want = (s32)ARG(1); Sound *s = sound_of(ARG(2)); int paused = (u8)ARG(3) != 0; u64 out = ARG(4);
  if (!s || !out) { RET(FMOD_ERR_INVALID_PARAM); return; }
  pthread_mutex_lock(&mixer_lock);
  Channel *k = 0;
  if (want == -2) k = channel_of(M32(out));
  else if (want >= 0 && want < channel_limit) k = &ch[want];
  if (!k) for (int i = 0; i < channel_limit; i++) if (!ch[i].active) { k = &ch[i]; break; }
  if (!k) for (int i = 0; i < channel_limit; i++) if (ch[i].done) { k = &ch[i]; break; }
  if (!k) k = &ch[0];   // all busy: the first is stolen
  stop(k);
  k->active = 1; k->count++; k->snd = s; k->at = 0; k->primed = 0; k->ended = 0; k->done = 0; k->frac = 0; k->buf_n = k->buf_at = 0;
  k->paused = paused; k->volume = 1; k->mode = s->mode; k->loops_left = s->loop_count;
  k->is3d = (s->mode & MODE_3D) != 0; memset(k->pos, 0, sizeof k->pos); memset(k->vel, 0, sizeof k->vel); gains(k);
  if (s->dec) audio_seek(s->dec, 0);
  M32(out) = handle_of(k);
  pthread_mutex_unlock(&mixer_lock); RET(FMOD_OK);
}

// ---- Channel ----
#define CHANNEL_DO(...) do { pthread_mutex_lock(&mixer_lock); Channel *k = channel_of(ARG(0)); u32 r_ = FMOD_ERR_INVALID_HANDLE; if (k) { __VA_ARGS__; r_ = FMOD_OK; } pthread_mutex_unlock(&mixer_lock); RET(r_); } while (0)
IMP(fmodex, Channel_isPlaying) { // (this, bool *): false once it has ended; an invalid handle once stopped or taken
  pthread_mutex_lock(&mixer_lock); Channel *k = channel_of(ARG(0));
  if (ARG(1)) M8(ARG(1)) = k && !k->done;
  pthread_mutex_unlock(&mixer_lock); RET(k ? FMOD_OK : FMOD_ERR_INVALID_HANDLE);
}
IMP(fmodex, Channel_stop) { CHANNEL_DO(stop(k)); }
IMP(fmodex, Channel_setPaused) { CHANNEL_DO(k->paused = (u8)ARG(1) != 0); }
IMP(fmodex, Channel_setVolume) { CHANNEL_DO({ float v = farg(c, 1); k->volume = v < 0 ? 0 : v > 1 ? 1 : v; }); }
IMP(fmodex, Channel_setMode) { CHANNEL_DO({ u32 m = (u32)ARG(1); if (m & (MODE_LOOP_OFF | MODE_LOOP_NORMAL | MODE_LOOP_BIDI)) k->mode = (k->mode & ~7u) | (m & 7u); if (m & MODE_2D) k->is3d = 0; if (m & MODE_3D) k->is3d = 1; gains(k); }); }
IMP(fmodex, Channel_setLoopCount) { CHANNEL_DO(k->loops_left = (s32)ARG(1)); }
IMP(fmodex, Channel_getLoopCount) { CHANNEL_DO(if (ARG(1)) M32(ARG(1)) = (u32)k->loops_left); }
IMP(fmodex, Channel_set3DAttributes) { CHANNEL_DO({ vec(ARG(1), k->pos); vec(ARG(2), k->vel); gains(k); }); }
IMP(fmodex, Channel_getCurrentSound) { CHANNEL_DO(if (ARG(1)) M32(ARG(1)) = (u32)(uptr)k->snd); }
IMP(fmodex, Channel_getPosition) { // (this, unsigned *, time unit)
  CHANNEL_DO({ u32 u = (u32)ARG(2), v = u == TIMEUNIT_MS ? (u32)(k->at * 1000 / (u64)k->snd->rate) : u == TIMEUNIT_PCMBYTES ? (u32)(k->at * 2 * (u64)k->snd->channels) : (u32)k->at; if (ARG(1)) M32(ARG(1)) = v; });
}

// ---- Sound ----
IMP(fmodex, Sound_getLength) { // (this, unsigned *, time unit)
  Sound *s = sound_of(ARG(0)); if (!s || !ARG(1)) { RET(FMOD_ERR_INVALID_PARAM); return; }
  u32 u = (u32)ARG(2); M32(ARG(1)) = u == TIMEUNIT_MS ? (u32)(s->frames * 1000 / (u64)s->rate) : u == TIMEUNIT_PCMBYTES ? (u32)(s->frames * 2 * (u64)s->channels) : (u32)s->frames;
  RET(FMOD_OK);
}
IMP(fmodex, Sound_getName) { Sound *s = sound_of(ARG(0)); if (!s || !ARG(1) || (s32)ARG(2) <= 0) { RET(FMOD_ERR_INVALID_PARAM); return; } snprintf(GSTR(ARG(1)), (size_t)ARG(2), "%s", s->name); RET(FMOD_OK); }
IMP(fmodex, Sound_setLoopCount) { Sound *s = sound_of(ARG(0)); if (!s) { RET(FMOD_ERR_INVALID_PARAM); return; } s->loop_count = (s32)ARG(1); RET(FMOD_OK); }
IMP(fmodex, Sound_release) {
  Sound *s = sound_of(ARG(0)); if (!s) { RET(FMOD_ERR_INVALID_PARAM); return; }
  pthread_mutex_lock(&mixer_lock);
  for (int i = 0; i < CHANNELS; i++) if (ch[i].active && ch[i].snd == s) stop(&ch[i]);
  s->magic = 0; pcm_release(s->pcm); audio_close(s->dec); free(s->file); free(s);
  pthread_mutex_unlock(&mixer_lock); RET(FMOD_OK);
}

// The decorated names Tag imports (public stdcall members: `this` and the arguments, 4 bytes each)
ALIAS(fmodex, "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z", System_init, 16);
ALIAS(fmodex, "?release@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ", System_release, 4);
ALIAS(fmodex, "?update@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ", System_update, 4);
ALIAS(fmodex, "?set3DSettings@System@FMOD@@QAG?AW4FMOD_RESULT@@MMM@Z", System_set3DSettings, 16);
ALIAS(fmodex, "?set3DListenerAttributes@System@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_VECTOR@@000@Z", System_set3DListenerAttributes, 24);
ALIAS(fmodex, "?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z", System_createSound, 20);
ALIAS(fmodex, "?createStream@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z", System_createStream, 20);
ALIAS(fmodex, "?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z", System_playSound, 20);
ALIAS(fmodex, "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z", Channel_isPlaying, 8);
ALIAS(fmodex, "?stop@Channel@FMOD@@QAG?AW4FMOD_RESULT@@XZ", Channel_stop, 4);
ALIAS(fmodex, "?setPaused@Channel@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z", Channel_setPaused, 8);
ALIAS(fmodex, "?setVolume@Channel@FMOD@@QAG?AW4FMOD_RESULT@@M@Z", Channel_setVolume, 8);
ALIAS(fmodex, "?setMode@Channel@FMOD@@QAG?AW4FMOD_RESULT@@I@Z", Channel_setMode, 8);
ALIAS(fmodex, "?setLoopCount@Channel@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", Channel_setLoopCount, 8);
ALIAS(fmodex, "?getLoopCount@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z", Channel_getLoopCount, 8);
ALIAS(fmodex, "?set3DAttributes@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PBUFMOD_VECTOR@@0@Z", Channel_set3DAttributes, 12);
ALIAS(fmodex, "?getCurrentSound@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSound@2@@Z", Channel_getCurrentSound, 8);
ALIAS(fmodex, "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z", Channel_getPosition, 12);
ALIAS(fmodex, "?getLength@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z", Sound_getLength, 12);
ALIAS(fmodex, "?getName@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z", Sound_getName, 12);
ALIAS(fmodex, "?setLoopCount@Sound@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", Sound_setLoopCount, 8);
ALIAS(fmodex, "?release@Sound@FMOD@@QAG?AW4FMOD_RESULT@@XZ", Sound_release, 4);
ALIAS(fmodex, "FMOD_System_Create", FMOD_System_Create, 4);   // FMOD's C API is stdcall, exported undecorated
