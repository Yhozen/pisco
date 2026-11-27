// SPDX-License-Identifier: GPL-2.0-or-later
// Programs lifted when they first run (Node only: it needs the lifter and wasi-sdk). When a machine (rt/microvm-x64)
// meets user code that no program it has holds, it gives the host the page there; this finds the program of the
// initramfs whose code page at that address is the same, lifts it into a module of its own (tools/build-side.sh,
// cached by the file's sha256 in out/lift-cache) and loads it (web/side.mjs). Minutes for a large program the first
// time, nothing the next.
import fs from 'node:fs'; import path from 'node:path'; import crypto from 'node:crypto'; import { execFileSync } from 'node:child_process';
import { loadSide } from './side.mjs';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');

// the regular files of a newc cpio that are ELFs, with their executable segments
function elfsOf(cpio) {
  const out = []; let at = 0; const hex = (o, n) => parseInt(cpio.toString('latin1', o, o + n), 16);
  while (at + 110 <= cpio.length && cpio.toString('latin1', at, at + 6) === '070701') {
    const mode = hex(at + 14, 8), size = hex(at + 54, 8), nlen = hex(at + 94, 8);
    const name = cpio.toString('utf8', at + 110, at + 110 + nlen - 1);
    const data = (at + 110 + nlen + 3) & ~3, next = (data + size + 3) & ~3;
    if (name === 'TRAILER!!!') break;
    if ((mode & 0o170000) === 0o100000 && size > 64 && cpio.readUInt32BE(data) === 0x7f454c46) {
      const f = cpio.subarray(data, data + size), segs = [];
      const phoff = Number(f.readBigUInt64LE(32)), n = f.readUInt16LE(56);
      for (let i = 0; i < n; i++) {
        const p = phoff + 56 * i; if (f.readUInt32LE(p) !== 1 || !(f.readUInt32LE(p + 4) & 1)) continue;
        segs.push({ off: Number(f.readBigUInt64LE(p + 8)), va: f.readBigUInt64LE(p + 16), filesz: Number(f.readBigUInt64LE(p + 32)), memsz: f.readBigUInt64LE(p + 40) });
      }
      if (segs.length) out.push({ name, file: f, segs });
    }
    at = next;
  }
  return out;
}
// the bytes of the program's code at [a, b) as its file has them (0 past the file's bytes)
function codeBytes(e, a, b) {
  const out = Buffer.alloc(Number(b - a));
  for (const s of e.segs) {
    const lo = a > s.va ? a : s.va, hi = b < s.va + s.memsz ? b : s.va + s.memsz;
    for (let x = lo; x < hi; x++) { const k = Number(x - s.va); if (k < s.filesz) out[Number(x - a)] = e.file[s.off + k]; }
  }
  return out;
}
export function programMissing({ readInitrd, instance, memory, log }) {
  let elfs = null; const tried = new Set();
  return (pc, page) => {
    if (!elfs) { const b = readInitrd(); elfs = b ? elfsOf(b) : []; }
    const guest = Buffer.from(new Uint8Array(memory.buffer, page, 4096)), pg = pc & ~0xFFFn;
    for (const e of elfs) {
      const lo = e.segs.reduce((m, s) => (s.va < m ? s.va : m), e.segs[0].va), hi = e.segs.reduce((m, s) => (s.va + s.memsz > m ? s.va + s.memsz : m), 0n);
      if (pc < lo || pc >= hi) continue;
      const a = pg > lo ? pg : lo, b = pg + 4096n < hi ? pg + 4096n : hi;
      if (!codeBytes(e, a, b).equals(guest.subarray(Number(a - pg), Number(b - pg)))) continue;
      const sha = crypto.createHash('sha256').update(e.file).digest('hex'), name = `p_${sha.slice(0, 16)}`;
      if (tried.has(sha)) return 0; tried.add(sha);
      const dir = path.join(root, 'out/lift-cache'), wasm = path.join(dir, `${sha}.wasm`);
      if (!fs.existsSync(wasm)) {
        fs.mkdirSync(dir, { recursive: true }); const tmp = path.join(dir, `${sha}.elf`); fs.writeFileSync(tmp, e.file);
        log(`lifting ${e.name} (${e.file.length} bytes), met when it first ran`); const t0 = Date.now();
        try { execFileSync(path.join(root, 'tools/build-side.sh'), [tmp, name, wasm], { stdio: ['ignore', 'ignore', 'inherit'] }); }
        catch { log(`could not lift ${e.name}`); return 0; } finally { fs.rmSync(tmp, { force: true }); }
        log(`lifted ${e.name} in ${((Date.now() - t0) / 1000).toFixed(1)} s (out/lift-cache)`);
      }
      loadSide(fs.readFileSync(wasm), instance, memory, name);
      return 1;
    }
    return 0;
  };
}
