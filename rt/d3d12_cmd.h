// The stream between the D3D12 front end (rt/d3d12.c, on the title's threads) and the GPU back end
// (web/gpu/backend.js on the page, web/gpu/null.js in Node). tools/gen-cmd-js.mjs turns every
// `#define GPU_<NAME> <value>` line of this file into web/gpu/cmd.gen.js: keep one define per line.
//
// The ring lives in shared memory: a header, then GPU_RING_BYTES of records. A record is
// [u32 op][u32 bytes, header included, a multiple of 4][payload]. The front end appends at `head`,
// the back end consumes at `tail` (both byte counters that only grow; position = counter % size).
// A record never wraps: GPU_R_WRAP sends the reader back to the start.
#pragma once

#define GPU_RING_BYTES 16777216
// header words (u32 index)
#define GPU_H_HEAD 0
#define GPU_H_TAIL 1
#define GPU_H_SIZE 2
#define GPU_H_EPOCH 3
#define GPU_H_SUBMITTED 4
#define GPU_H_CONSUMED 5
#define GPU_H_PRESENTS 6
#define GPU_H_INFLIGHT 7
#define GPU_H_WORDS 16

// ---- ring records ----
#define GPU_R_WRAP 0
// id, dimension (0 buffer, 2 tex2d, 3 tex3d, 1 tex1d), format, width lo, width hi, height, depth or array size,
// mip levels, sample count, flags, heap type, guest backing (0: none), gpu va lo, gpu va hi.
// A buffer with backing: guest memory holds its bytes (the GPU's copy is uploaded before work reads it and
// read back after work writes it). A texture with backing: its texels sit in guest memory in the title
// layout (subresources one after another, as GetCopyableFootprints places them) and the GPU follows them.
#define GPU_R_RESOURCE 1
// id
#define GPU_R_DESTROY 2
// id, bytes, the serialized root signature (RTS0 part or a whole container holding one)
#define GPU_R_ROOTSIG 3
// id, root signature id, 0 graphics / 1 compute, desc bytes, element count, then the desc
// (D3D12_GRAPHICS_PIPELINE_STATE_DESC, 656 bytes) with every pointer replaced by an offset into
// this record, input elements (32 bytes each, name at an offset), names, shader blobs
#define GPU_R_PSO 4
// queue id, then list words (GPU_L_ ops) up to the record's end
#define GPU_R_EXECUTE 5
// fence guest address, value lo, value hi: write it once all work before is done
#define GPU_R_SIGNAL 6
// fence guest address, value lo, value hi: later records wait for it
#define GPU_R_WAIT 7
// resource id, frame token lo, token hi, sync interval
#define GPU_R_PRESENT 8
// a mark for traces: bytes of text follow
#define GPU_R_MARK 9

// ---- command list words: [u16 op | u16 words of payload << 16][payload] ----
#define GPU_L_CLEAR_RTV 1
#define GPU_L_CLEAR_DSV 2
#define GPU_L_SET_RTS 3
#define GPU_L_VIEWPORTS 4
#define GPU_L_SCISSORS 5
#define GPU_L_PSO 6
#define GPU_L_TOPOLOGY 7
#define GPU_L_ROOTSIG 8
#define GPU_L_TABLE 9
#define GPU_L_CONSTANTS 10
#define GPU_L_ROOT_VIEW 11
#define GPU_L_VB 12
#define GPU_L_IB 13
#define GPU_L_DRAW 14
#define GPU_L_DRAW_INDEXED 15
#define GPU_L_COPY_BUFFER 16
#define GPU_L_COPY_TEXTURE 17
#define GPU_L_COPY_RESOURCE 18
#define GPU_L_BLEND_FACTOR 19
#define GPU_L_STENCIL_REF 20
#define GPU_L_DISPATCH 21
#define GPU_L_BARRIER 22
// D3D12.X: memory as the GPU sees it is guest memory (UMA). These run in order with the list's other work,
// once the work before them is done. Addresses are guest addresses (lo, hi).
// address lo hi, value lo hi, bytes (4 or 8), 1: write a GPU timestamp instead of the value
#define GPU_L_MEM_WRITE 23
// address lo hi, bytes lo hi, 32-bit value
#define GPU_L_MEM_FILL 24
// destination lo hi, source lo hi, bytes lo hi
#define GPU_L_MEM_COPY 25
// address lo hi, comparison (D3D12_COMPARISON_FUNC), reference lo hi, mask lo hi, bytes (4 or 8): later work waits for it
#define GPU_L_MEM_WAIT 26
// index address lo hi, DXGI format, index count: DrawIndexedX, indices straight from memory
#define GPU_L_DRAW_INDEXED_X 27
// depth bias, clamp, slope (float bits): RSSetDepthBiasX; all zero turns the override off
#define GPU_L_DEPTH_BIAS 28
// query heap id, query type, index
#define GPU_L_QUERY_BEGIN 29
#define GPU_L_QUERY_END 30
// query heap id, query type, first, count, destination resource id, offset lo hi
#define GPU_L_QUERY_RESOLVE 31
// the next n words were run before (KickoffX): only their state counts now. n
#define GPU_L_REPLAY 32

// ---- descriptors: 32-byte records the front end writes into descriptor heaps in guest memory ----
#define GPU_DESC_BYTES 32
#define GPU_D_NONE 0
#define GPU_D_RTV 1
#define GPU_D_DSV 2
#define GPU_D_SRV 3
#define GPU_D_UAV 4
#define GPU_D_CBV 5
#define GPU_D_SAMPLER 6
// view words: kind, resource id, format, view dimension, first mip, mip count, first slice, slice count
// CBV: kind, 0, va lo, va hi, size
// sampler: kind, filter, address u, address v, address w, max anisotropy, comparison, max lod (float bits)

// a fence object's completed value (u64) sits at this offset from the object's address
#define GPU_FENCE_VALUE 32
