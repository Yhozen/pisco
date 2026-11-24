// node tools/shader-prep.mjs <file>... [--out <dir>]: every DXIL shader container in the files (a title's exe and
// DLLs, or .cso files) becomes <dir>/<digest>.wgsl + <digest>.json, the names the GPU back end looks up (the
// container's own 16-byte digest, in hex). <dir> defaults to `shaders/` beside the first file.
// Needs target/dxil2spv (tools/dxil-wgsl/build.sh) and naga (cargo install naga-cli).
import fs from 'node:fs'; import path from 'node:path'; import os from 'node:os'; import { execFileSync } from 'node:child_process';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const argv = process.argv.slice(2); let out = null; const files = [];
for (let i = 0; i < argv.length; i++) if (argv[i] === '--out') out = argv[++i]; else files.push(argv[i]);
if (!files.length) { console.error('usage: shader-prep.mjs <file>... [--out dir]'); process.exit(2); }
out ??= path.join(path.dirname(path.resolve(files[0])), 'shaders');
fs.mkdirSync(out, { recursive: true });
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shader-prep-'));
const dxil2spv = process.env.DXIL2SPV || path.join(root, 'target/dxil2spv'), naga = process.env.NAGA || 'naga';
let found = 0, made = 0, failed = 0;
for (const file of files) {
  const b = fs.readFileSync(file);
  for (let at = b.indexOf('DXBC'); at >= 0; at = b.indexOf('DXBC', at + 4)) {
    if (at + 32 > b.length) break;
    const size = b.readUInt32LE(at + 24), parts = b.readUInt32LE(at + 28);
    if (b.readUInt32LE(at + 20) !== 1 || size < 32 || at + size > b.length || parts > 64) continue;
    const c = b.subarray(at, at + size);
    let dxil = false;
    for (let p = 0; p < parts; p++) { const o = c.readUInt32LE(32 + 4 * p); if (o + 8 <= size && ['DXIL', 'ILDB'].includes(c.toString('latin1', o, o + 4))) dxil = true; }
    if (!dxil) continue;
    found++;
    const digest = c.subarray(4, 20).toString('hex');
    if (fs.existsSync(path.join(out, `${digest}.wgsl`))) continue;
    const src = path.join(tmp, `${digest}.dxbc`), spv = path.join(tmp, `${digest}.spv`);
    fs.writeFileSync(src, c);
    try {
      execFileSync(dxil2spv, [src, spv, path.join(out, `${digest}.json`)], { stdio: ['ignore', 'ignore', 'pipe'] });
      execFileSync(naga, ['--keep-coordinate-space', spv, path.join(out, `${digest}.wgsl`)], { stdio: ['ignore', 'ignore', 'pipe'] });
      made++;
    } catch (e) { failed++; console.error(`shader-prep: ${path.basename(file)}+0x${at.toString(16)} (${digest}): ${String(e.stderr || e).trim().split('\n').slice(-3).join(' | ')}`); }
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`shader-prep: ${found} DXIL shaders, ${made} translated now, ${failed} failed -> ${out}`);
process.exit(failed ? 1 : 0);
