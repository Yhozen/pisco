// SPDX-License-Identifier: GPL-2.0-or-later
// The explicit records (rt/d3d12_cmd.h: GPU_R_SHADER ... GPU_R_DRAW) on WebGPU, as a part of backend.js: the
// front end already speaks WebGPU's terms, so this only keeps the objects and the order. Shaders are WGSL text,
// a pipeline comes with its vertex layout and its bind layout (group 0: a uniform block at a dynamic offset,
// then perhaps a texture and a sampler), a draw names its pipeline, buffers and bindings and brings its uniform
// block. Resources (buffers, textures) are backend.js's own (GPU_R_RESOURCE).
//
// Order. WebGPU runs queue writes at once and passes only when submitted, so one encoder collects passes until
// a present (or a mark) submits it, and a write that draws already recorded must not see goes one of three ways,
// by its mode: ORDERED submits what was recorded first; DISCARD gives the buffer new storage (the recorded draws
// keep the old); APPEND just writes (it touches bytes no recorded draw reads). Uniform blocks go into one arena
// buffer, written whole before the encoder is submitted.
import { GPU } from './cmd.gen.js';
import { readShader, readSampler, readPipeline, readPass, readViewport, readDraw } from './records.js';

export function startExplicit({ device, g, res, view, writeBuf, log, stats, tables, texelBytes }) {
  const { FORMATS, VERTEX_FORMATS, BLEND, BLEND_OP, COMPARE, TOPOLOGY, ADDRESS } = tables;
  const shaders = new Map(), samplers = new Map(), pipelines = new Map(), layouts = new Map();
  const align = device.limits.minUniformBufferOffsetAlignment;
  const arena = { size: 4 << 20, used: 0, cpu: null, gpu: null };
  const n = stats.explicit = { shaders: 0, pipelines: 0, passes: 0, draws: 0, submits: 0, renames: 0, orderedFlushes: 0, bufferWrites: 0, textureWrites: 0, readbacks: 0, bindGroups: 0, skipped: {} };
  const skip = (why) => { n.skipped[why] = (n.skipped[why] || 0) + 1; };
  let enc = null, pass = null, target = null, viewport = null, current = null, retired = [];
  const used = new Set();   // what the open encoder's passes read or draw into: not to be written or destroyed under it

  // ---- objects ----
  async function shader(at) {
    const s = readShader(g, at), module = device.createShaderModule({ code: s.code });
    shaders.set(s.id, { module }); n.shaders++;
    const info = await module.getCompilationInfo();
    for (const m of info.messages) if (m.type === 'error') log(`gpu: shader ${s.id}, line ${m.lineNum}: ${m.message}\n${s.code.split('\n')[m.lineNum - 1] ?? ''}`);
  }
  function sampler(at) {
    const s = readSampler(g, at), lin = (b) => (b ? 'linear' : 'nearest');
    const d = { addressModeU: ADDRESS[s.u] || 'clamp-to-edge', addressModeV: ADDRESS[s.v] || 'clamp-to-edge', addressModeW: ADDRESS[s.w] || 'clamp-to-edge',
      magFilter: lin(s.mag), minFilter: lin(s.min), mipmapFilter: lin(s.mip), lodMaxClamp: s.maxLod };
    if (s.anisotropy > 1 && s.mag && s.min && s.mip) d.maxAnisotropy = Math.min(16, s.anisotropy);
    samplers.set(s.id, { id: s.id, gpu: device.createSampler(d) });
  }
  // a bind layout, shared by the pipelines that list the same bindings; its bind groups hang off it (and off textures)
  const viewDim = (kind) => (kind === GPU.BIND_TEXTURE_3D ? '3d' : kind === GPU.BIND_TEXTURE_CUBE ? 'cube' : '2d');
  function layoutOf(bindings) {
    const sig = bindings.map((b) => `${b.binding}:${b.kind}:${b.size}`).join(',');
    let l = layouts.get(sig);
    if (!l) {
      const entries = bindings.map((b) => (b.kind === GPU.BIND_UNIFORM ? { binding: b.binding, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: b.size } }
        : b.kind === GPU.BIND_SAMPLER ? { binding: b.binding, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } }
        : { binding: b.binding, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: viewDim(b.kind) } }));
      const group = device.createBindGroupLayout({ entries });
      l = { sig, bindings, group, pipeline: device.createPipelineLayout({ bindGroupLayouts: [group] }), plain: null };
      layouts.set(sig, l);
    }
    return l;
  }
  async function pipeline(at) {
    const p = readPipeline(g, at), vs = shaders.get(p.vs), fs = shaders.get(p.fs);
    pipelines.set(p.id, p); n.pipelines++;
    if (!vs || !fs) { log(`gpu: pipeline ${p.id} names a shader that does not exist`); return; }
    p.layout = layoutOf(p.bindings); p.uniformSize = p.bindings.find((b) => b.kind === GPU.BIND_UNIFORM)?.size ?? 0;
    p.color = FORMATS[p.colorFormat] ?? null; p.depth = p.depthFormat ? FORMATS[p.depthFormat] : null;
    const topology = TOPOLOGY[p.topology] || 'triangle-list';
    const desc = {
      layout: p.layout.pipeline,
      vertex: { module: vs.module, entryPoint: 'main', buffers: p.buffers.map((b, k) => ({ arrayStride: b.stride, stepMode: b.step ? 'instance' : 'vertex',
        attributes: p.attributes.filter((a) => a.buffer === k).map((a) => ({ shaderLocation: a.location, offset: a.offset, format: VERTEX_FORMATS[a.format] })) })) },
      fragment: { module: fs.module, entryPoint: 'main', targets: [] },
      primitive: { topology, cullMode: ['none', 'front', 'back'][p.cull] || 'none', frontFace: p.frontFace ? 'cw' : 'ccw' },
    };
    if (topology.endsWith('strip')) desc.primitive.stripIndexFormat = 'uint16';
    if (p.color) {
      const t = { format: p.color, writeMask: p.writeMask & 15 };
      if (p.blend) t.blend = { color: { srcFactor: BLEND[p.blend.src] || 'one', dstFactor: BLEND[p.blend.dst] || 'zero', operation: BLEND_OP[p.blend.op] || 'add' },
        alpha: { srcFactor: BLEND[p.blend.srcA] || 'one', dstFactor: BLEND[p.blend.dstA] || 'zero', operation: BLEND_OP[p.blend.opA] || 'add' } };
      if (t.blend) for (const c of [t.blend.color, t.blend.alpha]) if (c.operation === 'min' || c.operation === 'max') { c.srcFactor = 'one'; c.dstFactor = 'one'; }
      desc.fragment.targets.push(t);
    }
    if (p.depth) {
      desc.depthStencil = { format: p.depth, depthWriteEnabled: !!p.depthWrite, depthCompare: COMPARE[p.depthCompare] || 'always' };
      if (topology.startsWith('triangle')) Object.assign(desc.depthStencil, { depthBias: p.depthBias, depthBiasSlopeScale: p.slope });
    }
    try { p.gpu = await device.createRenderPipelineAsync(desc); } catch (e) { log(`gpu: pipeline ${p.id}: ${e.message}`); p.gpu = null; }
  }
  // the resources of a draw's bindings, in the pipeline's order (null for the uniform block); the bind groups are
  // cached on the first texture (its ids, and the samplers', make the key: ids are never used again)
  function bindGroup(p, items) {
    const l = p.layout, tex = items.find((x) => x && x.dim !== undefined) ?? null, cache = tex ? (tex.groups ??= new Map()) : null;
    const key = tex ? `${l.sig}|${items.map((x) => x?.id ?? '').join(',')}` : '';
    let bg = tex ? cache.get(key) : l.plain;
    if (bg) return bg;
    if (!arena.gpu) { arena.cpu = new Uint8Array(arena.size); arena.gpu = device.createBuffer({ size: arena.size, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }); }
    const entries = l.bindings.map((b, i) => ({ binding: b.binding, resource: b.kind === GPU.BIND_UNIFORM ? { buffer: arena.gpu, offset: 0, size: b.size }
      : b.kind === GPU.BIND_SAMPLER ? items[i].gpu : view(items[i], { format: items[i].format, dim: viewDim(b.kind), mip: 0, mips: items[i].mips }) }));
    bg = device.createBindGroup({ layout: l.group, entries }); n.bindGroups++;
    if (tex) cache.set(key, bg); else l.plain = bg;
    return bg;
  }

  // ---- passes ----
  const endPass = () => { if (pass) { pass.end(); pass = null; } };
  function applyViewport() {
    if (!pass || !viewport || !target) return;
    const W = Math.max(1, target.color.width >> target.mip), H = Math.max(1, target.color.height >> target.mip), v = viewport, clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));
    const x = clamp(v.x, 0, W), y = clamp(v.y, 0, H), lo = clamp(v.minDepth, 0, 1);
    pass.setViewport(x, y, clamp(v.width, 0, W - x), clamp(v.height, 0, H - y), lo, clamp(v.maxDepth, lo, 1));
    const sx = clamp(v.scissor[0], 0, W), sy = clamp(v.scissor[1], 0, H);
    pass.setScissorRect(sx, sy, clamp(v.scissor[2], 0, W - sx), clamp(v.scissor[3], 0, H - sy));
  }
  // begins the pass on `target`: with the clears a GPU_R_PASS asked for, or (again, after a submit) loading everything
  function open(clear = {}) {
    enc ??= device.createCommandEncoder();
    const c = target.color, d = target.depth;
    const desc = { colorAttachments: [{ view: view(c, { format: c.format, dim: '2d', mip: target.mip }), loadOp: clear.clearColor ? 'clear' : 'load', clearValue: clear.clearColor ?? undefined, storeOp: 'store' }] };
    if (d) {
      desc.depthStencilAttachment = { view: view(d, { format: d.format, dim: '2d' }), depthLoadOp: clear.clearDepth != null ? 'clear' : 'load', depthClearValue: Math.min(1, Math.max(0, clear.clearDepth ?? 1)), depthStoreOp: 'store' };
      if (d.format.includes('stencil')) Object.assign(desc.depthStencilAttachment, { stencilLoadOp: clear.clearStencil != null ? 'clear' : 'load', stencilClearValue: clear.clearStencil ?? 0, stencilStoreOp: 'store' });
    }
    pass = enc.beginRenderPass(desc); current = null;
    used.add(c); if (d) used.add(d);
    applyViewport();
  }
  function beginPass(at) {
    const p = readPass(g, at), c = res.get(p.color), d = p.depth ? res.get(p.depth) : null;
    endPass(); n.passes++;
    if (!c || c.dim === 0 || (p.depth && !d)) { target = null; skip('a pass on a target that does not exist'); return; }
    target = { color: c, mip: p.mip, depth: d };
    open(p);
  }
  // what was recorded goes to the GPU; the pass, if one was open, is begun again by the next draw
  function flush() {
    endPass();
    if (!enc) return;
    if (arena.used) device.queue.writeBuffer(arena.gpu, 0, arena.cpu, 0, Math.ceil(arena.used / 4) * 4);
    device.queue.submit([enc.finish()]); n.submits++;
    enc = null; arena.used = 0; used.clear(); current = null;
    if (retired.length) { const list = retired; retired = []; device.queue.onSubmittedWorkDone().then(() => { for (const b of list) b.destroy(); }); }
  }

  // ---- writes ----
  function writeBuffer(at) {
    const w = g.u32, i = at >>> 2, r = res.get(w[i]), offset = w[i + 1], bytes = w[i + 2], mode = w[i + 3];
    if (!r || r.dim !== 0) return skip('a write to a buffer that does not exist');
    if (used.has(r)) {
      if (mode === GPU.WRITE_DISCARD) { retired.push(r.gpu); r.gpu = device.createBuffer({ size: r.gpu.size, usage: r.gpu.usage }); used.delete(r); n.renames++; }
      else if (mode === GPU.WRITE_ORDERED) { flush(); n.orderedFlushes++; }
    }
    writeBuf(r.gpu, offset, new Uint8Array(g.buf, at + 16, bytes)); n.bufferWrites++;
  }
  let shared = true;
  function writeTexture(at) {
    const w = g.u32, i = at >>> 2, r = res.get(w[i]);
    if (!r || r.dim === 0) return skip('a write to a texture that does not exist');
    if (used.has(r)) { flush(); n.orderedFlushes++; }
    const width = w[i + 5], height = w[i + 6], depth = w[i + 7], pitch = w[i + 8];
    const dst = { texture: r.gpu, mipLevel: w[i + 1], origin: { x: w[i + 2], y: w[i + 3], z: w[i + 4] } }, layout = { bytesPerRow: pitch, rowsPerImage: height }, size = { width, height, depthOrArrayLayers: depth };
    const src = new Uint8Array(g.buf, at + 36, pitch * height * depth); n.textureWrites++;
    if (shared) { try { device.queue.writeTexture(dst, src, layout, size); return; } catch { shared = false; } }
    device.queue.writeTexture(dst, src.slice(), layout, size);
  }

  // a level of a texture back into guest memory: what was recorded runs first; the ring moves on once the bytes are there
  async function readTexture(at) {
    const w = g.u32, i = at >>> 2, r = res.get(w[i]), mip = w[i + 1], layer = w[i + 2], width = w[i + 3], height = w[i + 4], dstPitch = w[i + 5], dst = w[i + 6];
    if (!r || r.dim !== 2) return skip('a read of a texture that does not exist');
    flush();
    const bpp = texelBytes(r.format), row = width * bpp, pitch = Math.ceil(row / 256) * 256;
    const rb = device.createBuffer({ size: pitch * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ }), e = device.createCommandEncoder();
    e.copyTextureToBuffer({ texture: r.gpu, mipLevel: mip, origin: { x: 0, y: 0, z: layer } }, { buffer: rb, bytesPerRow: pitch }, { width, height });
    device.queue.submit([e.finish()]); n.submits++;
    await rb.mapAsync(GPUMapMode.READ); n.readbacks++;
    const src = new Uint8Array(rb.getMappedRange()); g.refresh();
    const n8 = Math.min(row, dstPitch);
    for (let y = 0; y < height; y++) new Uint8Array(g.buf, (dst >>> 0) + y * dstPitch, n8).set(src.subarray(y * pitch, y * pitch + n8));
    rb.unmap(); rb.destroy();
  }

  // ---- draws ----
  function draw(at) {
    const d = readDraw(g, at), p = pipelines.get(d.pipeline);
    if (!p?.gpu) return skip('no pipeline');
    if (!target) return skip('no pass');
    if (p.color !== target.color.format || p.depth !== (target.depth?.format ?? null)) return skip('the pipeline is for other target formats');
    const items = p.bindings.map((b, i) => (b.kind === GPU.BIND_UNIFORM ? null : b.kind === GPU.BIND_SAMPLER ? samplers.get(d.bindings[i]) : res.get(d.bindings[i])));
    for (let i = 0; i < items.length; i++) {
      const kind = p.bindings[i].kind;
      if (kind === GPU.BIND_UNIFORM) continue;
      if (!items[i] || (kind !== GPU.BIND_SAMPLER && items[i].dim === 0)) return skip('a binding is missing');
      if (kind !== GPU.BIND_SAMPLER && (items[i] === target.color || items[i] === target.depth)) return skip('a draw samples its own target');
    }
    const buffers = d.vertexBuffers.map(([id]) => res.get(id)), ib = d.indexed ? res.get(d.indexBuffer) : null;
    if (buffers.length !== p.buffers.length || buffers.some((b) => !b || b.dim !== 0) || (d.indexed && (!ib || ib.dim !== 0))) return skip('a buffer is missing');
    const size = Math.max(d.uniformBytes, p.uniformSize);
    let off = Math.ceil(arena.used / align) * align;
    if (arena.gpu && off + size > arena.size) { flush(); off = 0; }
    const bg = bindGroup(p, items);
    if (!pass) open();
    g.refresh(); arena.cpu.set(g.u8.subarray(d.uniforms, d.uniforms + d.uniformBytes), off); arena.used = off + size;
    if (current !== p) { pass.setPipeline(p.gpu); current = p; }
    pass.setBindGroup(0, bg, [off]);
    for (let i = 0; i < items.length; i++) if (items[i] && p.bindings[i].kind !== GPU.BIND_SAMPLER) used.add(items[i]);
    for (let s = 0; s < buffers.length; s++) { pass.setVertexBuffer(s, buffers[s].gpu, d.vertexBuffers[s][1]); used.add(buffers[s]); }
    n.draws++;
    if (d.indexed) { pass.setIndexBuffer(ib.gpu, d.index32 ? 'uint32' : 'uint16', d.indexOffset); used.add(ib); pass.drawIndexed(d.count, d.instances, d.first, d.baseVertex, d.firstInstance); }
    else pass.draw(d.count, d.instances, d.first, d.firstInstance);
  }

  return {
    // one record; a promise for the ones that have to finish before the next
    handle(op, at) {
      switch (op) {
      case GPU.R_SHADER: return shader(at);
      case GPU.R_PIPELINE: return pipeline(at);
      case GPU.R_SAMPLER: sampler(at); break;
      case GPU.R_WRITE_BUFFER: writeBuffer(at); break;
      case GPU.R_WRITE_TEXTURE: writeTexture(at); break;
      case GPU.R_PASS: beginPass(at); break;
      case GPU.R_VIEWPORT: viewport = readViewport(g, at); applyViewport(); break;
      case GPU.R_DRAW: draw(at); break;
      case GPU.R_READ_TEXTURE: return readTexture(at);
      default: break;
      }
      return undefined;
    },
    flush,
    // a resource is being destroyed: true if what was recorded still needs it (it then goes once that is submitted)
    retire(r) {
      if (target && (target.color === r || target.depth === r)) { endPass(); target = null; }
      if (!used.has(r)) return false;
      retired.push(r.gpu); used.delete(r); return true;
    },
  };
}
