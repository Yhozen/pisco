// SPDX-License-Identifier: GPL-2.0-or-later
// The writer of the GPU ring (rt/d3d12_cmd.h): what the graphics front ends share. rt/gdk/d3d12.c (the console's
// Direct3D 12) and rt/windows-x86/d3d9_gpu.c (Direct3D 9 for Windows desktop titles) both append records here; one
// back end reads them (web/gpu/backend.js on the page, web/gpu/null.js in Node).
#pragma once
#include "rt.h"
#include "d3d12_cmd.h"

extern u32 *rt_gpu_ring;                    // the header words, then the records; null until rt_gpu_open
void rt_gpu_open(void);                     // makes the ring and tells the host, once
// Room for a record of `payload` bytes (zeroed): the caller fills it and ends it. Waits for the back end when the
// ring is full. The ring's lock is held from begin to end.
u8 *rt_gpu_rec_begin(u32 op, u32 payload);
void rt_gpu_rec_end(void);                  // publishes the record and wakes the back end
// Publishes without waking: for a run of small records (a frame's draws). The back end reads on once it is
// awake; rt_gpu_kick, any rt_gpu_rec_end, a full ring and rt_gpu_drain wake it.
void rt_gpu_rec_end_quiet(void);
void rt_gpu_kick(void);
void rt_gpu_rec_words(u32 op, const u32 *w, u32 n);
// GPU_R_PRESENT, then waits while more than two frames are in flight
void rt_gpu_present(u32 resource, u64 token, u32 interval);
void rt_gpu_drain(void);                    // returns once the back end has consumed every record (nothing if the ring was never opened)
