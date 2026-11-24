// The null GPU back end (Node): consumes the ring, completes fences and presents at once, checks that every
// pipeline's shaders were translated (shaders/<digest>.wgsl beside the title), and can write a text trace.
// Work that only moves bytes of guest memory runs for real (buffer copies between buffers with backing, the
// console's memory operations); nothing is drawn.
//   SW_GPU_TRACE=<file>   one line per record and command;  SW_GPU_TRACE_FRAME=<n>: only between presents n and n+1
import fs from 'node:fs'; import path from 'node:path';
import { GPU } from './cmd.gen.js';
import { Ring, words } from './ring.js';
import { memWrite, memFill, memCopy, memWait } from './memops.js';

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
  ring.run(async (op, at, n) => {
    const w = words(g, at, n >> 2);
    switch (op) {
    case GPU.R_RESOURCE: res.set(w[0], { dim: w[1], size: u64(w[3], w[4]), backing: w[11] }); trace(`resource ${w[0]} dim ${w[1]} format ${w[2]} ${w[3]}x${w[5]}x${w[6]} mips ${w[7]} flags ${w[9]} heap ${w[10]}`); break;
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
    default: trace(`record ${R[op] || op} (${n} bytes)`);
    }
  });
  return { get presents() { return presents; }, get missing() { return missing; } };
}
