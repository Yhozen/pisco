// SPDX-License-Identifier: GPL-2.0-or-later
// The explicit records of rt/d3d12_cmd.h (GPU_R_SHADER ... GPU_R_DRAW: what a front end that is not Direct3D 12
// writes, Direct3D 9 first) read into plain objects. One reader for both back ends: explicit.js draws them with
// WebGPU, null.js checks them. `g` is the guest's memory (ring.js), `at` a record's first payload byte.
import { GPU } from './cmd.gen.js';

export const isExplicit = (op) => op >= GPU.R_SHADER && op <= GPU.R_DRAW;

export function readShader(g, at) {
  g.refresh();
  const id = g.u32[at >>> 2], bytes = g.u32[(at >>> 2) + 1];
  return { id, code: new TextDecoder().decode(g.u8.slice(at + 8, at + 8 + bytes)) };
}
export function readSampler(g, at) {
  g.refresh();
  const w = g.u32, i = at >>> 2;
  return { id: w[i], u: w[i + 1], v: w[i + 2], w: w[i + 3], mag: w[i + 4], min: w[i + 5], mip: w[i + 6], anisotropy: w[i + 7], maxLod: g.f32[i + 8] };
}
export function readPipeline(g, at) {
  g.refresh();
  const w = g.u32, i = at >>> 2;
  const p = { id: w[i], vs: w[i + 1], fs: w[i + 2], topology: w[i + 3], cull: w[i + 4], frontFace: w[i + 5],
    colorFormat: w[i + 6], writeMask: w[i + 7], blend: w[i + 8] ? { src: w[i + 9], dst: w[i + 10], op: w[i + 11], srcA: w[i + 12], dstA: w[i + 13], opA: w[i + 14] } : null,
    depthFormat: w[i + 15], depthCompare: w[i + 16], depthWrite: w[i + 17], depthBias: w[i + 18] | 0, slope: g.f32[i + 19], bindings: [], buffers: [], attributes: [] };
  let k = i + GPU.PIPELINE_WORDS;
  for (let n = w[i + 20]; n > 0; n--, k += 3) p.bindings.push({ binding: w[k], kind: w[k + 1], size: w[k + 2] });
  for (let n = w[i + 21]; n > 0; n--, k += 2) p.buffers.push({ stride: w[k], step: w[k + 1] });
  for (let n = w[i + 22]; n > 0; n--, k += 4) p.attributes.push({ location: w[k], buffer: w[k + 1], offset: w[k + 2], format: w[k + 3] });
  return p;
}
export function readPass(g, at) {
  g.refresh();
  const w = g.u32, f = g.f32, i = at >>> 2, flags = w[i + 3];
  return { color: w[i], mip: w[i + 1], depth: w[i + 2],
    clearColor: flags & GPU.PASS_CLEAR_COLOR ? { r: f[i + 4], g: f[i + 5], b: f[i + 6], a: f[i + 7] } : null,
    clearDepth: flags & GPU.PASS_CLEAR_DEPTH ? f[i + 8] : null, clearStencil: flags & GPU.PASS_CLEAR_STENCIL ? w[i + 9] : null };
}
export function readViewport(g, at) {
  g.refresh();
  const w = g.u32, f = g.f32, i = at >>> 2;
  return { x: f[i], y: f[i + 1], width: f[i + 2], height: f[i + 3], minDepth: f[i + 4], maxDepth: f[i + 5], scissor: [w[i + 6], w[i + 7], w[i + 8], w[i + 9]] };
}
// A draw: `vertexBuffers` are [id, offset] pairs, `bindings` one id for each of the pipeline's bindings (0 for the
// uniform block), `uniforms` the byte offset of the block in guest memory
export function readDraw(g, at) {
  g.refresh();
  const w = g.u32, i = at >>> 2, nvb = w[i + 9], nb = w[i + 10];
  const d = { pipeline: w[i], indexed: (w[i + 1] & GPU.DRAW_INDEXED) !== 0, index32: (w[i + 1] & GPU.DRAW_INDEX32) !== 0, count: w[i + 2], instances: w[i + 3], first: w[i + 4],
    baseVertex: w[i + 5] | 0, firstInstance: w[i + 6], indexBuffer: w[i + 7], indexOffset: w[i + 8], vertexBuffers: [], bindings: [], uniformBytes: w[i + 11] };
  let k = i + GPU.DRAW_WORDS;
  for (let n = 0; n < nvb; n++, k += 2) d.vertexBuffers.push([w[k], w[k + 1]]);
  for (let n = 0; n < nb; n++, k++) d.bindings.push(w[k]);
  d.uniforms = k * 4;
  return d;
}
// What a shader's text asks of its pipeline: its bindings in group 0 as GPU_BIND_* kinds by number, and (a vertex
// stage) the locations of its inputs. Read from the WGSL the front ends write: one declaration a line.
export function shaderInterface(code) {
  const bindings = new Map(), inputs = new Set();
  for (const m of code.matchAll(/@group\(0\) @binding\((\d+)\) var(<uniform>)? \w+: ([\w<>]+);/g)) {
    bindings.set(+m[1], m[2] ? GPU.BIND_UNIFORM : m[3].startsWith('texture_3d') ? GPU.BIND_TEXTURE_3D : m[3].startsWith('texture_cube') ? GPU.BIND_TEXTURE_CUBE : m[3].startsWith('texture_2d') ? GPU.BIND_TEXTURE_2D : m[3] === 'sampler' ? GPU.BIND_SAMPLER : 0);
  }
  const vin = code.match(/struct VIn \{([^}]*)\}/);
  if (vin) for (const m of vin[1].matchAll(/@location\((\d+)\)/g)) inputs.add(+m[1]);
  return { bindings, inputs, vertex: /@vertex\s+fn main\(/.test(code), fragment: /@fragment\s+fn main\(/.test(code) };
}
