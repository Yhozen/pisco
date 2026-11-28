// SPDX-License-Identifier: GPL-2.0-or-later
// run-node.mjs: unkernel's kernel in Node, patched (build.sh), with PISCO's runtime for x86-64 tasks (runtime.wasm)
// in the kernel's memory above its RAM. A program met for the first time is lifted (lift-side.sh) into --cache
// (default out/tabc-pisco/lifted, by the sha256 of its image) and loaded; tabc's supervisor does the same for the page.
//   node run-node.mjs [--root dir] [--no-pisco] [--dir d] [--mem MiB] [--stats] [--trace n] -- cmd...
// x86load is registered with binfmt_misc first. --root: an amd64 root file system (an image's layers unpacked),
// each command run in it (chroot, with /proc, /tmp and /dev). Otherwise each command after -- is run by the base
// system's `sh -c` (vm.exec), as root, with the directory d at /mnt/x; its output is printed.
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const here = path.dirname(new URL(import.meta.url).pathname), out = path.resolve(here, '../../out/tabc-pisco');
const opt = { cache: path.join(out, 'lifted'), kernel: path.join(out, 'kernel.wasm'), pisco: true, sides: [], dir: '.', mem: 256, jit: true, rt: path.join(out, 'runtime.wasm'), unk: path.join(out, 'unkernel/package/dist/node.mjs'), trace: 0, stats: false };
const cmds = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--') { cmds.push(...args.slice(i + 1)); break; }
  if (a === '--kernel') opt.kernel = args[++i];
  else if (a === '--no-pisco') opt.pisco = false;
  else if (a === '--side') opt.sides.push(args[++i]);
  else if (a === '--dir') opt.dir = args[++i];
  else if (a === '--mem') opt.mem = +args[++i];
  else if (a === '--jit') opt.jit = args[++i] !== '0';
  else if (a === '--rt') opt.rt = args[++i];
  else if (a === '--unk') opt.unk = args[++i];
  else if (a === '--trace') opt.trace = +args[++i];
  else if (a === '--stats') opt.stats = true;
  else if (a === '--cache') opt.cache = args[++i];
  else if (a === '--root') opt.root = args[++i];
  else throw new Error('unknown option ' + a);
}
const t0 = performance.now();
const ms = () => (performance.now() - t0).toFixed(0);

// ---- the hooks: the kernel's memory may grow past its RAM (our part), and the kernel gets its `pisco` imports ----
const TOP = 0x90000000;   // the memory may grow to 2.25 GiB: the kernel's RAM, then the runtime's (from 0x80000000)
const OrigMemory = WebAssembly.Memory;
function Memory(desc) {
  if (!desc.shared && !desc.address && desc.maximum === desc.initial && desc.initial >= 1024) desc = { ...desc, maximum: TOP / 65536 };
  return new OrigMemory(desc);
}
Memory.prototype = OrigMemory.prototype;
WebAssembly.Memory = Memory;
const tramp = new WebAssembly.Module(fs.readFileSync(path.join(out, 'tramp.wasm')));
const ttab = new WebAssembly.Table({ element: 'anyfunc', initial: 4 });
const T = new WebAssembly.Instance(tramp, { js: { tab: ttab } });
let kmem = null, kex = null;
const origInstantiate = WebAssembly.instantiate;
WebAssembly.instantiate = async function (m, imports, ...rest) {
  if (imports && imports.unkernel && imports.env && imports.env.memory) {
    const mod = m instanceof WebAssembly.Module ? m : null;
    if (mod && WebAssembly.Module.imports(mod).some((i) => i.module === 'pisco')) imports = { ...imports, pisco: T.exports };
    kmem = imports.env.memory;
    const r = await origInstantiate.call(this, m, imports, ...rest);
    kex = (r.instance ?? r).exports;
    return r;
  }
  return origInstantiate.call(this, m, imports, ...rest);
};

// ---- PISCO's runtime and the lifted programs, once the kernel has booted ----
const dec = new TextDecoder();
const str = (p, n) => dec.decode(new Uint8Array(kmem.buffer, p >>> 0, n >>> 0));   // (the runtime's pointers are above 2 GiB: unsigned)
function loadSide(bytes, rt, name) {   // web/side.mjs, for a memory that is not shared
  const module = new WebAssembly.Module(bytes), ex = rt.exports;
  const sec = new Uint8Array(WebAssembly.Module.customSections(module, 'dylink.0')[0]);
  const at = { i: 0 }; const leb = () => { let v = 0, s = 0, x; do { x = sec[at.i++]; v |= (x & 0x7f) << s; s += 7; } while (x & 0x80); return v >>> 0; };
  let memSize = 0, memAlign = 0, tableSize = 0;
  while (at.i < sec.length) { const kind = sec[at.i++], len = leb(), end = at.i + len; if (kind === 1) { memSize = leb(); memAlign = leb(); tableSize = leb(); } at.i = end; }
  const memBase = memSize ? ex.rt_side_alloc(memSize, 2 ** memAlign) >>> 0 : 0;
  const table = ex.__indirect_function_table, tableBase = table.length;
  if (tableSize) table.grow(tableSize);
  const env = { memory: kmem, __indirect_function_table: table, __stack_pointer: ex.__stack_pointer,
    __memory_base: new WebAssembly.Global({ value: 'i32', mutable: false }, memBase), __table_base: new WebAssembly.Global({ value: 'i32', mutable: false }, tableBase) };
  const got = {}, gotFunc = {};
  for (const i of WebAssembly.Module.imports(module)) {
    if (i.module === 'env' && i.kind === 'function') { if (!ex[i.name]) throw new Error(`${name}: the runtime exports no ${i.name}`); env[i.name] = ex[i.name]; }
    else if (i.module === 'GOT.mem') got[i.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, ex[i.name].value);
    else if (i.module === 'GOT.func') { const at = table.length; table.grow(1); table.set(at, ex[i.name]); gotFunc[i.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, at); }
  }
  const side = new WebAssembly.Instance(module, { env, 'GOT.mem': got, 'GOT.func': gotFunc });
  side.exports.__wasm_apply_data_relocs?.(); side.exports.__wasm_call_ctors?.();
  ex.rt_program_register((memBase + (side.exports[`${name}_program`].value >>> 0)) | 0);
}
let rt = null;
import crypto from 'node:crypto'; import { execFileSync } from 'node:child_process';
const tried = new Set();
function programImage(addr, len, fnsp, nfns) {
  addr >>>= 0; len >>>= 0; fnsp >>>= 0;
  const image = Buffer.from(new Uint8Array(kmem.buffer, addr, len));
  const entries = [...new BigUint64Array(kmem.buffer.slice(fnsp, fnsp + 8 * nfns))].map((v) => v.toString(16));
  const sha = crypto.createHash('sha256').update(image).digest('hex'), name = `p_${sha.slice(0, 16)}`;
  if (tried.has(sha)) return 0; tried.add(sha);
  fs.mkdirSync(opt.cache, { recursive: true });
  const wasm = path.join(opt.cache, `${sha}.wasm`);
  if (!fs.existsSync(wasm)) {
    const tmp = path.join(opt.cache, `${sha}.elf`); fs.writeFileSync(tmp, image);
    console.error(`[run] lifting a program met when it first ran (${len} bytes, sha256 ${sha.slice(0, 16)})`); const t = Date.now();
    try { execFileSync(path.join(here, 'lift-side.sh'), [tmp, name, wasm, ...entries], { stdio: ['ignore', 'ignore', 'inherit'] }); }
    catch { console.error(`[run] could not lift it (${tmp} kept)`); return 0; }
    fs.writeFileSync(path.join(opt.cache, `${sha}.fns`), entries.join(' ') + '\n');
    fs.rmSync(tmp, { force: true });
    console.error(`[run] lifted it in ${((Date.now() - t) / 1000).toFixed(1)} s`);
  }
  loadSide(fs.readFileSync(wasm), rt, name);
  return 1;
}
function initPisco() {
  const need = 0x81000000;
  if (kmem.buffer.byteLength < need) kmem.grow((need - kmem.buffer.byteLength) / 65536);
  const nowNs = () => BigInt(Math.round(performance.now() * 1e6));
  rt = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(opt.rt)), {
    env: { memory: kmem }, kernel: { kw_user_addr: kex.kw_user_addr },
    host: { log: (p, n) => console.error(str(p, n)), now_ns: nowNs, program_image: programImage },
    wasi_snapshot_preview1: {
      clock_time_get: (id, prec, out) => { new DataView(kmem.buffer).setBigUint64(out >>> 0, nowNs(), true); return 0; },
      fd_close: () => 0, fd_seek: () => 70,
      fd_write: (fd, iov, n, out) => { const dv = new DataView(kmem.buffer); let t = 0; iov >>>= 0; out >>>= 0; for (let k = 0; k < n; k++) { const p = dv.getUint32(iov + 8 * k, true), l = dv.getUint32(iov + 8 * k + 4, true); process.stderr.write(str(p, l)); t += l; } dv.setUint32(out, t, true); return 0; },
    },
  });
  rt.exports._initialize();
  if (opt.trace) rt.exports.pisco_trace(opt.trace);
  for (const s of opt.sides) { const [name, file] = s.split('='); loadSide(fs.readFileSync(file), rt, name); }
  ttab.set(0, rt.exports.pisco_mine); ttab.set(1, rt.exports.pisco_run); ttab.set(2, rt.exports.pisco_after); ttab.set(3, rt.exports.pisco_flush);
}

const { createKernel, nodeFs, MemoryFs } = await import(path.resolve(opt.unk));
// x86load where binfmt_misc finds it (registered below, as tabc's init does), and an amd64 root if one is given
const pbin = new MemoryFs(); pbin.writeFile('/x86load', new Uint8Array(fs.readFileSync(path.join(out, 'x86load'))), { mode: 0o755 });
const mounts = { '/mnt/x': nodeFs(path.resolve(opt.dir)), '/pisco': { fs: pbin, readOnly: true } };
if (opt.root) mounts['/mnt/root'] = nodeFs(path.resolve(opt.root));
const kernel = opt.kernel ? new WebAssembly.Module(fs.readFileSync(opt.kernel)) : undefined;
const vm = await createKernel({ kernel, memoryMiB: opt.mem, jit: opt.jit, mounts });
console.error(`[run] booted in ${ms()} ms (memory ${kmem.buffer.byteLength >> 20} MiB)`);
if (opt.pisco) { initPisco(); console.error(`[run] pisco loaded at ${ms()} ms`); }
if (opt.pisco) {
  const reg = 'mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null; echo 0 > /proc/sys/kernel/randomize_va_space; echo 1 > /proc/sys/vm/legacy_va_layout; ' +
    "printf '%s' ':x86_64:M::\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x3e\\x00:" +
    "\\xff\\xff\\xff\\xff\\xff\\xfe\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff:/pisco/x86load:PF' > /proc/sys/fs/binfmt_misc/register" +
    (opt.root ? '; for d in proc tmp dev; do mkdir -p /mnt/root/$d; done; mount -t proc proc /mnt/root/proc; mount -t tmpfs tmp /mnt/root/tmp; mount -o bind /dev /mnt/root/dev' : '');
  const r = await vm.exec('sh', ['-c', reg], { user: 'root' });
  if (r.exitCode) console.error(`[run] registering x86load: ${r.stderr}`);
}
for (const c0 of cmds) {
  const c = opt.root ? `chroot /mnt/root /bin/sh -c ${JSON.stringify(c0).replace(/\$/g, '\\$')}` : c0;
  const s = performance.now();
  const r = await vm.exec('sh', ['-c', c], { user: 'root' });
  process.stdout.write(r.stdout); if (r.stderr) process.stderr.write(r.stderr);
  console.error(`[run] ${JSON.stringify(c)}: exit ${r.exitCode}${r.signal ? ' signal ' + r.signal : ''}, ${(performance.now() - s).toFixed(0)} ms`);
}
if (rt && opt.stats) rt.exports.pisco_stats();
await vm.stop?.();
process.exit(0);
