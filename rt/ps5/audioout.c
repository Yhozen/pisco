// SPDX-License-Identifier: GPL-2.0-or-later
// libSceAudioOut: a port takes blocks of 16-bit samples (mono, or stereo interleaved) at its rate; sceAudioOutOutput
// waits while the port has more than a few blocks queued, as the console's does once its buffer is full. The port is
// a source of the runtime's mixer (mixer.c), which the page plays (an AudioWorklet) and which, with no one listening,
// still consumes at the wall clock's pace (or the virtual clock's).
#include "ps5.h"
#include "mixer.h"

#define QUEUE 8192   // frames a port holds
typedef struct { u32 frames, rate, channels; s16 q[QUEUE * 2]; u32 head, tail; double pos; pthread_cond_t room; } Port;
static Port *ports[8];
static int started;
static void source(Cpu *c, float *out, u32 frames, void *ctx) {
  (void)c; Port *p = ctx;
  double step = (double)p->rate / MIXER_RATE;
  for (u32 i = 0; i < frames; i++) {
    u32 have = p->head - p->tail;
    if (!have) break;
    u32 at = p->tail % QUEUE;
    out[2 * i] += p->q[2 * at] / 32768.0f; out[2 * i + 1] += p->q[2 * at + 1] / 32768.0f;
    p->pos += step;
    while (p->pos >= 1.0 && p->head != p->tail) { p->tail++; p->pos -= 1.0; }
  }
  pthread_cond_broadcast(&p->room);
}
IMP(libsceaudioout, sceAudioOutInit) { RET(0); }
// (user, port type, index, frames a block, rate, format: 0 mono s16, 1 stereo s16)
IMP(libsceaudioout, sceAudioOutOpen) {
  int k = 1; while (k < 8 && ports[k]) k++;
  if (k == 8) { RET(SCE_KERNEL_ERROR(EBUSY)); return; }
  Port *p = calloc(1, sizeof *p); p->frames = (u32)A3; p->rate = (u32)A4 ? (u32)A4 : 48000; p->channels = (A5 & 0xFF) == 1 ? 2 : 1;
  pthread_cond_init(&p->room, 0);
  ports[k] = p;
  if (!started) { started = 1; mixer_start(); }
  mixer_add_source(source, p);
  RET((u64)k);
}
IMP(libsceaudioout, sceAudioOutClose) { RET(0); }
IMP(libsceaudioout, sceAudioOutSetVolume) { RET(0); }
// (port, samples): queued; waits while more than four blocks wait to be played
IMP(libsceaudioout, sceAudioOutOutput) {
  int k = (int)A0; Port *p = k > 0 && k < 8 ? ports[k] : 0;
  if (!p) { RET(SCE_KERNEL_ERROR(EINVAL)); return; }
  if (!A1) { RET(0); return; }
  const s16 *s = (const s16 *)GP(A1);
  pthread_mutex_lock(&mixer_lock);
  while (p->head - p->tail + p->frames > (p->frames * 4 < QUEUE ? p->frames * 4 : QUEUE)) {
    if (rt_clock_virtual()) { pthread_mutex_unlock(&mixer_lock); sched_yield(); pthread_mutex_lock(&mixer_lock); continue; }
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_nsec += 20000000; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&p->room, &mixer_lock, &t);
  }
  for (u32 i = 0; i < p->frames; i++) {
    u32 at = p->head % QUEUE;
    p->q[2 * at] = s[p->channels * i]; p->q[2 * at + 1] = s[p->channels * i + (p->channels - 1)];
    p->head++;
  }
  pthread_mutex_unlock(&mixer_lock);
  RET(0);
}
