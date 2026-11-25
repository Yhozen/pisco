// SPDX-License-Identifier: GPL-2.0-or-later
// The stream between the graphics front ends (on the title's threads: rt/gdk/d3d12.c for Direct3D 12 and the
// console's D3D12.X; rt/windows-x86/d3d9_gpu.c for Direct3D 9, with the explicit records further down) and the GPU back
// end (web/gpu/backend.js on the page, web/gpu/null.js in Node). tools/gen-cmd-js.mjs turns every
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
// in its flags, beside D3D12's: a D3D12 buffer, which gets 64 KiB past its end on the GPU (a constant buffer view
// near the end still has the 64 KiB its translated shader declares; WebGPU checks the bound range against it)
#define GPU_RESOURCE_CBV_TAIL 0x80000000u
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
// a mark for traces: bytes of text follow. "shot <name>": the page saves the frame presented last under that name
#define GPU_R_MARK 9

// ---- explicit records: a front end that is not Direct3D 12 says what WebGPU should do, in WebGPU's own terms.
// Direct3D 9 (rt/windows-x86/d3d9_gpu.c) is the first: no root signatures, no pipeline-state descriptions, no command
// lists. It shares GPU_R_RESOURCE / GPU_R_DESTROY (buffers and textures, without guest backing), GPU_R_PRESENT and
// GPU_R_MARK with the records above, and with them the ids, the DXGI format numbers and the D3D12 numbers of
// blend factors, blend operations, comparisons and address modes (Direct3D 9's own are the same numbers).
// All of them are run in order: a write lands before the draws recorded after it and after the ones before.
// id, bytes of text, then the WGSL text. A module has one entry point, `main`
#define GPU_R_SHADER 10
// id, address mode u, v, w (1 repeat, 2 mirror, 3 clamp), magnification filter, minification filter, mip filter
// (0 nearest, 1 linear), largest anisotropy, largest level of detail (float bits)
#define GPU_R_SAMPLER 11
// id, vertex shader id, fragment shader id, topology (GPU_L_TOPOLOGY's numbers: 1 points ... 5 triangle strip),
// cull (0 none, 1 front, 2 back), front face (0 counter-clockwise, 1 clockwise),
// colour target format (DXGI; 0: none), colour write mask, blending on, source, destination, operation, and the
// same three for alpha,
// depth format (DXGI; 0: none), depth comparison, depth write, depth bias (signed), slope-scaled bias (float bits),
// binding count, vertex buffer count, attribute count; then
//   a binding: number in group 0, kind (GPU_BIND_*), least size in bytes (a uniform block)
//   a vertex buffer: stride, step (0 a vertex, 1 an instance)
//   an attribute: shader location, vertex buffer, offset, format (DXGI)
// The bind layout is exactly these bindings: a draw gives one resource for each, in this order.
#define GPU_R_PIPELINE 12
#define GPU_PIPELINE_WORDS 24
#define GPU_BIND_UNIFORM 1
#define GPU_BIND_TEXTURE_2D 2
#define GPU_BIND_TEXTURE_3D 3
#define GPU_BIND_SAMPLER 4
// buffer id, offset, bytes, mode (GPU_WRITE_*), then the bytes
#define GPU_R_WRITE_BUFFER 13
// the bytes replace what earlier draws read: those draws still see the old bytes
#define GPU_WRITE_ORDERED 0
// the bytes are ones no earlier draw read (Direct3D's NOOVERWRITE)
#define GPU_WRITE_APPEND 1
// nothing of the buffer's old contents is needed after this (Direct3D's DISCARD): the back end may hand out new storage
#define GPU_WRITE_DISCARD 2
// texture id, mip level, x, y, z, width, height, depth, bytes a row, then the rows (depth slices one after another)
#define GPU_R_WRITE_TEXTURE 14
// A render pass begins (the one before ends): colour texture id, its mip level, depth texture id (0: none),
// flags (GPU_PASS_*), clear colour r g b a (float bits), clear depth (float bits), clear stencil.
// What is not cleared is loaded. The viewport and the scissor rectangle stay as GPU_R_VIEWPORT last set them.
#define GPU_R_PASS 15
#define GPU_PASS_CLEAR_COLOR 1
#define GPU_PASS_CLEAR_DEPTH 2
#define GPU_PASS_CLEAR_STENCIL 4
// x, y, width, height, least depth, greatest depth (float bits), then the scissor rectangle: x, y, width, height
#define GPU_R_VIEWPORT 16
// One draw in the pass: pipeline id, flags (GPU_DRAW_*), count (indices or vertices), instances, first (index or
// vertex), base vertex (signed), first instance, index buffer id, offset into it in bytes, vertex buffer count,
// binding count, uniform bytes; then
//   a vertex buffer: buffer id, offset in bytes
//   a binding, in the pipeline's order: the texture's or the sampler's id (0 for the uniform block)
//   the uniform block's bytes
#define GPU_R_DRAW 17
#define GPU_DRAW_WORDS 12
#define GPU_DRAW_INDEXED 1
#define GPU_DRAW_INDEX32 2

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
