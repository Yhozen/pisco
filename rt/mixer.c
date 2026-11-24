// SPDX-License-Identifier: GPL-2.0-or-later
// The sound output (mixer.h): sources mixed into one ring of 48 kHz stereo float frames that the host plays.
// Ring header (16 words, then the frames): [0] frames written (the host reads behind it), [1] the host's read
// position, [2] capacity in frames, [3] rate, [4] channels, [5] 1 if the mix follows a virtual clock, [6] / [7] that
// clock (microseconds since it started, low / high word) at frame 0 of the ring: a recording made in the browser
// places the sound by it (web/browser/main.js).
// With the host's clock a thread mixes on wall-clock time a little ahead of now, so sound advances in real time
// even with no listener. With a virtual clock the title's thread mixes, each time the clock moves, exactly up to
// the clock: a run gives the same sound every time, however fast the host is. Under SW_AUDIO_WAV the host writes
// those frames to a file too (Node: a 32-bit float WAV).
#include <time.h>
#include "mixer.h"

HOST("audio_open") void host_audio_open(u32 ring);
HOST("audio_dump") void host_audio_dump(const float *frames, u32 n);
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg);
u64 rt_clock_virtual_us(void);   // kernel32.c
#define RING_FRAMES 16384
#define LEAD 2400 // frames mixed ahead of the wall clock (50 ms)
#define BLOCK 1024

pthread_mutex_t mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { MixerSource fn; void *ctx; } sources[4]; static int source_count;
static u32 *ring; static float *ring_data;
static int started, virtual_mix, dump;
static u64 virtual_start_us, virtual_mixed;   // the clock when the mix started; frames mixed since

void mixer_add_source(MixerSource fn, void *ctx) {
  pthread_mutex_lock(&mixer_lock);
  if (source_count < 4) { sources[source_count].fn = fn; sources[source_count].ctx = ctx; source_count++; }
  pthread_mutex_unlock(&mixer_lock);
}
// `n` (at most BLOCK) frames of every source into the ring at write position w
static void mix_block(Cpu *c, u32 w, u32 n) {
  static float tmp[2 * BLOCK];
  memset(tmp, 0, sizeof(float) * 2 * n);
  for (int i = 0; i < source_count; i++) sources[i].fn(c, tmp, n, sources[i].ctx);
  for (u32 i = 0; i < n; i++) { u32 at = (w + i) % RING_FRAMES; ring_data[2 * at] = tmp[2 * i]; ring_data[2 * at + 1] = tmp[2 * i + 1]; }
  if (dump) host_audio_dump(tmp, n);
}
static void mixer_main(Cpu *c, void *arg) {
  (void)arg;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    u64 target = (u64)(((double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9) * MIXER_RATE) + LEAD;
    u32 w = __atomic_load_n(&ring[0], __ATOMIC_ACQUIRE);
    while ((u64)w < target) {
      u32 n = (u32)(target - w); if (n > BLOCK) n = BLOCK;
      pthread_mutex_lock(&mixer_lock); mix_block(c, w, n); pthread_mutex_unlock(&mixer_lock);
      w += n; __atomic_store_n(&ring[0], w, __ATOMIC_RELEASE);
    }
    struct timespec ts = {0, 5000000}; nanosleep(&ts, 0);
  }
}
void mixer_clock_moved(void) {
  if (!started || !virtual_mix) return;
  u64 target = (rt_clock_virtual_us() - virtual_start_us) * MIXER_RATE / 1000000;
  pthread_mutex_lock(&mixer_lock);
  while (virtual_mixed < target) {
    u32 n = target - virtual_mixed > BLOCK ? BLOCK : (u32)(target - virtual_mixed);
    mix_block(0, (u32)virtual_mixed, n);
    virtual_mixed += n; __atomic_store_n(&ring[0], (u32)virtual_mixed, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&mixer_lock);
}
void mixer_start(void) {
  if (started) return;
  started = 1;
  ring = aligned_alloc(64, 64 + sizeof(float) * 2 * RING_FRAMES);
  memset(ring, 0, 64 + sizeof(float) * 2 * RING_FRAMES);
  ring[2] = RING_FRAMES; ring[3] = MIXER_RATE; ring[4] = 2;
  ring_data = (float *)(ring + 16);
  virtual_mix = rt_clock_virtual();
  if (virtual_mix) {
    virtual_start_us = rt_clock_virtual_us();
    ring[5] = 1; ring[6] = (u32)virtual_start_us; ring[7] = (u32)(virtual_start_us >> 32);
    const char *w = getenv("SW_AUDIO_WAV"); dump = w && *w;
  }
  host_audio_open((u32)(uptr)ring);
  if (!virtual_mix) rt_spawn_native(mixer_main, 0);
}
