// SPDX-License-Identifier: GPL-2.0-or-later
// The D3D12 front end's objects (rt/gdk/d3d12.c), for the files that add methods to them: the console's
// (rt/xbox-series/d3d12x.c, xg.c) and the PC's swap chains (rt/windows-x64/dxgi_swapchain.c).
#pragma once
#include <time.h>
#include "rt.h"
#include "d3d12_cmd.h"
#include "gpu_ring.h"

#define S_OK 0
#define S_FALSE 1
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define DXGI_ERROR_UNSUPPORTED 0x887A0004u

enum { C_DEVICE, C_QUEUE, C_ALLOCATOR, C_LIST, C_FENCE, C_RESOURCE, C_HEAP, C_ROOTSIG, C_PSO, C_HEAPOBJ, C_QUERYHEAP, C_CMDSIG, C_COUNT };
#define OMAGIC 0x44334432u

typedef struct Obj { u64 vtbl; u32 magic, cls, id; s32 refs; u32 console; u32 pad; } Obj;
typedef struct { Obj o; } Device;
typedef struct { Obj o; u32 type; } Queue;
typedef struct { Obj o; u32 type; u32 *w; u32 n, cap; u32 kick; u64 up_base, up, up_end; } List; // kick: words KickoffX ran; up: AllocateUploadMemoryX
typedef struct { Obj o; u8 *rec; u32 bytes; } Pso; // the PSO record, kept for CreateDerivedPipelineStateX
typedef struct { Obj o; u64 completed; } Fence; // `completed` is 8-aligned: the back end writes it
_Static_assert(__builtin_offsetof(Fence, completed) == GPU_FENCE_VALUE, "fence layout");
typedef struct { Obj o; u32 dim, format, height, depth, mips, samples, flags, heap; u64 width, backing, va; u8 desc[56]; } Resource;
typedef struct { Obj o; u32 type, count, flags; u64 storage; } DescHeap;
typedef struct { Obj o; u32 type, cpu_page; u64 size, va; } HeapObj; // va: its memory (console: every heap has some)
typedef struct { Obj o; u32 type, count; } QueryHeap;
typedef struct { Obj o; } Simple;


// the ring: its writer is rt/gpu_ring.c, shared with the other front ends
#define ring rt_gpu_ring
#define gpu_open rt_gpu_open
#define rec_begin rt_gpu_rec_begin
#define rec_end rt_gpu_rec_end
#define rec_words rt_gpu_rec_words
#define LIST List *self = (List *)GP(ARG(0))

Obj *d3d12_obj(u64 p);                  // the object at p, or 0
u32 d3d12_id_of(u64 p);                 // its id on the GPU ring
u32 d3d12_give(Obj *o, u64 riid, u64 ppv); // hands out o through (riid, ppv) as QueryInterface does
Device *d3d12_device(int console);      // the device of that flavour
u32 *d3d12_emit(List *l, u32 op, u32 n); // a command word with n words after it
void d3d12_execute_words(u32 queue, List *l);
u32 d3d12_create_resource(Obj *dev, u32 heap, u32 cpu_page, u64 desc, u64 at, u64 riid, u64 ppv);
u64 d3d12_alloc_info(const u8 *desc);
void d3d12_footprints(const u8 *desc, u32 first, u32 num, u64 base, u64 layouts, u64 rows, u64 sizes, u64 total);
u64 rt_blob_new(const void *data, u64 size);
int d3d12_pace_off(void);              // SW_PACE=off: no waiting for the frame clock
