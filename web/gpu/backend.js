// SPDX-License-Identifier: GPL-2.0-or-later
// The WebGPU back end: interprets the front end's ring (rt/d3d12_cmd.h) on the page's main thread (it never
// blocks: it waits with Atomics.waitAsync and awaits WebGPU's promises). D3D12 state becomes WebGPU passes,
// pipelines and bind groups; CPU-visible buffers are uploaded from guest memory when a submission uses them;
// a present draws the frame into the canvas. Shaders come translated ahead of time (tools/shader-prep.mjs):
// <title folder>/shaders/<container digest>.wgsl + .json, bindings by register (dxil2spv.cpp's scheme).
// A front end that is not Direct3D 12 (Direct3D 9) sends explicit records instead: explicit.js runs those, on
// the same resources and with the same present.
import { GPU } from './cmd.gen.js';
import { Ring, words } from './ring.js';
import { memWrite, memFill, memCopy, memWait } from './memops.js';
import { startExplicit } from './explicit.js';
import { isExplicit } from './records.js';

const FORMATS = {
  1: 'rgba32float', 2: 'rgba32float', 3: 'rgba32uint', 4: 'rgba32sint', 9: 'rgba16float', 10: 'rgba16float', 11: 'rgba16unorm', 12: 'rgba16uint', 13: 'rgba16snorm', 14: 'rgba16sint',
  15: 'rg32float', 16: 'rg32float', 17: 'rg32uint', 18: 'rg32sint', 19: 'depth32float-stencil8', 20: 'depth32float-stencil8',
  23: 'rgb10a2unorm', 24: 'rgb10a2unorm', 25: 'rgb10a2uint', 26: 'rg11b10ufloat', 27: 'rgba8unorm', 28: 'rgba8unorm', 29: 'rgba8unorm-srgb', 30: 'rgba8uint', 31: 'rgba8snorm', 32: 'rgba8sint',
  33: 'rg16float', 34: 'rg16float', 35: 'rg16unorm', 36: 'rg16uint', 37: 'rg16snorm', 38: 'rg16sint',
  39: 'r32float', 40: 'depth32float', 41: 'r32float', 42: 'r32uint', 43: 'r32sint', 44: 'depth24plus-stencil8', 45: 'depth24plus-stencil8', 46: 'r32float',
  48: 'rg8unorm', 49: 'rg8unorm', 50: 'rg8uint', 51: 'rg8snorm', 52: 'rg8sint', 53: 'r16float', 54: 'r16float', 55: 'depth16unorm', 56: 'r16unorm', 57: 'r16uint', 58: 'r16snorm', 59: 'r16sint',
  60: 'r8unorm', 61: 'r8unorm', 62: 'r8uint', 63: 'r8snorm', 64: 'r8sint', 65: 'r8unorm',
  70: 'bc1-rgba-unorm', 71: 'bc1-rgba-unorm', 72: 'bc1-rgba-unorm-srgb', 73: 'bc2-rgba-unorm', 74: 'bc2-rgba-unorm', 75: 'bc2-rgba-unorm-srgb', 76: 'bc3-rgba-unorm', 77: 'bc3-rgba-unorm', 78: 'bc3-rgba-unorm-srgb',
  79: 'bc4-r-unorm', 80: 'bc4-r-unorm', 81: 'bc4-r-snorm', 82: 'bc5-rg-unorm', 83: 'bc5-rg-unorm', 84: 'bc5-rg-snorm',
  85: 'rgba8unorm', 86: 'rgba8unorm', 87: 'bgra8unorm', 88: 'bgra8unorm', 90: 'bgra8unorm', 91: 'bgra8unorm-srgb', 92: 'bgra8unorm', 93: 'bgra8unorm-srgb',
  94: 'bc6h-rgb-ufloat', 95: 'bc6h-rgb-ufloat', 96: 'bc6h-rgb-float', 97: 'bc7-rgba-unorm', 98: 'bc7-rgba-unorm', 99: 'bc7-rgba-unorm-srgb',
};
const VERTEX_FORMATS = { 2: 'float32x4', 3: 'uint32x4', 4: 'sint32x4', 6: 'float32x3', 7: 'uint32x3', 8: 'sint32x3', 10: 'float16x4', 11: 'unorm16x4', 12: 'uint16x4', 13: 'snorm16x4', 14: 'sint16x4',
  16: 'float32x2', 17: 'uint32x2', 18: 'sint32x2', 24: 'unorm10-10-10-2', 28: 'unorm8x4', 30: 'uint8x4', 31: 'snorm8x4', 32: 'sint8x4', 34: 'float16x2', 35: 'unorm16x2', 36: 'uint16x2', 37: 'snorm16x2', 38: 'sint16x2',
  41: 'float32', 42: 'uint32', 43: 'sint32', 87: 'unorm8x4-bgra' };
const VERTEX_SIZE = { float32x4: 16, uint32x4: 16, sint32x4: 16, float32x3: 12, uint32x3: 12, sint32x3: 12, float32x2: 8, uint32x2: 8, sint32x2: 8, float16x4: 8, unorm16x4: 8, uint16x4: 8, snorm16x4: 8, sint16x4: 8 };
const SRGB = { rgba8unorm: 'rgba8unorm-srgb', 'rgba8unorm-srgb': 'rgba8unorm', bgra8unorm: 'bgra8unorm-srgb', 'bgra8unorm-srgb': 'bgra8unorm' };
const BLEND = [null, 'zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha', 'one-minus-dst-alpha', 'dst', 'one-minus-dst', 'src-alpha-saturated', null, null, 'constant', 'one-minus-constant', 'src1', 'one-minus-src1', 'src1-alpha', 'one-minus-src1-alpha'];
const BLEND_OP = [null, 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
const COMPARE = [null, 'never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const STENCIL_OP = [null, 'keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert', 'increment-wrap', 'decrement-wrap'];
const TOPOLOGY = { 1: 'point-list', 2: 'line-list', 3: 'line-strip', 4: 'triangle-list', 5: 'triangle-strip' };
const ADDRESS = [null, 'repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'clamp-to-edge'];
const isDepth = (f) => f && f.startsWith('depth');
const blockOf = (f) => (f && f.startsWith('bc') ? 4 : 1);
const u64 = (lo, hi) => hi * 4294967296 + lo; // GPU addresses fit a double

// ---- root signatures (RTS0, versions 1.0 and 1.1) ----
function findPart(b, fourcc) {
  if (b.length >= 32 && String.fromCharCode(...b.subarray(0, 4)) === 'DXBC') {
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength), n = dv.getUint32(28, true);
    for (let i = 0; i < n; i++) { const o = dv.getUint32(32 + 4 * i, true); if (String.fromCharCode(...b.subarray(o, o + 4)) === fourcc) return b.subarray(o + 8, o + 8 + dv.getUint32(o + 4, true)); }
    return null;
  }
  return b;
}
export function parseRootSignature(bytes) {
  const b = findPart(bytes, 'RTS0'); if (!b) return null;
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength), r = (o) => dv.getUint32(o, true);
  const version = r(0), params = [], samplers = [];
  for (let i = 0; i < r(4); i++) {
    const at = r(8) + 12 * i, type = r(at), vis = r(at + 4), p = r(at + 8);
    if (type === 0) {
      const ranges = []; let next = 0;
      for (let k = 0; k < r(p); k++) {
        const q = r(p + 4) + (version >= 2 ? 24 : 20) * k;
        const count = r(q + 4), off = r(q + (version >= 2 ? 20 : 16));
        const start = off === 0xFFFFFFFF ? next : off;
        ranges.push({ type: r(q), count, base: r(q + 8), space: r(q + 12), offset: start });
        next = start + (count === 0xFFFFFFFF ? 0 : count);
      }
      params.push({ type, vis, ranges });
    } else if (type === 1) params.push({ type, vis, reg: r(p), space: r(p + 4), num: r(p + 8) });
    else params.push({ type, vis, reg: r(p), space: r(p + 4) });
  }
  for (let i = 0; i < r(12); i++) {
    const at = r(16) + 52 * i;
    samplers.push({ filter: r(at), u: r(at + 4), v: r(at + 8), w: r(at + 12), aniso: r(at + 20), compare: r(at + 24), maxLod: dv.getFloat32(at + 36, true), reg: r(at + 40), space: r(at + 44) });
  }
  return { params, samplers };
}

// the app layout of a texture in guest memory (rt/gdk/d3d12.c d3d12_footprints(): subresources one after another,
// 512-byte aligned, rows 256-byte aligned): [{offset, pitch, rows, width, height, depth, mip, layer}]
const BYTES = { 'rgba32float': 16, 'rgba32uint': 16, 'rgba32sint': 16, 'rgba16float': 8, 'rgba16unorm': 8, 'rgba16uint': 8, 'rgba16snorm': 8, 'rgba16sint': 8, 'rg32float': 8, 'rg32uint': 8, 'rg32sint': 8,
  'rg8unorm': 2, 'rg8uint': 2, 'rg8snorm': 2, 'rg8sint': 2, 'r16float': 2, 'r16unorm': 2, 'r16uint': 2, 'r16snorm': 2, 'r16sint': 2, 'depth16unorm': 2, 'r8unorm': 1, 'r8uint': 1, 'r8snorm': 1, 'r8sint': 1 };
const texelBytes = (f) => (f.startsWith('bc1') || f.startsWith('bc4') ? 8 : f.startsWith('bc') ? 16 : BYTES[f] ?? 4);
export function titleLayout(r) {
  const out = []; let off = 0; const bl = blockOf(r.format), bpe = texelBytes(r.format);
  const layers = r.dim === 3 ? 1 : r.depth;
  for (let layer = 0; layer < layers; layer++) for (let mip = 0; mip < r.mips; mip++) {
    const w = Math.max(1, r.width >> mip), h = Math.max(1, r.height >> mip), d = r.dim === 3 ? Math.max(1, r.depth >> mip) : 1;
    const rows = Math.ceil(h / bl), pitch = Math.ceil(Math.ceil(w / bl) * bpe / 256) * 256;
    off = Math.ceil(off / 512) * 512;
    out.push({ offset: off, pitch, rows, width: w, height: h, depth: d, mip, layer });
    off += pitch * rows * d;
  }
  return { subs: out, size: off };
}

export async function startBackend({ memory, ring: ringAddr, device, canvas, dir, log = console.log, onFrame = null, keepAll = false, onShot = null }) {
  const ring = new Ring(memory, ringAddr), g = ring.g;
  const res = new Map(), rootsigs = new Map(), psos = new Map(), shaders = new Map(), queryHeaps = new Map();
  const vas = []; // buffers by address: [va, end, r]
  const errors = { count: 0 };
  device.onuncapturederror = (e) => { if (errors.count++ < 20) log(`gpu: WebGPU error: ${e.error.message}`); };
  const ctx = canvas.getContext('webgpu'), canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
  let samplerCache = new Map(), pipelineCache = new Map(), presents = 0;
  const stats = { executes: 0, draws: 0, presents: 0, segments: 0, readbacks: 0, errors, skipped: {} };
  // bind groups: each pipeline's layouts kept, and within a segment a group of the same objects made once (a draw
  // each made one, at hundreds of draws a frame; resources are not destroyed inside a segment)
  const layouts = new WeakMap(), ids = new WeakMap(); let nextId = 1;
  const idOf = (o) => { let i = ids.get(o); if (!i) ids.set(o, i = nextId++); return i; };
  const skip = (why) => { stats.skipped[why] = (stats.skipped[why] || 0) + 1; };

  const findVA = (va) => {
    let lo = 0, hi = vas.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (vas[m][0] <= va) lo = m + 1; else hi = m; }
    const e = vas[lo - 1]; return e && va < e[1] ? { r: e[2], offset: va - e[0] } : null;
  };
  // an address in guest memory (untagged, or in the console's GPU-visible region), not a PC GPU-only buffer's
  const isGuest = (va) => { const hi = Math.floor(va / 4294967296); return hi === 0 || (hi >= 4 && hi < 8); };
  const guestBytes = (a, n) => { g.refresh(); return new Uint8Array(g.buf, a >>> 0, n); };
  let writeShared = true; // writeBuffer / writeTexture straight from shared memory, until the browser refuses
  const writeBuf = (buf, off, src) => {
    if (writeShared) { try { device.queue.writeBuffer(buf, off, src); return; } catch (e) { writeShared = false; log(`gpu: writeBuffer from shared memory refused (${e.message}): copying`); } }
    device.queue.writeBuffer(buf, off, src.slice());
  };

  // ---- resources ----
  function createResource(w) {
    const [id, dim, fmt, wlo, whi, height, depth, mips, samples, flags, heap, backing, valo, vahi] = w;
    const r = { id, dim, dxgi: fmt, format: FORMATS[fmt], width: u64(wlo, whi), height, depth, mips, samples, flags, heap, backing, va: u64(valo, vahi), views: new Map() };
    if (dim === 0) {
      r.size = r.width;
      const usage = GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST | GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.UNIFORM | GPUBufferUsage.STORAGE | GPUBufferUsage.INDIRECT;
      // A D3D12 buffer (flag 0x80000000) gets 64 KiB past the end: a constant buffer view near the end still has
      // the 64 KiB its translated shader declares (D3D12's limit; WebGPU checks the bound range against it).
      // Direct3D 9's buffers are their own size. Only the real size is ever uploaded.
      r.gpu = device.createBuffer({ size: Math.ceil(r.size / 4) * 4 + (flags & 0x80000000 ? 65536 : 0), usage });
      const at = vas.findIndex((e) => e[0] > r.va); vas.splice(at < 0 ? vas.length : at, 0, [r.va, r.va + r.size, r]);
    } else {
      let f = r.format;
      if (!f) { log(`gpu: texture ${id}: DXGI format ${fmt} has no WebGPU format; using rgba8unorm`); f = 'rgba8unorm'; }
      if ((flags & 2) && !isDepth(f)) f = fmt === 39 || fmt === 41 ? 'depth32float' : 'depth24plus-stencil8';
      r.format = f;
      let usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC;
      if (flags & 3) usage |= GPUTextureUsage.RENDER_ATTACHMENT;
      if ((flags & 4) && !isDepth(f) && !f.endsWith('srgb') && !f.startsWith('bc')) usage |= GPUTextureUsage.STORAGE_BINDING;
      const size = { width: r.width, height: dim === 1 ? 1 : height, depthOrArrayLayers: depth };
      const viewFormats = SRGB[f] ? [SRGB[f]] : [];
      r.gpu = device.createTexture({ size, format: f, usage, mipLevelCount: mips, sampleCount: samples, dimension: dim === 3 ? '3d' : dim === 1 ? '1d' : '2d', viewFormats });
      if (backing) { r.app = titleLayout(r); r.hash = -1; } // texels in app memory: the GPU follows them
    }
    res.set(id, r);
  }
  const view = (r, { format, dim, mip = 0, mips = 1, slice = 0, slices = 1, aspect = 'all' }) => {
    const key = `${format}|${dim}|${mip}|${mips}|${slice}|${slices}|${aspect}`;
    let v = r.views.get(key);
    if (!v) {
      const d = { format, dimension: dim, baseMipLevel: mip, mipLevelCount: mips, baseArrayLayer: slice, arrayLayerCount: dim === '3d' ? 1 : slices, aspect };
      if (dim === 'cube') d.arrayLayerCount = 6;
      v = r.gpu.createView(d); r.views.set(key, v);
    }
    return v;
  };
  const descriptor = (a) => { g.refresh(); return g.u32.slice(a >>> 2, (a >>> 2) + 8); };
  const viewFormat = (r, dxgi) => { const f = dxgi ? FORMATS[dxgi] : r.format; return f && (f === r.format || SRGB[r.format] === f) ? f : r.format; };
  // a texture in app memory: uploaded again when its bytes changed (no write protection in wasm: a hash)
  const hashGuest = (a, n) => { g.refresh(); const w = new Uint32Array(g.buf, a >>> 0, n >>> 2); let h = 2166136261; for (let i = 0; i < w.length; i++) h = Math.imul(h ^ w[i], 16777619); return h >>> 0; };
  const syncTitleTexture = (r) => {
    if (!r?.app) return;
    const h = hashGuest(r.backing, r.app.size); if (h === r.hash) return;
    r.hash = h;
    for (const s of r.app.subs) {
      const src = guestBytes(r.backing + s.offset, s.pitch * s.rows * s.depth), dst = { texture: r.gpu, mipLevel: s.mip, origin: { x: 0, y: 0, z: r.dim === 3 ? 0 : s.layer } };
      const layout = { bytesPerRow: s.pitch, rowsPerImage: s.rows }, size = { width: s.width, height: s.height, depthOrArrayLayers: s.depth };
      if (writeShared) { try { device.queue.writeTexture(dst, src, layout, size); continue; } catch (e) { writeShared = false; } }
      device.queue.writeTexture(dst, src.slice(), layout, size);
    }
  };

  const explicit = startExplicit({ device, g, res, view, writeBuf, log, stats, tables: { FORMATS, VERTEX_FORMATS, BLEND, BLEND_OP, COMPARE, TOPOLOGY, ADDRESS }, texelBytes });

  // ---- shaders and pipelines ----
  const shader = (digest) => {
    if (!shaders.has(digest)) shaders.set(digest, (async () => {
      const [w, j] = await Promise.all([fetch(`${dir}shaders/${digest}.wgsl`), fetch(`${dir}shaders/${digest}.json`)]);
      if (!w.ok || !j.ok) { log(`gpu: shader ${digest} has no translation (tools/shader-prep.mjs)`); return null; }
      const code = await w.text(), info = await j.json();
      const module = device.createShaderModule({ code });
      const types = {};
      for (const m of code.matchAll(/@group\((\d+)\) @binding\((\d+)\)\s*var(?:<[^>]*>)?\s+\w+\s*:\s*([^;]+);/g)) types[`${m[1]}/${m[2]}`] = m[3].trim();
      return { digest, module, info, types };
    })());
    return shaders.get(digest);
  };
  function createPSO(at, n) {
    g.refresh();
    const w = g.u32, h = at >>> 2, id = w[h], rootId = w[h + 1], kind = w[h + 2], ne = w[h + 3];
    const d = at + 16, dv = new DataView(g.buf), r32 = (o) => dv.getUint32(d + o, true), f32 = (o) => dv.getFloat32(d + o, true);
    const blob = (o) => { const off = Number(dv.getBigUint64(d + o, true)), size = Number(dv.getBigUint64(d + o + 8, true)); return off && size ? g.copy(at + off, size) : null; };
    // a DXIL container is translated; a console one (RDNA2 code) is not: its pipeline draws nothing
    const digestOf = (b) => Array.from(b.subarray(4, 20), (x) => x.toString(16).padStart(2, '0')).join('');
    const p = { id, rootId, kind };
    if (kind === 1) { const cs = blob(8); p.cs = cs && shader(digestOf(cs)); p.csBlob = cs; psos.set(id, p); return; }
    const vs = blob(8), ps = blob(24);
    p.vsBlob = vs; p.vs = vs && shader(digestOf(vs)); p.ps = ps && shader(digestOf(ps));
    p.blend = { a2c: r32(120), independent: r32(124), rt: [] };
    for (let i = 0; i < 8; i++) { const o = 128 + 40 * i; p.blend.rt.push({ enable: r32(o), src: r32(o + 8), dst: r32(o + 12), op: r32(o + 16), srcA: r32(o + 20), dstA: r32(o + 24), opA: r32(o + 28), mask: dv.getUint8(d + o + 36) }); }
    p.sampleMask = r32(448);
    p.raster = { fill: r32(452), cull: r32(456), ccw: r32(460), bias: dv.getInt32(d + 464, true), biasClamp: f32(468), slope: f32(472), clip: r32(476) };
    const face = (o) => ({ fail: r32(o), depthFail: r32(o + 4), pass: r32(o + 8), func: r32(o + 12) });
    p.depth = { enable: r32(496), write: r32(500), func: r32(504), stencil: r32(508), readMask: dv.getUint8(d + 512), writeMask: dv.getUint8(d + 513), front: face(516), back: face(532) };
    p.elements = [];
    for (let i = 0; i < ne; i++) {
      const e = at + 16 + 656 + 32 * i;
      p.elements.push({ name: g.str(at + Number(dv.getBigUint64(e, true))), index: dv.getUint32(e + 8, true), format: dv.getUint32(e + 12, true), slot: dv.getUint32(e + 16, true), offset: dv.getUint32(e + 20, true), instance: dv.getUint32(e + 24, true) === 1, step: dv.getUint32(e + 28, true) });
    }
    p.topologyType = r32(572); p.numRT = r32(576); p.rtv = []; for (let i = 0; i < 8; i++) p.rtv.push(r32(580 + 4 * i));
    p.dsv = r32(612); p.samples = r32(616) || 1;
    psos.set(id, p);
  }
  async function renderPipeline(p, st, colorFormats, depthFormat, samples, ibFormat) {
    const strides = p.elements.map((e) => st.vbs[e.slot]?.stride ?? 0).join(',');
    const bias = st.bias ? st.bias.join('/') : '';
    const key = `${p.id}|${st.topo}|${strides}|${ibFormat ?? ''}|${colorFormats.join(',')}|${depthFormat}|${samples}|${bias}`;
    let pl = pipelineCache.get(key);
    if (pl !== undefined) return pl;
    const vs = await p.vs, ps = p.ps ? await p.ps : null;
    if (!vs || (p.ps && !ps)) { pipelineCache.set(key, null); return null; }
    // vertex buffers: the input layout matched to the shader's inputs by semantic
    const buffers = [], running = {};
    for (const e of p.elements) {
      const loc = vs.info.inputs.find((i) => i.semantic.toUpperCase() === e.name.toUpperCase() && i.index === e.index);
      const format = VERTEX_FORMATS[e.format] || 'float32x4', size = VERTEX_SIZE[format] || 4;
      const off = e.offset === 0xFFFFFFFF ? (running[e.slot] ?? 0) : e.offset;
      running[e.slot] = off + size;
      if (!loc) continue;
      buffers[e.slot] ??= { arrayStride: st.vbs[e.slot]?.stride ?? 0, stepMode: e.instance ? 'instance' : 'vertex', attributes: [] };
      buffers[e.slot].attributes.push({ shaderLocation: loc.location, offset: off, format });
    }
    for (let i = 0; i < buffers.length; i++) buffers[i] ??= { arrayStride: 0, attributes: [] };
    const targets = colorFormats.map((f, i) => {
      if (!f) return null;
      const b = p.blend.independent ? p.blend.rt[i] : p.blend.rt[0];
      const t = { format: f, writeMask: b.mask & 0xF };
      if (b.enable && !f.endsWith('uint') && !f.endsWith('sint')) t.blend = { color: { srcFactor: BLEND[b.src] || 'one', dstFactor: BLEND[b.dst] || 'zero', operation: BLEND_OP[b.op] || 'add' }, alpha: { srcFactor: BLEND[b.srcA] || 'one', dstFactor: BLEND[b.dstA] || 'zero', operation: BLEND_OP[b.opA] || 'add' } };
      if (t.blend) for (const c of [t.blend.color, t.blend.alpha]) if (c.operation === 'min' || c.operation === 'max') { c.srcFactor = 'one'; c.dstFactor = 'one'; }
      return t;
    });
    const topology = TOPOLOGY[st.topo] || 'triangle-list';
    const desc = {
      layout: 'auto',
      vertex: { module: vs.module, entryPoint: 'main', buffers },
      primitive: { topology, cullMode: [null, 'none', 'front', 'back'][p.raster.cull] || 'none', frontFace: p.raster.ccw ? 'ccw' : 'cw', unclippedDepth: false },
      multisample: { count: samples, mask: p.sampleMask >>> 0, alphaToCoverageEnabled: !!p.blend.a2c },
    };
    if (topology.endsWith('strip') && ibFormat) desc.primitive.stripIndexFormat = ibFormat;
    if (ps) desc.fragment = { module: ps.module, entryPoint: 'main', targets };
    if (depthFormat) {
      const face = (f) => ({ compare: COMPARE[f.func] || 'always', failOp: STENCIL_OP[f.fail] || 'keep', depthFailOp: STENCIL_OP[f.depthFail] || 'keep', passOp: STENCIL_OP[f.pass] || 'keep' });
      desc.depthStencil = { format: depthFormat, depthWriteEnabled: !!(p.depth.enable && p.depth.write), depthCompare: p.depth.enable ? COMPARE[p.depth.func] || 'less' : 'always' };
      if (p.depth.stencil && depthFormat.includes('stencil')) Object.assign(desc.depthStencil, { stencilFront: face(p.depth.front), stencilBack: face(p.depth.back), stencilReadMask: p.depth.readMask, stencilWriteMask: p.depth.writeMask });
      // RSSetDepthBiasX overrides the pipeline's own bias; WebGPU's constant bias is an integer
      if (topology.startsWith('triangle')) Object.assign(desc.depthStencil, st.bias ? { depthBias: Math.round(st.bias[0]), depthBiasClamp: st.bias[1], depthBiasSlopeScale: st.bias[2] } : { depthBias: p.raster.bias, depthBiasSlopeScale: p.raster.slope, depthBiasClamp: p.raster.biasClamp });
    }
    try { pl = await device.createRenderPipelineAsync(desc); } catch (e) { log(`gpu: pipeline ${p.id}: ${e.message}`); pl = null; }
    pl && (pl.stages = [vs, ps].filter(Boolean));
    pipelineCache.set(key, pl);
    return pl;
  }
  const sampler = (s) => {
    const key = `${s.filter}|${s.u}|${s.v}|${s.w}|${s.aniso}|${s.compare}`;
    let v = samplerCache.get(key);
    if (!v) {
      const f = s.filter, aniso = (f & 0x55) === 0x55, lin = (b) => (b ? 'linear' : 'nearest');
      const d = { addressModeU: ADDRESS[s.u] || 'repeat', addressModeV: ADDRESS[s.v] || 'repeat', addressModeW: ADDRESS[s.w] || 'repeat',
        minFilter: lin(aniso || f & 0x10), magFilter: lin(aniso || f & 0x4), mipmapFilter: lin(aniso || f & 0x1) };
      if (aniso && s.aniso > 1) d.maxAnisotropy = Math.min(16, s.aniso);
      if (f & 0x80) d.compare = COMPARE[s.compare] || 'less';
      v = device.createSampler(d); samplerCache.set(key, v);
    }
    return v;
  };

  // ---- one submission ----
  // Guest memory is the truth for buffers with backing (all of the console's): a segment of work uploads the
  // ranges it reads before it runs and reads back the ranges it writes before anything later runs. A segment
  // ends where a memory operation (Write/Fill/Copy/Wait...X) has to see the work before it.
  async function execute(at, n) {
    const w = words(g, at, n >> 2);
    const st = { pso: null, root: [null, null], args: [[], []], rts: [], dsv: null, vp: null, sc: null, topo: 4, vbs: [], ib: null, blend: [1, 1, 1, 1], stencil: 0, bias: null, active: new Set() };
    const clears = new Map(), sums = new Map(); // occlusion: query key -> samples, over the whole submission
    let groupCache, enc, uniforms, uploads, written, temps, pass, passKey, passSize, passFormats, passDepth, passSamples, qs, qn, drawQueries, resolves;
    const begin = () => {
      groupCache = new Map();
      enc = device.createCommandEncoder(); uniforms = []; uploads = new Map(); written = new Map(); temps = [];
      pass = null; passKey = ''; passSize = null; passFormats = null; passDepth = null; passSamples = 1; qs = null; qn = 0; drawQueries = []; resolves = [];
      st.uniformBuffer = device.createBuffer({ size: 256 * 1024, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    };
    begin();
    const endPass = () => { if (pass) { pass.end(); pass = null; passKey = ''; } };
    // what the segment reads of a buffer in guest memory (offset, bytes; undefined: all of it)
    const useBuffer = (r, off = 0, size) => { if (!r || !r.backing || r.dim !== 0) return; const l = uploads.get(r) || []; l.push([off, size ?? r.size - off]); uploads.set(r, l); };
    const wrote = (r, off = 0, size) => { if (!r || !r.backing || r.dim !== 0) return; const l = written.get(r) || []; l.push([off, size ?? r.size - off]); written.set(r, l); };
    // bytes of guest memory no resource holds (an index buffer at an app address): a buffer for this segment
    const guestBuffer = (va, size) => {
      const n4 = Math.max(4, Math.ceil(size / 4) * 4), b = device.createBuffer({ size: n4, usage: GPUBufferUsage.INDEX | GPUBufferUsage.VERTEX | GPUBufferUsage.UNIFORM | GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
      writeBuf(b, 0, guestBytes(va, n4)); temps.push(b); return { r: { gpu: b, size: n4 }, offset: 0 };
    };
    const bufferAt = (va, size) => { const f = findVA(va); if (f) { useBuffer(f.r, f.offset, size); return f; } return isGuest(va) && va ? guestBuffer(va, size) : null; };
    const rtView = (d) => { const r = res.get(d[1]); if (!r || !r.gpu.createView) return null; const f = viewFormat(r, d[2]); return { r, key: `${d[1]}/${d[4]}/${d[6]}/${f}`, v: view(r, { format: f, dim: '2d', mip: d[4], slice: d[6] }), f }; };
    const dsView = (d) => { const r = res.get(d[1]); if (!r) return null; return { r, key: `d${d[1]}/${d[4]}`, v: view(r, { format: r.format, dim: '2d', mip: d[4] }), f: r.format }; };
    const beginPass = () => {
      const colors = st.rts.map(rtView), depth = st.dsv && st.dsv[0] ? dsView(st.dsv) : null;
      const key = colors.map((c) => c?.key).join(',') + '|' + (depth?.key ?? '');
      if (pass && key === passKey) return true;
      endPass();
      const any = colors.find(Boolean) || depth; if (!any) return false;
      const desc = { colorAttachments: colors.map((c) => c && (clears.has(c.key) ? { view: c.v, loadOp: 'clear', clearValue: clears.get(c.key), storeOp: 'store' } : { view: c.v, loadOp: 'load', storeOp: 'store' })) };
      if (depth) {
        const cl = clears.get(depth.key) || {}, hasStencil = depth.f.includes('stencil');
        desc.depthStencilAttachment = { view: depth.v, depthLoadOp: cl.depth !== undefined ? 'clear' : 'load', depthClearValue: cl.depth ?? 1, depthStoreOp: 'store' };
        if (hasStencil) Object.assign(desc.depthStencilAttachment, { stencilLoadOp: cl.stencil !== undefined ? 'clear' : 'load', stencilClearValue: cl.stencil ?? 0, stencilStoreOp: 'store' });
      }
      if (qs) desc.occlusionQuerySet = qs;
      for (const c of colors) if (c) clears.delete(c.key);
      if (depth) clears.delete(depth.key);
      pass = enc.beginRenderPass(desc); passKey = key;
      const sz = any.r; passSize = [Math.max(1, sz.width), Math.max(1, sz.height)];
      passFormats = colors.map((c) => c?.f ?? null); passDepth = depth?.f ?? null; passSamples = any.r.samples || 1;
      pass.setBlendConstant(st.blend); pass.setStencilReference(st.stencil);
      return true;
    };
    const flushClears = () => {
      for (const [key] of clears) {
        // a clear no draw used: an empty pass that clears it
        const isD = key.startsWith('d'), id = +key.slice(isD ? 1 : 0).split('/')[0], r = res.get(id); if (!r) continue;
        const parts = key.split('/'), f = isD ? r.format : parts[3];
        const v = view(r, { format: f, dim: '2d', mip: +parts[1], slice: isD ? 0 : +parts[2] }), cl = clears.get(key);
        const d = isD ? { colorAttachments: [], depthStencilAttachment: Object.assign({ view: v, depthLoadOp: 'clear', depthClearValue: cl.depth ?? 1, depthStoreOp: 'store' }, f.includes('stencil') ? { stencilLoadOp: 'clear', stencilClearValue: cl.stencil ?? 0, stencilStoreOp: 'store' } : {}) }
          : { colorAttachments: [{ view: v, loadOp: 'clear', clearValue: cl, storeOp: 'store' }] };
        enc.beginRenderPass(d).end();
      }
      clears.clear();
    };
    // the root arguments of a binding, resolved against the root signature
    const resolve = (bp, kind, space, reg) => {
      const rs = rootsigs.get(st.root[bp]); if (!rs) return null;
      const want = { cbv: [2, 2], srv: [3, 0], uav: [4, 1], sampler: [-1, 3] }[kind];
      if (kind === 'sampler') { const s = rs.samplers.find((s) => s.reg === reg && s.space === space); if (s) return { sampler: s }; }
      for (let i = 0; i < rs.params.length; i++) {
        const p = rs.params[i], a = st.args[bp][i];
        if (p.type === want[0] && p.reg === reg && p.space === space) return a ? { va: a.va } : null;
        if (kind === 'cbv' && p.type === 1 && p.reg === reg && p.space === space) return { constants: a?.values ?? new Uint32Array(p.num), num: p.num };
        if (p.type === 0) for (const rg of p.ranges) {
          if (rg.type !== want[1] || rg.space !== space || reg < rg.base || (rg.count !== 0xFFFFFFFF && reg >= rg.base + rg.count)) continue;
          if (!a) return null;
          return { desc: descriptor(a.handle + GPU.DESC_BYTES * (rg.offset + reg - rg.base)) };
        }
      }
      return null;
    };
    const bindGroups = (pl, bp) => {
      const groups = [];
      for (const stg of pl.stages) for (const b of stg.info.resources) {
        const gkey = `${b.group}/${b.binding}`;
        groups[b.group] ??= new Map();
        if (groups[b.group].has(b.binding)) continue;
        const kind = b.kind, rr = resolve(bp, kind, b.space, b.register), type = stg.types[gkey] || '';
        let resource = null;
        if (kind === 'sampler') {
          const s = rr?.sampler ?? (rr?.desc ? { filter: rr.desc[1], u: rr.desc[2], v: rr.desc[3], w: rr.desc[4], aniso: rr.desc[5], compare: rr.desc[6] } : { filter: 0x15, u: 1, v: 1, w: 1 });
          resource = sampler(s);
        } else if (kind === 'cbv') {
          if (rr?.constants) { const off = uniforms.length * 256; uniforms.push(rr.constants); resource = { buffer: st.uniformBuffer, offset: off, size: 256 }; }
          else {
            const va = rr?.va ?? (rr?.desc ? u64(rr.desc[2], rr.desc[3]) : 0), size = rr?.desc ? rr.desc[4] : 65536, f = va && bufferAt(va, size || 65536);
            if (f) resource = { buffer: f.r.gpu, offset: f.offset, size: Math.min(65536, f.r.gpu.size - f.offset) };
          }
        } else if (rr?.desc && rr.desc[0] && !type.startsWith('array') && !type.startsWith('ptr')) {
          const r = res.get(rr.desc[1]);
          if (r && r.dim !== 0) {
            syncTitleTexture(r);
            const dimension = type.startsWith('texture_cube_array') ? 'cube-array' : type.startsWith('texture_cube') || type.startsWith('texture_depth_cube') ? 'cube' : type.includes('2d_array') ? '2d-array' : type.startsWith('texture_3d') || type.includes('storage_3d') ? '3d' : type.startsWith('texture_1d') || type.includes('storage_1d') ? '1d' : '2d';
            const d = rr.desc, f = isDepth(r.format) ? r.format : viewFormat(r, d[2]);
            resource = view(r, { format: f, dim: dimension, mip: d[4], mips: kind === 'uav' ? 1 : Math.min(d[5] === 0xFFFFFFFF || !d[5] ? r.mips - d[4] : d[5], r.mips - d[4]), slice: d[6], slices: dimension === '2d' || dimension === '3d' ? 1 : Math.max(1, Math.min(d[7] || r.depth, r.depth - d[6])), aspect: isDepth(r.format) ? 'depth-only' : 'all' });
          } else if (r) { useBuffer(r); if (kind === 'uav') wrote(r); resource = { buffer: r.gpu }; }
        } else if (rr?.va) { const f = bufferAt(rr.va); if (f) { if (kind === 'uav') wrote(f.r); resource = { buffer: f.r.gpu, offset: f.offset }; } }
        groups[b.group].set(b.binding, resource);
      }
      const out = [];
      for (let i = 0; i < groups.length; i++) {
        if (!groups[i]) continue;
        const entries = [];
        for (const [binding, resource] of groups[i]) { if (!resource) return null; entries.push({ binding, resource }); }
        let ls = layouts.get(pl); if (!ls) layouts.set(pl, ls = []);
        const layout = ls[i] ??= pl.getBindGroupLayout(i);
        const key = idOf(layout) + entries.map((e) => `|${e.binding}:` + (e.resource.buffer ? `${idOf(e.resource.buffer)}@${e.resource.offset ?? 0}/${e.resource.size ?? ''}` : idOf(e.resource))).join('');
        let bg = groupCache.get(key);
        if (!bg) { try { bg = device.createBindGroup({ layout, entries }); } catch (e) { log(`gpu: bind group ${i}: ${e.message}`); return null; } groupCache.set(key, bg); }
        out.push([i, bg]);
      }
      return out;
    };
    const draw = async (indexed, args, ibOverride) => {
      const p = psos.get(st.pso); if (!p || p.kind !== 0) return skip('no graphics pipeline state');
      if (st.active.size && !qs) { endPass(); qs = device.createQuerySet({ type: 'occlusion', count: 4096 }); temps.push(qs); } // passes need it from their start
      if (!beginPass()) return skip('no render target');
      let ib = null;
      if (indexed) { ib = ibOverride ?? (st.ib && { ...st.ib, at: bufferAt(st.ib.va, st.ib.size) }); if (!ib?.at) return skip('no index buffer'); }
      const pl = await renderPipeline(p, st, passFormats, passDepth, passSamples, ib?.format); if (!pl) return skip('no pipeline');
      const bgs = bindGroups(pl, 0); if (!bgs) return skip('a binding is missing');
      if (!pass) beginPass(); // (awaiting the pipeline never ends a pass; kept for safety)
      pass.setPipeline(pl);
      for (const [i, bg] of bgs) pass.setBindGroup(i, bg);
      for (let s = 0; s < st.vbs.length; s++) { const vb = st.vbs[s]; if (!vb) continue; const f = bufferAt(vb.va, vb.size); if (f) pass.setVertexBuffer(s, f.r.gpu, f.offset, Math.min(vb.size, f.r.gpu.size - f.offset)); }
      if (st.vp) { const [x, y, ww, hh, mn, mx] = st.vp; pass.setViewport(x, y, Math.max(0, ww), Math.max(0, hh), Math.max(0, Math.min(1, mn)), Math.max(0, Math.min(1, mx))); }
      const [pw, ph] = passSize;
      if (st.sc) { const [l, t, r, b] = st.sc, x0 = Math.max(0, Math.min(pw, l)), y0 = Math.max(0, Math.min(ph, t)); pass.setScissorRect(x0, y0, Math.max(0, Math.min(pw, r) - x0), Math.max(0, Math.min(ph, b) - y0)); }
      else pass.setScissorRect(0, 0, pw, ph);
      stats.draws++;
      // occlusion queries: each draw inside any gets its own WebGPU query (D3D12 nests them, WebGPU does not)
      const q = st.active.size && qs && qn < 4096 ? qn++ : -1;
      if (q >= 0) { pass.beginOcclusionQuery(q); drawQueries.push([q, [...st.active]]); }
      if (indexed) {
        pass.setIndexBuffer(ib.at.r.gpu, ib.format, ib.at.offset, Math.min(ib.size, ib.at.r.gpu.size - ib.at.offset));
        pass.drawIndexed(args[0], args[1], args[2], args[3] | 0, args[4]);
      } else pass.draw(args[0], args[1], args[2], args[3]);
      if (q >= 0) pass.endOcclusionQuery();
    };
    const location = (p) => ({ id: p[0], type: p[1], sub: p[2], offset: u64(p[3], p[4]), format: p[5], width: p[6], height: p[7], depth: p[8], pitch: p[9] });
    // the segment so far goes to the GPU; what it wrote of guest memory comes back before this returns
    const flush = async () => {
      endPass(); flushClears();
      if (uniforms.length) { const u = new Uint32Array(uniforms.length * 64); uniforms.forEach((v, k) => u.set(v.subarray(0, 64), k * 64)); device.queue.writeBuffer(st.uniformBuffer, 0, u); }
      // each buffer's ranges merged first: they all read guest memory now, so overlaps carry the same bytes (a root
      // CBV has no size and reads 64 KiB from its offset; one draw each into a constant ring made that 48 MB a frame)
      for (const [r, list] of uploads) {
        const end = Math.ceil(r.size / 4) * 4, spans = [];
        for (const [o, s] of list) { const a = Math.floor(o / 4) * 4, e = Math.min(end, Math.ceil((o + s) / 4) * 4); if (e > a) spans.push([a, e]); }
        spans.sort((x, y) => x[0] - y[0]);
        let cur = null;
        for (const sp of spans) { if (cur && sp[0] <= cur[1]) cur[1] = Math.max(cur[1], sp[1]); else { if (cur) writeBuf(r.gpu, cur[0], guestBytes(r.backing + cur[0], cur[1] - cur[0])); cur = sp; } }
        if (cur) writeBuf(r.gpu, cur[0], guestBytes(r.backing + cur[0], cur[1] - cur[0]));
      }
      // read back: ranges of written buffers, then the occlusion results, into one staging buffer
      const back = []; let total = 0;
      for (const [r, list] of written) for (const [o, s] of list) { const a = Math.floor(o / 4) * 4, e = Math.min(Math.ceil(r.size / 4) * 4, Math.ceil((o + s) / 4) * 4); if (e > a) { back.push([r, a, e - a, total]); total += e - a; } }
      let qAt = -1;
      if (qn) { const qb = device.createBuffer({ size: 8 * qn, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC }); enc.resolveQuerySet(qs, 0, qn, qb, 0); temps.push(qb); qAt = total; total += 8 * qn; back.push([{ gpu: qb }, 0, 8 * qn, qAt]); }
      let staging = null;
      if (total) { staging = device.createBuffer({ size: total, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }); for (const [r, a, s, to] of back) enc.copyBufferToBuffer(r.gpu, a, staging, to, s); }
      device.queue.submit([enc.finish()]);
      stats.segments++;
      const ub = st.uniformBuffer, tmp = temps; device.queue.onSubmittedWorkDone().then(() => { ub.destroy(); for (const b of tmp) b.destroy(); });
      if (staging) {
        await staging.mapAsync(GPUMapMode.READ); stats.readbacks++;
        const src = new Uint8Array(staging.getMappedRange()); g.refresh();
        for (const [r, a, s, to] of back) if (r.backing) new Uint8Array(g.buf, (r.backing >>> 0) + a, s).set(src.subarray(to, to + s));
        if (qAt >= 0) {
          const counts = new BigUint64Array(src.buffer.slice(qAt, qAt + 8 * qn));
          for (const [q, keys] of drawQueries) for (const k of keys) sums.set(k, (sums.get(k) ?? 0n) + counts[q]);
        }
        staging.unmap(); staging.destroy();
      }
      // ResolveQueryData: the sums so far, into the destination's guest memory
      for (const v of resolves) {
        const r = res.get(v.dst); if (!r?.backing) continue; g.refresh();
        for (let i = 0; i < v.count; i++) { let x = sums.get(`${v.heap}/${v.first + i}`) ?? 0n; if (v.type === 1 && x) x = 1n; g.dv.setBigUint64((r.backing >>> 0) + v.offset + 8 * i, x, true); }
      }
      begin();
    };
    let replay = 0; // words still to replay as state only (KickoffX ran them)
    for (let i = 0; i < w.length;) { // the list's words (the queue id is not part of them)
      const op = w[i] & 0xFFFF, len = w[i] >>> 16, a = w.subarray(i + 1, i + 1 + len);
      const work = replay <= 0;
      replay -= 1 + len; i += 1 + len;
      switch (op) {
      case GPU.L_REPLAY: replay = a[0]; break;
      case GPU.L_PSO: st.pso = a[0]; break;
      case GPU.L_TOPOLOGY: st.topo = a[0]; break;
      case GPU.L_ROOTSIG: st.root[a[0]] = a[1]; st.args[a[0]] = []; break;
      case GPU.L_TABLE: st.args[a[0]][a[1]] = { handle: u64(a[2], a[3]) }; break;
      case GPU.L_CONSTANTS: { const cur = st.args[a[0]][a[1]]?.values ?? new Uint32Array(64); const v = new Uint32Array(cur); v.set(a.subarray(4, 4 + a[3]), a[2]); st.args[a[0]][a[1]] = { values: v }; break; }
      case GPU.L_ROOT_VIEW: st.args[a[0]][a[1]] = { va: u64(a[3], a[4]) }; break;
      case GPU.L_VB: for (let k = 0; k < a[1]; k++) { const o = 2 + 4 * k; st.vbs[a[0] + k] = a[o + 2] ? { va: u64(a[o], a[o + 1]), size: a[o + 2], stride: a[o + 3] } : null; } break;
      case GPU.L_IB: st.ib = a[2] ? { va: u64(a[0], a[1]), size: a[2], format: a[3] === 57 ? 'uint16' : 'uint32' } : null; break;
      case GPU.L_VIEWPORTS: if (a[0]) st.vp = Array.from(new Float32Array(a.buffer, a.byteOffset + 4, 6)); break;
      case GPU.L_SCISSORS: if (a[0]) st.sc = Array.from(new Int32Array(a.buffer, a.byteOffset + 4, 4)); break;
      case GPU.L_BLEND_FACTOR: st.blend = Array.from(new Float32Array(a.buffer, a.byteOffset, 4)); if (pass) pass.setBlendConstant(st.blend); break;
      case GPU.L_STENCIL_REF: st.stencil = a[0]; if (pass) pass.setStencilReference(a[0]); break;
      case GPU.L_DEPTH_BIAS: { const f = Array.from(new Float32Array(a.buffer, a.byteOffset, 3)); st.bias = f.some((x) => x !== 0) ? f : null; break; }
      case GPU.L_SET_RTS: {
        const nrt = a[0]; st.rts = []; for (let k = 0; k < nrt; k++) { const d = a.slice(1 + 8 * k, 9 + 8 * k); st.rts.push(d[0] ? d : null); }
        const ds = a.slice(1 + 8 * nrt, 9 + 8 * nrt); st.dsv = ds[0] ? ds : null; break;
      }
      case GPU.L_QUERY_BEGIN: if (a[1] <= 1) st.active.add(`${a[0]}/${a[2]}`); break;
      case GPU.L_QUERY_END: st.active.delete(`${a[0]}/${a[2]}`); break;
      default: if (!work) break;
        switch (op) {
        case GPU.L_CLEAR_RTV: { const c = rtView(a); if (!c) break; if (pass && passKey.split('|')[0].split(',').includes(c.key)) endPass(); const f = new Float32Array(a.buffer, a.byteOffset + 32, 4); clears.set(c.key, { r: f[0], g: f[1], b: f[2], a: f[3] }); break; }
        case GPU.L_CLEAR_DSV: { const d = dsView(a); if (!d) break; if (pass && passKey.endsWith(d.key)) endPass(); const cl = clears.get(d.key) || {}; if (a[8] & 1) cl.depth = new Float32Array(a.buffer, a.byteOffset + 36, 1)[0]; if (a[8] & 2) cl.stencil = a[10]; clears.set(d.key, cl); break; }
        case GPU.L_DRAW: await draw(false, a); break;
        case GPU.L_DRAW_INDEXED: await draw(true, [a[0], a[1], a[2], a[3] | 0, a[4]]); break;
        case GPU.L_DRAW_INDEXED_X: { const format = a[2] === 57 ? 'uint16' : 'uint32', size = a[3] * (format === 'uint16' ? 2 : 4), va = u64(a[0], a[1]); await draw(true, [a[3], 1, 0, 0, 0], { va, size, format, at: bufferAt(va, size) }); break; }
        case GPU.L_COPY_BUFFER: {
          endPass(); flushClears();
          const d = res.get(a[0]), s = res.get(a[3]), n = u64(a[6], a[7]), so = u64(a[4], a[5]), dof = u64(a[1], a[2]); if (!d || !s) break;
          useBuffer(s, so, n); wrote(d, dof, n);
          enc.copyBufferToBuffer(s.gpu, so, d.gpu, dof, Math.ceil(n / 4) * 4); break;
        }
        case GPU.L_COPY_TEXTURE: {
          endPass(); flushClears();
          const dl = location(a.subarray(0, 10)), sl = location(a.subarray(13, 23)), box = a[23] ? a.subarray(24, 30) : null;
          const d = res.get(dl.id), s = res.get(sl.id); if (!d || !s) break;
          const sub = (r, x) => ({ mipLevel: x % r.mips, layer: Math.floor(x / r.mips) });
          if (dl.type === 0 && sl.type === 1) { // buffer footprint -> texture
            const m = sub(d, dl.sub), bl = blockOf(d.format);
            const wdt = box ? box[3] - box[0] : sl.width, hgt = box ? box[4] - box[1] : sl.height, dep = box ? box[5] - box[2] : sl.depth;
            const off = sl.offset + (box ? box[1] / bl * sl.pitch + box[0] / bl * (d.format.startsWith('bc') ? (d.format.startsWith('bc1') || d.format.startsWith('bc4') ? 8 : 16) : 4) : 0);
            useBuffer(s, off, sl.pitch * Math.ceil(hgt / bl) * dep);
            // block-compressed: the copy covers whole 4x4 blocks (a mip smaller than a block included)
            enc.copyBufferToTexture({ buffer: s.gpu, offset: off, bytesPerRow: sl.pitch, rowsPerImage: Math.ceil(sl.height / bl) }, { texture: d.gpu, mipLevel: m.mipLevel, origin: { x: a[10], y: a[11], z: d.dim === 3 ? a[12] : m.layer } }, { width: Math.ceil(wdt / bl) * bl, height: Math.ceil(hgt / bl) * bl, depthOrArrayLayers: dep });
          } else if (dl.type === 1 && sl.type === 0) { // texture -> buffer footprint
            syncTitleTexture(s);
            const m = sub(s, sl.sub), hgt = box ? box[4] - box[1] : dl.height, dep = box ? box[5] - box[2] : dl.depth;
            enc.copyTextureToBuffer({ texture: s.gpu, mipLevel: m.mipLevel, origin: { x: box ? box[0] : 0, y: box ? box[1] : 0, z: s.dim === 3 ? (box ? box[2] : 0) : m.layer } }, { buffer: d.gpu, offset: dl.offset, bytesPerRow: dl.pitch, rowsPerImage: Math.ceil(dl.height / blockOf(s.format)) }, { width: box ? box[3] - box[0] : dl.width, height: hgt, depthOrArrayLayers: dep });
            wrote(d, dl.offset, dl.pitch * Math.ceil(hgt / blockOf(s.format)) * dep);
          } else if (dl.type === 0 && sl.type === 0) {
            syncTitleTexture(s);
            const ms = sub(s, sl.sub), md = sub(d, dl.sub);
            const wd = Math.max(1, s.width >> ms.mipLevel), ht = Math.max(1, s.height >> ms.mipLevel);
            enc.copyTextureToTexture({ texture: s.gpu, mipLevel: ms.mipLevel, origin: { x: box ? box[0] : 0, y: box ? box[1] : 0, z: box ? box[2] : ms.layer } }, { texture: d.gpu, mipLevel: md.mipLevel, origin: { x: a[10], y: a[11], z: d.dim === 3 ? a[12] : md.layer } }, { width: box ? box[3] - box[0] : wd, height: box ? box[4] - box[1] : ht, depthOrArrayLayers: 1 });
          }
          break;
        }
        case GPU.L_COPY_RESOURCE: {
          endPass(); flushClears();
          const d = res.get(a[0]), s = res.get(a[1]); if (!d || !s) break;
          if (d.dim === 0) { useBuffer(s); const n4 = Math.min(s.gpu.size, d.gpu.size); enc.copyBufferToBuffer(s.gpu, 0, d.gpu, 0, n4); wrote(d, 0, n4); }
          else { syncTitleTexture(s); for (let m = 0; m < Math.min(s.mips, d.mips); m++) enc.copyTextureToTexture({ texture: s.gpu, mipLevel: m }, { texture: d.gpu, mipLevel: m }, { width: Math.max(1, s.width >> m), height: Math.max(1, s.height >> m), depthOrArrayLayers: s.dim === 3 ? Math.max(1, s.depth >> m) : s.depth }); }
          break;
        }
        case GPU.L_QUERY_RESOLVE: resolves.push({ heap: a[0], type: a[1], first: a[2], count: a[3], dst: a[4], offset: u64(a[5], a[6]) }); break;
        case GPU.L_MEM_WRITE: await flush(); memWrite(g, a); break;
        case GPU.L_MEM_FILL: await flush(); memFill(g, a); break;
        case GPU.L_MEM_COPY: await flush(); memCopy(g, a); break;
        case GPU.L_MEM_WAIT: await flush(); await memWait(g, a, log); break;
        default: break;
        }
      }
    }
    await flush();
    stats.executes++;
  }
  let pending = []; // (work a fence waits for besides the queue's; readbacks now finish inside execute)
  // ---- present: the frame into the canvas (and to onFrame, read back, when someone records) ----
  const blitModule = device.createShaderModule({ code: `
@group(0) @binding(0) var t: texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)); return vec4f(p * vec2f(2, -2) + vec2f(-1, 1), 0, 1); }
@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f { return vec4f(textureLoad(t, vec2i(p.xy), 0).rgb, 1); }` });
  const blit = device.createRenderPipeline({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format: canvasFormat }] }, primitive: { topology: 'triangle-list' } });
  // read-backs are 8-bit RGBA: a back buffer in another format (R10G10B10A2, half floats) is drawn into one first
  const blit8 = device.createRenderPipeline({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] }, primitive: { topology: 'triangle-list' } });
  let capturing = 0, shown = null;
  async function present(w) {
    const r = res.get(w[0]);
    shown = r;
    if (r && r.dim !== 0) {
      if (canvas.width !== r.width || canvas.height !== r.height) { canvas.width = r.width; canvas.height = r.height; }
      const enc = device.createCommandEncoder();
      const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
      pass.setPipeline(blit);
      pass.setBindGroup(0, device.createBindGroup({ layout: blit.getBindGroupLayout(0), entries: [{ binding: 0, resource: view(r, { format: r.format, dim: '2d' }) }] }));
      pass.draw(3); pass.end();
      let rb = null, pitch = 0;
      let rbFormat = r.format;
      if (onFrame && (keepAll || capturing < 3) && !r.format.startsWith('bc')) {
        pitch = Math.ceil(r.width * 4 / 256) * 256;
        rb = device.createBuffer({ size: pitch * r.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        let src = r.gpu;
        if (!/^(rgba8|bgra8)unorm/.test(r.format)) {
          src = device.createTexture({ size: { width: r.width, height: r.height }, format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
          const p8 = enc.beginRenderPass({ colorAttachments: [{ view: src.createView(), loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
          p8.setPipeline(blit8); p8.setBindGroup(0, device.createBindGroup({ layout: blit8.getBindGroupLayout(0), entries: [{ binding: 0, resource: view(r, { format: r.format, dim: '2d' }) }] })); p8.draw(3); p8.end();
          rbFormat = 'rgba8unorm'; const t = src; device.queue.onSubmittedWorkDone().then(() => t.destroy());
        }
        enc.copyTextureToBuffer({ texture: src }, { buffer: rb, bytesPerRow: pitch }, { width: r.width, height: r.height });
      }
      device.queue.submit([enc.finish()]);
      if (rb) {
        capturing++;
        const t = performance.now(), n = presents;
        rb.mapAsync(GPUMapMode.READ).then(() => {
          const src = new Uint8Array(rb.getMappedRange()), px = new Uint8Array(r.width * r.height * 4);
          for (let y = 0; y < r.height; y++) px.set(src.subarray(y * pitch, y * pitch + r.width * 4), y * r.width * 4);
          rb.unmap(); rb.destroy(); capturing--;
          onFrame({ index: n, time: t, width: r.width, height: r.height, format: rbFormat, pixels: px });
        });
      }
    }
    presents++; stats.presents = presents;
    device.queue.onSubmittedWorkDone().then(() => { ring.set(GPU.H_PRESENTS, presents); ring.bump(); });
  }
  // "shot <name>" (GPU_R_MARK): the frame presented last, read back and handed to onShot (the page saves it)
  async function mark(text) {
    const r = shown, name = text.startsWith('shot ') ? text.slice(5) : null;
    if (!name || !onShot || !r || r.dim === 0) return;
    const pitch = Math.ceil(r.width * 4 / 256) * 256, rb = device.createBuffer({ size: pitch * r.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }), enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: r.gpu }, { buffer: rb, bytesPerRow: pitch }, { width: r.width, height: r.height });
    device.queue.submit([enc.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(rb.getMappedRange()), px = new Uint8Array(r.width * r.height * 4);
    for (let y = 0; y < r.height; y++) px.set(src.subarray(y * pitch, y * pitch + r.width * 4), y * r.width * 4);
    rb.unmap(); rb.destroy();
    await onShot({ name, index: presents - 1, width: r.width, height: r.height, format: r.format, pixels: px });
  }

  // ---- the ring ----
  ring.run(async (op, at, n) => {
    switch (op) {
    case GPU.R_RESOURCE: createResource(words(g, at, 14)); break;
    case GPU.R_DESTROY: { const r = res.get(g.u32[at >>> 2]); if (r) { res.delete(r.id); const k = vas.findIndex((e) => e[2] === r); if (k >= 0) vas.splice(k, 1); const gpu = r.gpu; if (!explicit.retire(r)) device.queue.onSubmittedWorkDone().then(() => gpu.destroy()); } break; }
    case GPU.R_ROOTSIG: { const id = g.u32[at >>> 2], len = g.u32[(at >>> 2) + 1]; rootsigs.set(id, parseRootSignature(g.copy(at + 8, len))); break; }
    case GPU.R_PSO: createPSO(at, n); { const p = psos.get(g.u32[at >>> 2]); if (p && !rootsigs.get(p.rootId) && p.vsBlob) { p.rootId = -p.id; rootsigs.set(p.rootId, parseRootSignature(p.vsBlob)); } } break;
    case GPU.R_EXECUTE: await execute(at + 4, n - 4); break;
    case GPU.R_SIGNAL: {
      const fence = g.u32[at >>> 2], v = BigInt(g.u32[(at >>> 2) + 1]) | BigInt(g.u32[(at >>> 2) + 2]) << 32n, wait = pending; pending = [];
      Promise.all([device.queue.onSubmittedWorkDone(), ...wait]).then(() => { g.refresh(); g.dv.setBigUint64(fence + GPU.FENCE_VALUE, v, true); ring.bump(); });
      break;
    }
    case GPU.R_WAIT: { const fence = g.u32[at >>> 2], v = BigInt(g.u32[(at >>> 2) + 1]) | BigInt(g.u32[(at >>> 2) + 2]) << 32n; while (g.u64(fence + GPU.FENCE_VALUE) < v) await ring.waitEpoch(); break; }
    case GPU.R_PRESENT: explicit.flush(); await present(words(g, at, 4)); break;
    case GPU.R_MARK: explicit.flush(); await mark(new TextDecoder().decode(g.copy(at, n)).replace(/\0+$/, '')); break;
    default: if (isExplicit(op)) await explicit.handle(op, at); break;
    }
  });
  return stats;
}
