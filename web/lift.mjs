// SPDX-License-Identifier: GPL-2.0-or-later
// Programs lifted when they first run (Node only: it needs the lifter and wasi-sdk). When a machine (rt/microvm-x64)
// meets a program no module of its holds, at the program's entry point, it rebuilds the program's ELF from memory
// (its headers and segments, as the kernel loaded them from wherever the file was) and hands it to the host. This
// lifts it into a module of its own (tools/build-side.sh, cached by the image's sha256 in out/lift-cache) and loads it
// (web/side.mjs). Seconds for a small program the first time, nothing the next.
import fs from 'node:fs'; import path from 'node:path'; import crypto from 'node:crypto'; import { execFileSync } from 'node:child_process';
import { loadSide } from './side.mjs';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');

export function programImage({ instance, memory, log }) {
  const tried = new Set();
  return (addr, len) => {
    const image = Buffer.from(new Uint8Array(memory.buffer, addr, len));
    const sha = crypto.createHash('sha256').update(image).digest('hex'), name = `p_${sha.slice(0, 16)}`;
    if (tried.has(sha)) return 0; tried.add(sha);
    const dir = path.join(root, 'out/lift-cache'), wasm = path.join(dir, `${sha}.wasm`);
    if (!fs.existsSync(wasm)) {
      fs.mkdirSync(dir, { recursive: true }); const tmp = path.join(dir, `${sha}.elf`); fs.writeFileSync(tmp, image);
      log(`lifting a program met when it first ran (${len} bytes, sha256 ${sha.slice(0, 16)})`); const t0 = Date.now();
      try { execFileSync(path.join(root, 'tools/build-side.sh'), [tmp, name, wasm], { stdio: ['ignore', 'ignore', 'inherit'] }); }
      catch { log(`could not lift it (sha256 ${sha.slice(0, 16)})`); return 0; } finally { fs.rmSync(tmp, { force: true }); }
      log(`lifted it in ${((Date.now() - t0) / 1000).toFixed(1)} s (out/lift-cache)`);
    }
    loadSide(fs.readFileSync(wasm), instance, memory, name);
    return 1;
  };
}
