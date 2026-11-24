// Reads the front end's ring (rt/d3d12_cmd.h) in order, on a thread that must not block (the page's main
// thread, Node's supervisor): it waits with Atomics.waitAsync and hands each record to `handle`, which may be
// async; a record is consumed (its room given back) once `handle` resolves.
import { GPU } from './cmd.gen.js';

export class Guest {
  constructor(memory) { this.memory = memory; this.refresh(); }
  refresh() {
    if (this.buf === this.memory.buffer) return;
    this.buf = this.memory.buffer;
    this.u8 = new Uint8Array(this.buf); this.u32 = new Uint32Array(this.buf); this.i32 = new Int32Array(this.buf);
    this.f32 = new Float32Array(this.buf); this.dv = new DataView(this.buf);
  }
  u64(a) { this.refresh(); return this.dv.getBigUint64(a, true); }
  // guest addresses above 4 GiB are not in linear memory (wasm32): only the low 32 bits name a byte
  bytes(a, n) { this.refresh(); return this.u8.subarray(a, a + n); }
  copy(a, n) { return this.bytes(a, n).slice(); }
  str(a) { this.refresh(); let e = a; while (this.u8[e]) e++; return new TextDecoder().decode(this.u8.slice(a, e)); }
}

export class Ring {
  constructor(memory, addr) {
    this.g = new Guest(memory); this.addr = addr; this.h = addr >>> 2;
    this.data = addr + 4 * GPU.H_WORDS; this.size = GPU.RING_BYTES;
  }
  word(i) { this.g.refresh(); return Atomics.load(this.g.i32, this.h + i) >>> 0; }
  set(i, v) { this.g.refresh(); Atomics.store(this.g.i32, this.h + i, v | 0); Atomics.notify(this.g.i32, this.h + i); }
  // the epoch wakes every thread that waits on a fence or a present
  bump() { this.g.refresh(); Atomics.add(this.g.i32, this.h + GPU.H_EPOCH, 1); Atomics.notify(this.g.i32, this.h + GPU.H_EPOCH); }
  async waitEpoch(ms = 50) { const e = Atomics.load(this.g.i32, this.h + GPU.H_EPOCH); const r = Atomics.waitAsync(this.g.i32, this.h + GPU.H_EPOCH, e, ms); if (r.async) await r.value; }
  async run(handle) {
    for (;;) {
      const head = this.word(GPU.H_HEAD); let tail = this.word(GPU.H_TAIL);
      if (head === tail) { const r = Atomics.waitAsync(this.g.i32, this.h + GPU.H_HEAD, head | 0, 1000); if (r.async) await r.value; continue; }
      const pos = tail % this.size, at = this.data + pos;
      this.g.refresh();
      const op = this.g.u32[at >>> 2], bytes = this.g.u32[(at >>> 2) + 1];
      if (op !== GPU.R_WRAP) await handle(op, at + 8, bytes - 8);
      tail = (tail + bytes) >>> 0;
      this.set(GPU.H_TAIL, tail);
    }
  }
}

// a desc of guest bytes (copied: the ring's room is reused)
export function words(g, at, n) { g.refresh(); return g.u32.slice(at >>> 2, (at >>> 2) + n); }
