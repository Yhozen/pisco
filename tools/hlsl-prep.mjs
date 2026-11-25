// SPDX-License-Identifier: GPL-2.0-or-later
// node tools/hlsl-prep.mjs <run log>... --title <title folder>: the HLSL a title compiles at run time (D3DCompile),
// compiled ahead of time. A run logs every source it found no shader for ("d3dcompile-source <key> <target>
// <entry> <base64>", rt/gdk/d3d12.c); this compiles each with dxc to DXIL (shader model 6.0 for 4.x/5.x targets)
// into <title folder>/shaders/hlsl/<key>.cso, the file D3DCompile looks up, then translates them for the GPU
// back end (tools/shader-prep.mjs). Environment: DXC (default ~/winsdk/dxc/dxc-docker, which needs paths
// under $HOME).
import fs from 'node:fs'; import path from 'node:path'; import { execFileSync } from 'node:child_process';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const argv = process.argv.slice(2); let title = null; const logs = [];
for (let i = 0; i < argv.length; i++) if (argv[i] === '--title') title = argv[++i]; else logs.push(argv[i]);
if (!title || !logs.length) { console.error('usage: hlsl-prep.mjs <run log>... --title <title folder>'); process.exit(2); }
const dxc = process.env.DXC || path.join(process.env.HOME, 'winsdk/dxc/dxc-docker');
const out = path.resolve(title, 'shaders/hlsl'); fs.mkdirSync(out, { recursive: true });
const tmp = fs.mkdtempSync(path.join(root, 'out', 'hlsl-prep-'));
const seen = new Map();
for (const log of logs) for (const m of fs.readFileSync(log, 'utf8').matchAll(/d3dcompile-source ([0-9a-f]{16}) (\S+) (\S+) ([A-Za-z0-9+/=]+)/g)) seen.set(m[1], { target: m[2], entry: m[3], source: Buffer.from(m[4], 'base64') });
let made = 0, failed = 0;
for (const [key, s] of seen) {
  const dst = path.join(out, `${key}.cso`);
  if (fs.existsSync(dst)) continue;
  const [stage, major] = s.target.split('_'), target = +major >= 6 ? s.target : `${stage}_6_0`;
  const src = path.join(tmp, `${key}.hlsl`); fs.writeFileSync(src, s.source);
  try { execFileSync(dxc, ['-T', target, '-E', s.entry, src, '-Fo', dst], { stdio: ['ignore', 'ignore', 'pipe'] }); made++; }
  catch (e) { failed++; console.error(`hlsl-prep: ${key} (${s.target} ${s.entry}): ${String(e.stderr || e).trim().split('\n').slice(0, 3).join(' | ')}`); }
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`hlsl-prep: ${seen.size} sources, ${made} compiled now, ${failed} failed -> ${out}`);
const csos = fs.readdirSync(out).filter((f) => f.endsWith('.cso')).map((f) => path.join(out, f));
if (csos.length) execFileSync(process.execPath, [path.join(root, 'tools/shader-prep.mjs'), ...csos, '--out', path.resolve(title, 'shaders')], { stdio: 'inherit' });
process.exit(failed ? 1 : 0);
