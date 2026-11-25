// SPDX-License-Identifier: GPL-2.0-or-later
// The null GPU back end (Node): consumes the ring, completes fences and presents at once, checks that every
// pipeline's shaders were translated (shaders/<digest>.wgsl beside the title), and can write a text trace.
// Work that only moves bytes of guest memory runs for real (buffer copies between buffers with backing, the
// console's memory operations); nothing is drawn.
// The explicit records (Direct3D 9's) are checked for what WebGPU would refuse and counted: every shader a
// pipeline names exists and has its entry point, a pipeline's bind layout and vertex layout are what its shaders
// declare, every draw gives its pipeline's bindings and buffers, in a pass whose formats are the pipeline's, and
// writes stay inside what they write to. summary() says how many of each there were and how many were wrong.
//   SW_GPU_TRACE=<file>   one line per record and command;  SW_GPU_TRACE_FRAME=<n>: only between presents n and n+1
import fs from 'node:fs'; import path from 'node:path';
import { GPU } from './cmd.gen.js';
import { Ring, words } from './ring.js';
import { memWrite, memFill, memCopy, memWait } from './memops.js';
import { isExplicit, readShader, readSampler, readPipeline, readPass, readDraw, shaderInterface } from './records.js';

const R = Object.fromEntries(Object.entries(GPU).filter(([k]) => k.startsWith('R_')).map(([k, v]) => [v, k.slice(2)]));
const L = Object.fromEntries(Object.entries(GPU).filter(([k]) => k.startsWith('L_')).map(([k, v]) => [v, k.slice(2)]));
export function digestOf(g, at) { return Buffer.from(g.bytes(at + 4, 16)).toString('hex'); }

export function startNull(memory, addr, { dir, log }) {
  const ring = new Ring(memory, addr), g = ring.g;
  const tracePath = process.env.SW_GPU_TRACE, onlyFrame = process.env.SW_GPU_TRACE_FRAME;
  const out = tracePath ? fs.openSync(tracePath, 'w') : -1;
  let presents = 0, missing = 0;
  const trace = (s) => { if (out >= 0 && (onlyFrame === undefined || +onlyFrame === presents)) fs.writeSync(out, s + '\n'); };
  const fences = new Map(), res = new Map();
  const u64 = (lo, hi) => hi * 4294967296 + lo;
  const copy = (d, doff, s, soff, n) => { if (!d?.backing || !s?.backing) return; g.refresh(); g.u8.copyWithin((d.backing >>> 0) + doff, (s.backing >>> 0) + soff, (s.backing >>> 0) + soff + n); };
  async function execute(w) {
    let replay = 0;
    for (let i = 1; i < w.length;) {
      const op = w[i] & 0xFFFF, len = w[i] >>> 16, a = w.subarray(i + 1, i + 1 + len), work = replay <= 0;
      trace(`  ${L[op] || op} ${Array.from(a.subarray(0, Math.min(len, 12))).join(' ')}${len > 12 ? ' …' : ''}`);
      replay -= 1 + len; i += 1 + len;
      if (op === GPU.L_REPLAY) { replay = a[0]; continue; }
      if (!work) continue;
      switch (op) {
      case GPU.L_COPY_BUFFER: copy(res.get(a[0]), u64(a[1], a[2]), res.get(a[3]), u64(a[4], a[5]), u64(a[6], a[7])); break;
      case GPU.L_COPY_RESOURCE: { const d = res.get(a[0]), s = res.get(a[1]); if (d?.dim === 0 && s?.dim === 0) copy(d, 0, s, 0, Math.min(d.size, s.size)); break; }
      case GPU.L_MEM_WRITE: memWrite(g, a); break;
      case GPU.L_MEM_FILL: memFill(g, a); break;
      case GPU.L_MEM_COPY: memCopy(g, a); break;
      case GPU.L_MEM_WAIT: await memWait(g, a, log); break;
      default: break;
      }
    }
  }
  const writeFence = (fence, v) => { g.refresh(); g.dv.setBigUint64(fence + GPU.FENCE_VALUE, v, true); ring.bump(); };
  // ---- the explicit records ----
  const shaders = new Map(), samplers = new Set(), pipelines = new Map(), count = { records: 0, shaders: 0, samplers: 0, pipelines: 0, passes: 0, draws: 0, indices: 0, bufferWrites: 0, textureWrites: 0, bytes: 0, errors: 0 };
  let target = null;
  const wrong = (s) => { if (count.errors++ < 10) log(`gpu: ${s}`); };
  const VERTEX_BYTES = { 2: 16, 6: 12, 16: 8, 41: 4, 28: 4, 30: 4 };   // DXGI formats of vertex attributes: their sizes
  function explicit(op, at, w) {
    count.records++;
    switch (op) {
    case GPU.R_SHADER: {
      const s = readShader(g, at), face = shaderInterface(s.code); count.shaders++;
      if (shaders.has(s.id)) wrong(`shader ${s.id} is made twice`);
      if (!s.code.trim() || (!face.vertex && !face.fragment)) wrong(`shader ${s.id} is empty or has no entry point "main"`);
      shaders.set(s.id, face); trace(`shader ${s.id} (${s.code.length} bytes, ${face.vertex ? 'vertex' : 'fragment'})`); break;
    }
    case GPU.R_SAMPLER: { const s = readSampler(g, at); samplers.add(s.id); count.samplers++; trace(`sampler ${s.id} address ${s.u} ${s.v} ${s.w} filters ${s.mag} ${s.min} ${s.mip} anisotropy ${s.anisotropy}`); break; }
    case GPU.R_PIPELINE: {
      const p = readPipeline(g, at), vs = shaders.get(p.vs), fs = shaders.get(p.fs); count.pipelines++; pipelines.set(p.id, p);
      trace(`pipeline ${p.id} vs ${p.vs} fs ${p.fs} topology ${p.topology} cull ${p.cull} colour ${p.colorFormat} depth ${p.depthFormat} bindings ${p.bindings.map((b) => `${b.binding}:${b.kind}`).join(' ')} attributes ${p.attributes.map((a) => `${a.location}@${a.buffer}+${a.offset}`).join(' ')}`);
      if (!vs?.vertex || !fs?.fragment) { wrong(`pipeline ${p.id}: shader ${!vs?.vertex ? p.vs : p.fs} does not exist or is of the other stage`); break; }
      // the bind layout is what the two shaders declare, kind for kind
      const layout = new Map(p.bindings.map((b) => [b.binding, b.kind]));
      for (const face of [vs, fs]) for (const [binding, kind] of face.bindings) if (layout.get(binding) !== kind) wrong(`pipeline ${p.id}: a shader declares binding ${binding} of kind ${kind}, the layout has ${layout.get(binding) ?? 'none'}`);
      for (const b of p.bindings) if (b.kind !== GPU.BIND_UNIFORM && !vs.bindings.has(b.binding) && !fs.bindings.has(b.binding)) wrong(`pipeline ${p.id}: binding ${b.binding} of the layout is in neither shader`);
      // the vertex layout gives every input of the vertex shader, inside its buffer's stride
      const given = new Set(p.attributes.map((a) => a.location));
      for (const loc of vs.inputs) if (!given.has(loc)) wrong(`pipeline ${p.id}: the vertex shader reads location ${loc}, which no attribute gives`);
      for (const a of p.attributes) {
        if (!vs.inputs.has(a.location)) wrong(`pipeline ${p.id}: attribute at location ${a.location} is not an input of the vertex shader`);
        const b = p.buffers[a.buffer], size = VERTEX_BYTES[a.format];
        if (!b || !size || (b.stride && a.offset + size > b.stride)) wrong(`pipeline ${p.id}: attribute at location ${a.location} (format ${a.format}, offset ${a.offset}) does not fit its buffer`);
      }
      if (p.topology < 1 || p.topology > 5 || (!p.colorFormat && !p.depthFormat)) wrong(`pipeline ${p.id}: topology ${p.topology}, no target`);
      break;
    }
    case GPU.R_WRITE_BUFFER: {
      const r = res.get(w[0]); count.bufferWrites++; count.bytes += w[2]; trace(`write buffer ${w[0]} +${w[1]} ${w[2]} bytes mode ${w[3]}`);
      if (!r || r.dim !== 0) wrong(`a write to buffer ${w[0]}, which does not exist`);
      else if (w[1] % 4 || w[2] % 4 || w[1] + w[2] > Math.max(16, Math.ceil(r.size / 4) * 4)) wrong(`a write of ${w[2]} bytes at ${w[1]} to buffer ${w[0]} of ${r.size}`);
      break;
    }
    case GPU.R_WRITE_TEXTURE: {
      const r = res.get(w[0]), mip = w[1]; count.textureWrites++; count.bytes += w[8] * w[6] * w[7]; trace(`write texture ${w[0]} level ${mip} ${w[2]},${w[3]},${w[4]} ${w[5]}x${w[6]}x${w[7]}`);
      if (!r || r.dim === 0) { wrong(`a write to texture ${w[0]}, which does not exist`); break; }
      const side = (v) => Math.max(1, v >> mip);
      if (mip >= r.mips || w[2] + w[5] > side(r.width) || w[3] + w[6] > side(r.height) || w[4] + w[7] > (r.dim === 3 ? side(r.depth) : r.depth || 1) || w[8] < 4 * w[5]) wrong(`a write to texture ${w[0]} level ${mip} outside it`);
      break;
    }
    case GPU.R_READ_TEXTURE: {   // no pixels here: the guest's bytes stay as they are
      const r = res.get(w[0]), mip = w[1], side = (v) => Math.max(1, v >> mip); count.readbacks = (count.readbacks || 0) + 1; trace(`read texture ${w[0]} level ${mip} layer ${w[2]} ${w[3]}x${w[4]}`);
      if (!r || r.dim !== 2) { wrong(`a read of texture ${w[0]}, which is no 2D texture`); break; }
      if (mip >= r.mips || w[2] >= (r.depth || 1) || w[3] !== side(r.width) || w[4] !== side(r.height) || w[5] < w[3]) wrong(`a read of texture ${w[0]} that is not one whole level of it`);
      break;
    }
    case GPU.R_PASS: {
      const p = readPass(g, at), c = res.get(p.color), d = p.depth ? res.get(p.depth) : null; count.passes++; target = null;
      trace(`pass colour ${p.color} level ${p.mip} depth ${p.depth}${p.clearColor ? ' clear colour' : ''}${p.clearDepth != null ? ' clear depth' : ''}`);
      if (!c || c.dim === 0 || !(c.flags & 1)) { wrong(`a pass on ${p.color}, which is no render target`); break; }
      if (p.depth && (!d || !(d.flags & 2) || d.width !== c.width || d.height !== c.height)) { wrong(`a pass with depth buffer ${p.depth}, which is none or of another size`); break; }
      target = { color: c, depth: d, colorId: p.color }; break;
    }
    case GPU.R_VIEWPORT: trace(`viewport ${Array.from(g.f32.subarray(at >>> 2, (at >>> 2) + 6)).join(' ')}`); break;
    case GPU.R_DRAW: {
      const d = readDraw(g, at), p = pipelines.get(d.pipeline); count.draws++; count.indices += d.count * d.instances;
      trace(`draw pipeline ${d.pipeline} ${d.indexed ? 'indexed ' : ''}${d.count} x${d.instances} first ${d.first} base ${d.baseVertex} buffers ${d.vertexBuffers.map((b) => b.join('+')).join(' ')} bindings ${d.bindings.join(' ')} uniforms ${d.uniformBytes}`);
      if (!p) { wrong(`a draw with pipeline ${d.pipeline}, which does not exist`); break; }
      if (!target) { wrong('a draw outside a pass'); break; }
      if (p.colorFormat !== target.color.format || p.depthFormat !== (target.depth?.format ?? 0)) wrong(`a draw with pipeline ${d.pipeline} (formats ${p.colorFormat}, ${p.depthFormat}) in a pass of formats ${target.color.format}, ${target.depth?.format ?? 0}`);
      if (d.bindings.length !== p.bindings.length) { wrong(`a draw gives ${d.bindings.length} bindings, pipeline ${d.pipeline} has ${p.bindings.length}`); break; }
      p.bindings.forEach((b, k) => {
        const id = d.bindings[k], r = res.get(id);
        if (b.kind === GPU.BIND_UNIFORM) { if (d.uniformBytes < b.size) wrong(`a draw brings ${d.uniformBytes} bytes of uniforms, pipeline ${d.pipeline} needs ${b.size}`); }
        else if (b.kind === GPU.BIND_SAMPLER) { if (!samplers.has(id)) wrong(`a draw binds sampler ${id}, which does not exist`); }
        else if (!r || r.dim !== (b.kind === GPU.BIND_TEXTURE_3D ? 3 : 2) || (b.kind === GPU.BIND_TEXTURE_CUBE && r.depth !== 6)) wrong(`a draw binds texture ${id} to a binding of kind ${b.kind}`);
        else if (id === target.colorId) wrong(`a draw samples texture ${id}, the pass's own target`);
      });
      if (d.vertexBuffers.length !== p.buffers.length || d.vertexBuffers.some(([id]) => res.get(id)?.dim !== 0)) wrong(`a draw gives ${d.vertexBuffers.length} vertex buffers, pipeline ${d.pipeline} reads ${p.buffers.length}`);
      if (d.indexed && res.get(d.indexBuffer)?.dim !== 0) wrong(`a draw with index buffer ${d.indexBuffer}, which does not exist`);
      break;
    }
    default: break;
    }
  }
  ring.run(async (op, at, n) => {
    const w = words(g, at, isExplicit(op) ? Math.min(n >> 2, 16) : n >> 2);   // an explicit record's first words: its bytes (a texture's) are not copied
    switch (op) {
    case GPU.R_RESOURCE: res.set(w[0], { dim: w[1], format: w[2], size: u64(w[3], w[4]), width: w[3], height: w[5], depth: w[6], mips: w[7], flags: w[9], backing: w[11] }); trace(`resource ${w[0]} dim ${w[1]} format ${w[2]} ${w[3]}x${w[5]}x${w[6]} mips ${w[7]} flags ${w[9]} heap ${w[10]}`); break;
    case GPU.R_DESTROY: res.delete(w[0]); trace(`destroy ${w[0]}`); break;
    case GPU.R_ROOTSIG: trace(`root signature ${w[0]} (${w[1]} bytes)`); break;
    case GPU.R_PSO: {
      const kind = w[2], base = at + 16, shaders = [];
      const stages = kind === 1 ? [[8, 'cs']] : [[8, 'vs'], [24, 'ps'], [40, 'ds'], [56, 'hs'], [72, 'gs']];
      for (const [o, name] of stages) {
        const off = Number(g.u64(base + o)), size = Number(g.u64(base + o + 8));
        if (!off || !size) continue;
        const d = digestOf(g, at + off); shaders.push(`${name} ${d}`);
        if (dir && !fs.existsSync(path.join(dir, 'shaders', `${d}.wgsl`))) { missing++; log(`gpu: ${name} shader ${d} of pipeline ${w[0]} has no translation (run tools/shader-prep.mjs)`); }
      }
      trace(`pipeline ${w[0]} ${kind ? 'compute' : 'graphics'} root ${w[1]}: ${shaders.join(', ')}`); break;
    }
    case GPU.R_EXECUTE: {
      trace(`execute queue ${w[0]}`);
      await execute(w);
      break;
    }
    case GPU.R_SIGNAL: { const v = BigInt(w[1]) | BigInt(w[2]) << 32n; trace(`signal fence ${w[0].toString(16)} ${v}`); writeFence(w[0], v); fences.set(w[0], v); break; }
    case GPU.R_WAIT: { const v = BigInt(w[1]) | BigInt(w[2]) << 32n; trace(`wait fence ${w[0].toString(16)} ${v}`); while (g.u64(w[0] + GPU.FENCE_VALUE) < v) await ring.waitEpoch(); break; }
    case GPU.R_PRESENT: trace(`present ${w[0]} token ${w[1]}`); presents++; ring.set(GPU.H_PRESENTS, presents); ring.bump(); break;
    default: if (isExplicit(op)) explicit(op, at, w); else trace(`record ${R[op] || op} (${n} bytes)`);
    }
  });
  return { get presents() { return presents; }, get missing() { return missing; }, get errors() { return count.errors; },
    // one line about the explicit records, or '' if there were none
    summary: () => (count.records ? `gpu (null back end): ${count.shaders} shaders, ${count.pipelines} pipelines, ${count.samplers} samplers, ${count.passes} passes, ${count.draws} draws (${count.indices} vertices), `
      + `${count.bufferWrites} buffer and ${count.textureWrites} texture writes (${count.bytes} bytes), ${presents} presents; ${count.errors} of the records were inconsistent` : '') };
}
