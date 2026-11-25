// SPDX-License-Identifier: GPL-2.0-or-later
// The GPU ring's writer (gpu_ring.h). The ring lives in shared memory: header words, then GPU_RING_BYTES of
// records (rt/d3d12_cmd.h has the layout). Front ends append at `head`; the back end consumes at `tail`.
#include "gpu_ring.h"

u32 *rt_gpu_ring;  // header words, then the records
static u8 *ring_data;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
#define ring rt_gpu_ring
void rt_gpu_open(void) {
  if (ring) return;
  u32 *r = aligned_alloc(64, GPU_H_WORDS * 4 + GPU_RING_BYTES);
  memset(r, 0, GPU_H_WORDS * 4);
  r[GPU_H_SIZE] = GPU_RING_BYTES;
  ring_data = (u8 *)(r + GPU_H_WORDS);
  ring = r;
  host_gpu_open((u32)(uptr)r);
}
static u32 rec_pos;
// room for a record of `payload` bytes; the caller fills it and calls rt_gpu_rec_end. Holds ring_lock.
u8 *rt_gpu_rec_begin(u32 op, u32 payload) {
  rt_gpu_open();
  u32 bytes = (8 + payload + 3) & ~3u;
  if (bytes > GPU_RING_BYTES / 2) rt_fatal("gpu: a record of %u bytes does not fit the ring", bytes);
  pthread_mutex_lock(&ring_lock);
  u32 head = ring[GPU_H_HEAD], pos = head % GPU_RING_BYTES, need = bytes;
  if (pos + bytes > GPU_RING_BYTES) need += GPU_RING_BYTES - pos;
  for (;;) {
    u32 tail = __atomic_load_n(&ring[GPU_H_TAIL], __ATOMIC_ACQUIRE);
    if (GPU_RING_BYTES - (head - tail) >= need) break;
    __builtin_wasm_memory_atomic_notify((int *)&ring[GPU_H_HEAD], 4);   // records published quietly: the back end may be asleep
    __builtin_wasm_memory_atomic_wait32((int *)&ring[GPU_H_TAIL], (int)tail, 50000000);
  }
  if (pos + bytes > GPU_RING_BYTES) {
    u32 skip = GPU_RING_BYTES - pos;
    ((u32 *)(ring_data + pos))[0] = GPU_R_WRAP; ((u32 *)(ring_data + pos))[1] = skip;
    head += skip; __atomic_store_n(&ring[GPU_H_HEAD], head, __ATOMIC_RELEASE);
    pos = 0;
  }
  u32 *h = (u32 *)(ring_data + pos); h[0] = op; h[1] = bytes;
  memset(h + 2, 0, bytes - 8);
  rec_pos = bytes;
  return (u8 *)(h + 2);
}
void rt_gpu_rec_end_quiet(void) {
  __atomic_store_n(&ring[GPU_H_HEAD], ring[GPU_H_HEAD] + rec_pos, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&ring_lock);
}
void rt_gpu_kick(void) { if (ring) __builtin_wasm_memory_atomic_notify((int *)&ring[GPU_H_HEAD], 4); }
void rt_gpu_rec_end(void) {
  __atomic_store_n(&ring[GPU_H_HEAD], ring[GPU_H_HEAD] + rec_pos, __ATOMIC_RELEASE);
  __builtin_wasm_memory_atomic_notify((int *)&ring[GPU_H_HEAD], 4);
  pthread_mutex_unlock(&ring_lock);
}
void rt_gpu_rec_words(u32 op, const u32 *w, u32 n) { u8 *p = rt_gpu_rec_begin(op, 4 * n); memcpy(p, w, 4 * n); rt_gpu_rec_end(); }

static u32 presents;
void rt_gpu_present(u32 resource, u64 token, u32 interval) { // D3D12's PresentX, DXGI's Present (rt/dxgi.c), Direct3D 9's Present
  u32 w[4] = {resource, (u32)token, (u32)(token >> 32), interval}; rt_gpu_rec_words(GPU_R_PRESENT, w, 4);
  presents++;
  // two frames in flight at most: wait until the back end shows the one before last
  while (__atomic_load_n(&ring[GPU_H_PRESENTS], __ATOMIC_ACQUIRE) + 2 < presents) {
    u32 e = __atomic_load_n(&ring[GPU_H_EPOCH], __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&ring[GPU_H_PRESENTS], __ATOMIC_ACQUIRE) + 2 >= presents) break;
    __builtin_wasm_memory_atomic_wait32((int *)&ring[GPU_H_EPOCH], (int)e, 100000000);
  }
}
void rt_gpu_drain(void) {
  if (!ring) return;
  for (;;) {
    u32 tail = __atomic_load_n(&ring[GPU_H_TAIL], __ATOMIC_ACQUIRE);
    if (tail == __atomic_load_n(&ring[GPU_H_HEAD], __ATOMIC_ACQUIRE)) return;
    __builtin_wasm_memory_atomic_notify((int *)&ring[GPU_H_HEAD], 4);
    __builtin_wasm_memory_atomic_wait32((int *)&ring[GPU_H_TAIL], (int)tail, 50000000);
  }
}
