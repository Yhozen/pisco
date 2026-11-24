// XAudio2 (xaudio2_9.dll): source voices mixed here, in C, into one ring of 48 kHz stereo float frames in shared
// memory that the host plays (an AudioWorklet in the browser; Node has no output and the ring just turns).
// The mixer runs on wall-clock time a little ahead of now, so the title's voices advance in real time even
// with no listener. PCM (8-bit unsigned, 16-bit, float) at any rate, any channel count, through each voice's
// output matrix; submix voices pass through. XMA2 / ADPCM / WMA are not decoded (the voice stays silent).
// Voice callbacks (OnBufferStart, OnBufferEnd, OnStreamEnd, OnVoiceProcessingPassStart) run on the mixer thread.
#include <math.h>
#include <time.h>
#include "rt.h"

HOST("audio_open") void host_audio_open(u32 ring);
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg);
#define S_OK 0
#define E_INVALIDARG 0x80070057u
#define RATE 48000
#define RING_FRAMES 16384
#define LEAD 2400 // frames mixed ahead of the clock (50 ms)

typedef struct { u32 flags, bytes; u64 data; u32 begin, length, loop_begin, loop_length, loop_count; u64 context; } Buf;
#define QMAX 64
typedef struct Voice {
  u64 vtbl; u32 magic, kind; // 0 source, 1 submix, 2 mastering
  u32 channels, rate, bits, is_float, block, supported;
  float volume, ratio, matrix[16]; int matrix_set;
  u64 callback;
  Buf q[QMAX]; u32 qhead, qn;
  double pos;  // frames into q[qhead], from its PlayBegin
  int started, started_buf, ended;
  u64 played;
  struct Voice *next;
} Voice;
#define VMAGIC 0x58415632u
static Voice *voices;
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 engine, vt_engine, vt_source, vt_master, vt_submix;
static u32 *ring; static float *ring_data;
static int mixer_running;

static Voice *voice(u64 p) { Voice *v = (Voice *)GP(p); return p && v->magic == VMAGIC ? v : 0; }
static float sample(Voice *v, Buf *b, u64 frame, u32 ch) {
  u64 at = b->data + frame * v->block + ch * (v->bits / 8);
  if (v->is_float) return MF32(at);
  if (v->bits == 8) return ((float)M8(at) - 128.0f) / 128.0f;
  if (v->bits == 16) return (float)(s16)M16(at) / 32768.0f;
  if (v->bits == 24) { s32 x = (s32)((u32)M8(at) << 8 | (u32)M8(at + 1) << 16 | (u32)M8(at + 2) << 24); return (float)x / 2147483648.0f; }
  if (v->bits == 32) return (float)(s32)M32(at) / 2147483648.0f;
  return 0;
}
static void callback(Cpu *c, Voice *v, int slot, u64 arg) {
  if (!v->callback) return;
  u64 vt = M64(v->callback); u64 fn = M64(vt + 8 * slot);
  if (fn) rt_guest_call(c, fn, v->callback, arg, 0, 0);
}
static u32 buf_frames(Voice *v, Buf *b) { u32 total = v->block ? b->bytes / v->block : 0; if (b->length) return b->length; return total > b->begin ? total - b->begin : 0; }
// mixes `n` frames of every started source voice into out (stereo)
static void mix(Cpu *c, float *out, u32 n) {
  memset(out, 0, sizeof(float) * 2 * n);
  for (Voice *v = voices; v; v = v->next) {
    if (v->kind != 0 || !v->started || !v->qn || !v->supported) continue;
    if (v->callback) callback(c, v, 0, (u64)n * v->block);
    double step = (double)v->rate * v->ratio / RATE;
    float m[2][8];
    for (u32 s = 0; s < v->channels && s < 8; s++) for (u32 d = 0; d < 2; d++) {
      float g = v->matrix_set ? v->matrix[d * v->channels + s] : v->channels == 1 ? 1.0f : (s % 2 == d ? 1.0f : 0.0f);
      m[d][s] = g * v->volume;
    }
    for (u32 i = 0; i < n && v->qn; i++) {
      Buf *b = &v->q[v->qhead];
      if (!v->started_buf) { v->started_buf = 1; if (v->callback) callback(c, v, 3, b->context); }
      u32 frames = buf_frames(v, b);
      u64 f0 = (u64)v->pos;
      if (f0 >= frames) {
        // the buffer is done: loop or retire
        if (b->loop_count && b->loop_length) { v->pos -= b->loop_length; if (b->loop_count != 255) b->loop_count--; i--; continue; }
        u64 ctx = b->context; u32 eos = b->flags & 0x40;
        v->qhead = (v->qhead + 1) % QMAX; v->qn--; v->pos = 0; v->started_buf = 0;
        if (v->callback) { callback(c, v, 4, ctx); if (eos) callback(c, v, 2, 0); }
        i--; continue;
      }
      double t = v->pos - (double)f0; u64 f1 = f0 + 1 < frames ? f0 + 1 : f0;
      for (u32 s = 0; s < v->channels && s < 8; s++) {
        float a = sample(v, b, b->begin + f0, s), bb = sample(v, b, b->begin + f1, s), x = a + (bb - a) * (float)t;
        out[2 * i] += x * m[0][s]; out[2 * i + 1] += x * m[1][s];
      }
      v->pos += step; v->played++;
    }
  }
}
static void mixer_main(Cpu *c, void *arg) {
  (void)arg;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  static float tmp[2 * 1024];
  for (;;) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    u64 target = (u64)(((double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9) * RATE) + LEAD;
    u32 w = __atomic_load_n(&ring[0], __ATOMIC_ACQUIRE);
    while ((u64)w < target) {
      u32 n = (u32)(target - w); if (n > 1024) n = 1024;
      pthread_mutex_lock(&mix_lock); mix(c, tmp, n); pthread_mutex_unlock(&mix_lock);
      for (u32 i = 0; i < n; i++) { u32 at = (w + i) % RING_FRAMES; ring_data[2 * at] = tmp[2 * i]; ring_data[2 * at + 1] = tmp[2 * i + 1]; }
      w += n; __atomic_store_n(&ring[0], w, __ATOMIC_RELEASE);
    }
    struct timespec ts = {0, 5000000}; nanosleep(&ts, 0);
  }
}
static void start_mixer(void) {
  if (mixer_running) return;
  mixer_running = 1;
  ring = aligned_alloc(64, 64 + sizeof(float) * 2 * RING_FRAMES);
  memset(ring, 0, 64 + sizeof(float) * 2 * RING_FRAMES);
  ring[2] = RING_FRAMES; ring[3] = RATE; ring[4] = 2;
  ring_data = (float *)(ring + 16);
  host_audio_open((u32)(uptr)ring);
  rt_spawn_native(mixer_main, 0);
}

static u64 table(const char *const *names, unsigned n) {
  u64 *vt = calloc(n, 8);
  for (unsigned i = 0; i < n; i++) {
    u64 a = rt_dynamic_import("xaudio2_9", names[i]);
    if (!a) { char full[96]; snprintf(full, sizeof full, "xaudio2_9.dll!%s", names[i]); a = rt_stub_import(full); }
    vt[i] = a;
  }
  return (uptr)vt;
}
static const char *const engine_slots[] = {"Engine_QueryInterface", "Engine_AddRef", "Engine_Release", "Engine_RegisterForCallbacks", "Engine_UnregisterForCallbacks", "Engine_CreateSourceVoice", "Engine_CreateSubmixVoice", "Engine_CreateMasteringVoice", "Engine_StartEngine", "Engine_StopEngine", "Engine_CommitChanges", "Engine_GetPerformanceData", "Engine_SetDebugConfiguration"};
#define VOICE_SLOTS "Voice_GetVoiceDetails", "Voice_SetOutputVoices", "Voice_SetEffectChain", "Voice_EnableEffect", "Voice_DisableEffect", "Voice_GetEffectState", "Voice_SetEffectParameters", "Voice_GetEffectParameters", "Voice_SetFilterParameters", "Voice_GetFilterParameters", "Voice_SetOutputFilterParameters", "Voice_GetOutputFilterParameters", "Voice_SetVolume", "Voice_GetVolume", "Voice_SetChannelVolumes", "Voice_GetChannelVolumes", "Voice_SetOutputMatrix", "Voice_GetOutputMatrix", "Voice_DestroyVoice"
static const char *const source_slots[] = {VOICE_SLOTS, "Source_Start", "Source_Stop", "Source_SubmitSourceBuffer", "Source_FlushSourceBuffers", "Source_Discontinuity", "Source_ExitLoop", "Source_GetState", "Source_SetFrequencyRatio", "Source_GetFrequencyRatio", "Source_SetSourceSampleRate"};
static const char *const master_slots[] = {VOICE_SLOTS, "Master_GetChannelMask"};
static const char *const submix_slots[] = {VOICE_SLOTS};
static void tables(void) {
  if (vt_engine) return;
  vt_engine = table(engine_slots, sizeof engine_slots / sizeof *engine_slots); vt_source = table(source_slots, sizeof source_slots / sizeof *source_slots);
  vt_master = table(master_slots, sizeof master_slots / sizeof *master_slots); vt_submix = table(submix_slots, sizeof submix_slots / sizeof *submix_slots);
}
IMP(xaudio2_9, XAudio2Create) { // (&engine, flags, processor)
  tables();
  if (!engine) { engine = (uptr)calloc(1, 16); M64(engine) = vt_engine; }
  M64(ARG(0)) = engine; RET(S_OK);
}
IMP(xaudio2_9, XAudio2CreateWithVersionInfo) { imp_xaudio2_9_XAudio2Create(c); }
IMP(xaudio2_9, Engine_QueryInterface) { M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(xaudio2_9, Engine_AddRef) { RET(2); }
IMP(xaudio2_9, Engine_Release) { RET(1); }
IMP(xaudio2_9, Engine_RegisterForCallbacks) { RET(S_OK); }
IMP(xaudio2_9, Engine_UnregisterForCallbacks) {}
IMP(xaudio2_9, Engine_StartEngine) { RET(S_OK); }
IMP(xaudio2_9, Engine_StopEngine) {}
IMP(xaudio2_9, Engine_CommitChanges) { RET(S_OK); }
IMP(xaudio2_9, Engine_GetPerformanceData) { memset((void *)GP(ARG(1)), 0, 64); }
IMP(xaudio2_9, Engine_SetDebugConfiguration) {}
static Voice *voice_new(int kind, u64 vt) {
  Voice *v = aligned_alloc(16, sizeof(Voice)); memset(v, 0, sizeof *v);
  v->vtbl = vt; v->magic = VMAGIC; v->kind = (u32)kind; v->volume = 1; v->ratio = 1;
  pthread_mutex_lock(&mix_lock); v->next = voices; voices = v; pthread_mutex_unlock(&mix_lock);
  return v;
}
IMP(xaudio2_9, Engine_CreateSourceVoice) { // (&voice, format, flags, max ratio, callback, sends, effects)
  u64 f = ARG(2); if (!f) { RET(E_INVALIDARG); return; }
  Voice *v = voice_new(0, vt_source);
  u16 tag = M16(f); v->channels = M16(f + 2); v->rate = M32(f + 4); v->block = M16(f + 12); v->bits = M16(f + 14);
  if (tag == 0xFFFE && M16(f + 16) >= 22) tag = (u16)M32(f + 24); // WAVEFORMATEXTENSIBLE: the subformat
  v->is_float = tag == 3;
  v->supported = (tag == 1 || tag == 3) && v->channels && v->channels <= 8 && v->block;
  if (!v->supported) rt_log("xaudio2: source format 0x%x (%u ch, %u Hz) is not decoded; the voice is silent", tag, v->channels, v->rate);
  v->callback = ARG(5);
  M64(ARG(1)) = (uptr)v; RET(S_OK);
}
IMP(xaudio2_9, Engine_CreateSubmixVoice) { Voice *v = voice_new(1, vt_submix); v->channels = (u32)ARG(2); v->rate = (u32)ARG(3); M64(ARG(1)) = (uptr)v; RET(S_OK); }
IMP(xaudio2_9, Engine_CreateMasteringVoice) { // (&voice, channels, rate, flags, device, effects, category)
  Voice *v = voice_new(2, vt_master); v->channels = 2; v->rate = RATE;
  start_mixer();
  M64(ARG(1)) = (uptr)v; RET(S_OK);
}
IMP(xaudio2_9, Voice_GetVoiceDetails) { Voice *v = voice(ARG(0)); u64 d = ARG(1); M32(d) = 0; M32(d + 4) = 0; M32(d + 8) = v ? v->channels : 2; M32(d + 12) = v ? v->rate : RATE; }
IMP(xaudio2_9, Voice_SetOutputVoices) { RET(S_OK); }
IMP(xaudio2_9, Voice_SetEffectChain) { RET(S_OK); }
IMP(xaudio2_9, Voice_EnableEffect) { RET(S_OK); }
IMP(xaudio2_9, Voice_DisableEffect) { RET(S_OK); }
IMP(xaudio2_9, Voice_GetEffectState) { if (ARG(2)) M32(ARG(2)) = 0; }
IMP(xaudio2_9, Voice_SetEffectParameters) { RET(S_OK); }
IMP(xaudio2_9, Voice_GetEffectParameters) { RET(S_OK); }
IMP(xaudio2_9, Voice_SetFilterParameters) { RET(S_OK); }
IMP(xaudio2_9, Voice_GetFilterParameters) {}
IMP(xaudio2_9, Voice_SetOutputFilterParameters) { RET(S_OK); }
IMP(xaudio2_9, Voice_GetOutputFilterParameters) {}
IMP(xaudio2_9, Voice_SetVolume) { Voice *v = voice(ARG(0)); if (v) v->volume = X[1].ps[0]; RET(S_OK); }
IMP(xaudio2_9, Voice_GetVolume) { Voice *v = voice(ARG(0)); MF32(ARG(1)) = v ? v->volume : 1; }
IMP(xaudio2_9, Voice_SetChannelVolumes) { RET(S_OK); }
IMP(xaudio2_9, Voice_GetChannelVolumes) { for (u32 i = 0; i < (u32)ARG(1); i++) MF32(ARG(2) + 4 * i) = 1; }
IMP(xaudio2_9, Voice_SetOutputMatrix) { // (destination, source channels, destination channels, levels, operation set)
  Voice *v = voice(ARG(0)); u32 s = (u32)ARG(2), d = (u32)ARG(3); u64 m = ARG(4);
  if (!v || !m || s * 2 > 16 || d < 1) { RET(S_OK); return; }
  pthread_mutex_lock(&mix_lock);
  for (u32 di = 0; di < 2; di++) for (u32 si = 0; si < s; si++) v->matrix[di * s + si] = di < d ? MF32(m + 4 * (di * s + si)) : 0;
  if (d == 1) for (u32 si = 0; si < s; si++) v->matrix[s + si] = v->matrix[si];
  v->matrix_set = 1;
  pthread_mutex_unlock(&mix_lock);
  RET(S_OK);
}
IMP(xaudio2_9, Voice_GetOutputMatrix) {}
IMP(xaudio2_9, Voice_DestroyVoice) {
  Voice *v = voice(ARG(0)); if (!v) return;
  pthread_mutex_lock(&mix_lock);
  for (Voice **pv = &voices; *pv; pv = &(*pv)->next) if (*pv == v) { *pv = v->next; break; }
  v->magic = 0;
  pthread_mutex_unlock(&mix_lock);
}
IMP(xaudio2_9, Source_Start) { Voice *v = voice(ARG(0)); if (v) v->started = 1; RET(S_OK); }
IMP(xaudio2_9, Source_Stop) { Voice *v = voice(ARG(0)); if (v) v->started = 0; RET(S_OK); }
IMP(xaudio2_9, Source_SubmitSourceBuffer) { // (XAUDIO2_BUFFER*, XAUDIO2_BUFFER_WMA*)
  Voice *v = voice(ARG(0)); u64 b = ARG(1);
  if (!v || !b) { RET(E_INVALIDARG); return; }
  pthread_mutex_lock(&mix_lock);
  if (v->qn >= QMAX) { pthread_mutex_unlock(&mix_lock); RET(0x88960001u); return; } // XAUDIO2_E_INVALID_CALL
  Buf *q = &v->q[(v->qhead + v->qn) % QMAX];
  q->flags = M32(b); q->bytes = M32(b + 4); q->data = M64(b + 8); q->begin = M32(b + 16); q->length = M32(b + 20);
  q->loop_begin = M32(b + 24); q->loop_length = M32(b + 28); q->loop_count = M32(b + 32); q->context = M64(b + 40);
  v->qn++;
  pthread_mutex_unlock(&mix_lock);
  RET(S_OK);
}
IMP(xaudio2_9, Source_FlushSourceBuffers) { Voice *v = voice(ARG(0)); if (v) { pthread_mutex_lock(&mix_lock); v->qn = 0; v->pos = 0; v->started_buf = 0; pthread_mutex_unlock(&mix_lock); } RET(S_OK); }
IMP(xaudio2_9, Source_Discontinuity) { RET(S_OK); }
IMP(xaudio2_9, Source_ExitLoop) { Voice *v = voice(ARG(0)); if (v && v->qn) v->q[v->qhead].loop_count = 0; RET(S_OK); }
IMP(xaudio2_9, Source_GetState) { // (XAUDIO2_VOICE_STATE*, flags)
  Voice *v = voice(ARG(0)); u64 s = ARG(1); if (!s) return;
  pthread_mutex_lock(&mix_lock);
  M64(s) = v && v->qn ? v->q[v->qhead].context : 0; M32(s + 8) = v ? v->qn : 0; M64(s + 16) = v ? v->played : 0;
  pthread_mutex_unlock(&mix_lock);
}
IMP(xaudio2_9, Source_SetFrequencyRatio) { Voice *v = voice(ARG(0)); if (v) v->ratio = X[1].ps[0]; RET(S_OK); }
IMP(xaudio2_9, Source_GetFrequencyRatio) { Voice *v = voice(ARG(0)); MF32(ARG(1)) = v ? v->ratio : 1; }
IMP(xaudio2_9, Source_SetSourceSampleRate) { Voice *v = voice(ARG(0)); if (v) v->rate = (u32)ARG(1); RET(S_OK); }
IMP(xaudio2_9, Master_GetChannelMask) { M32(ARG(1)) = 3; RET(S_OK); }
