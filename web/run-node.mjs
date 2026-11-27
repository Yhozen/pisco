// SPDX-License-Identifier: GPL-2.0-or-later
// Runs a lifted title under Node: node run-node.mjs title.wasm <the title's .exe> [title arguments...]
//                              or node run-node.mjs <image> [title arguments...] (a title image: web/image.mjs, docs/images.md;
//                              its file layers are the title's C: drive, its config says which exe, its registry, ...)
// Environment: SW_* variables reach the runtime (SW_TRACE, SW_STRICT, SW_CLOCK, ...); SW_SAVES, SW_SESSION and SW_AUDIO_WAV
// are the host's too. SW_DUMP_RAW=<file> writes each presented frame as packed RGBA (ffmpeg: -f rawvideo -pix_fmt rgba);
// SW_KEYS=<json> is a scripted keyboard ([[ms, code, keyCode, down], ...], times on the virtual clock, as tools/run-chrome.mjs);
// SW_MAX_FRAMES is the runtime's (videoout). SW_ROOT=<folder> (a Linux title): the folder is the root of its file system and every program
// in it has its module beside it (bin/bash -> bin/bash.wasm), for execve; with SW_EXE=<path of the first program>.
// The main thread only supervises: the title's threads (its main thread too) run in workers, so an exit
// from any of them ends the process even while others are blocked.
import fs from 'node:fs';
import path from 'node:path';
import { Worker, isMainThread, workerData, parentPort } from 'node:worker_threads';
import { sessionEnv, sessionSteps } from './session.mjs';
import { isImageDir, openImageDir, driveName, blobPath } from './image.mjs';
import { createKernel, processImports } from './kernel.mjs';
import { loadSide } from './side.mjs';

const EXIT = new BroadcastChannel('pisco-exit');
const GPU_OPEN = new BroadcastChannel('pisco-gpu'); // a title thread opened the GPU ring: the supervisor runs the null back end
const STACK_MB = +(process.env.PISCO_STACK_MB || 64); // wasm frames of recursive guest code live on the worker's native stack

if (isMainThread) {
  const [first, ...rest] = process.argv.slice(2);
  let image = null, module, args, dir = null;
  if (await isImageDir(first)) {
    image = await openImageDir(first);
    module = await WebAssembly.compile(image.wasm); image.wasm = null;
    args = rest.length ? rest : image.settings.args;   // (arguments given replace the image's, as a container's command does)
  } else {
    const [exePath, ...more] = rest; args = more;
    module = await WebAssembly.compile(fs.readFileSync(first));
    dir = process.env.SW_ROOT ? path.resolve(process.env.SW_ROOT) : path.dirname(path.resolve(exePath)); // the title's folder: its exe and the DLLs it ships
  }
  // a Linux title: its processes (web/kernel.mjs), each in a worker of its own
  const kernel = WebAssembly.Module.exports(module).some((e) => e.name === 'rt_fork_resume') ? createKernel() : null;
  const programs = image?.config.programs || null;   // a title image's other programs: path -> module blob
  const side = image?.config.side || null;           // a machine's programs in modules of their own: name -> blob
  const memory = new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true });
  const tids = new Int32Array(new SharedArrayBuffer(4)); tids[0] = 1;
  // SW_SAVES=<folder>: what the title keeps between runs (rt/vfs.c) lives there: its persistent storage (S:\) at the
  // top, and a Windows desktop title's writes in its own folder (the top layer over it) in <folder>/@game
  const saves = process.env.SW_SAVES ? path.resolve(process.env.SW_SAVES) : null, store = [];
  const walk = (d, rel, mount) => { let es = []; try { es = fs.readdirSync(d, { withFileTypes: true }); } catch {} for (const e of es) {
    const p = path.join(d, e.name), r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory() && !rel && e.name.startsWith('@')) walk(p, '', e.name.slice(1)); else if (e.isDirectory()) walk(p, r, mount); else store.push({ name: `${mount}/${r}`, data: fs.readFileSync(p) }); } };
  if (saves) walk(saves, '', 'pls');
  // the null GPU back end's account of the explicit records (Direct3D 9): a run in which any was inconsistent fails
  let gpu = null; const say = (s) => fs.writeSync(2, `pisco: ${s}\n`);
  EXIT.onmessage = (e) => { const s = gpu?.summary(); if (s) say(s); process.exit(e.data || (gpu?.errors ? 119 : 0)); };
  // SW_SESSION=<session.json>: the runtime's session player gets its steps (rt/windows-x86/input.c)
  const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => k.startsWith('SW_')));
  // the image's settings for the runtime (its registry values: SW_REGISTRY), with the environment's after them
  if (image) for (const [k, v] of Object.entries(image.settings.env)) env[k] = k === 'SW_REGISTRY' && env[k] ? `${v}\n${env[k]}` : env[k] ?? v;
  const imageDir = image ? path.resolve(first) : null, folder = image ? image.settings.folder : null;
  if (process.env.SW_SESSION) { const s = JSON.parse(fs.readFileSync(process.env.SW_SESSION, 'utf8')); env.SW_SESSION_STEPS = sessionSteps(s); for (const [k, v] of Object.entries(sessionEnv(s))) if (!(k in env)) env[k] = v; }
  GPU_OPEN.onmessage = async (e) => {
    const { startNull } = await import('./gpu/null.js');
    gpu = startNull(memory, e.data, { dir, log: say });
  };
  const compiled = new Map();
  const moduleOf = (where) => {
    const key = where.file || where.blob;
    if (!compiled.has(key)) compiled.set(key, WebAssembly.compile(where.file ? fs.readFileSync(where.file) : fs.readFileSync(path.join(imageDir, blobPath(where.blob)))));
    return compiled.get(key);
  };
  // a machine's console (rt/microvm-x64/devices.c): standard input into its ring, a terminal's keys as they are typed
  // (raw; Ctrl-] ends the run), a pipe's bytes and then its end
  if (process.stdin.isTTY) env.SW_CONSOLE = 'tty';
  const startConsole = (addr) => {
    const u32 = new Uint32Array(memory.buffer, addr, 4), i32 = new Int32Array(memory.buffer, addr, 4), data = new Uint8Array(memory.buffer, addr + 16, u32[2]);
    const put = (bytes) => { for (const b of bytes) { if (u32[0] - Atomics.load(u32, 1) >= u32[2]) break; data[u32[0] % u32[2]] = b; Atomics.store(u32, 0, u32[0] + 1); } Atomics.notify(i32, 0); };
    if (process.stdin.isTTY) process.stdin.setRawMode(true);
    process.stdin.on('data', (b) => { if (process.stdin.isTTY && b.includes(0x1d)) { process.stdin.setRawMode(false); process.exit(0); } put(b); });
    process.stdin.on('end', () => { Atomics.store(u32, 3, 1); Atomics.notify(i32, 0); });
  };
  const startProcess = (data) => {
    const w = new Worker(new URL(import.meta.url), { workerData: { t0: performance.timeOrigin, dir, imageDir, folder, saves, store, kernel, programs, side, tid: 0, ...data },
      resourceLimits: { stackSizeMb: STACK_MB } });
    w.on('error', (e) => { console.error(e); process.exit(121); });
    w.on('message', async (m) => {
      if (m.console !== undefined) startConsole(m.console);
      if (m.fork) startProcess({ module: m.fork.module, memory: m.fork.memory, pid: m.fork.pid, mode: 'fork', tids: new Int32Array(new SharedArrayBuffer(4)).fill(1), args: [], env });
      if (m.exec) {
        const mod = await moduleOf(m.exec.program);
        startProcess({ module: mod, memory: new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true }), pid: m.exec.pid, mode: 'exec',
          tids: new Int32Array(new SharedArrayBuffer(4)).fill(1), args: m.exec.args, env: { ...env, ...m.exec.settings } });
        w.terminate();
      }
    });
  };
  startProcess({ module, memory, tids, args, env, pid: 1, mode: 'start' });
} else {
  const { module, memory, dir, imageDir, folder, tids, tid, args, env, saves, store, t0, kernel, programs, side = null, pid = 1, mode = 'start' } = workerData;
  // a title image: its drive, read from the layers (the main thread has checked their digests)
  const drive = imageDir ? (await openImageDir(imageDir, { verify: false, wasm: false })).drive : null;
  const clock = () => performance.timeOrigin + performance.now() - t0; // one monotonic clock for every thread
  const kinds = ['trap', 'instruction not lifted', 'import not implemented', 'indirect call to unknown code', 'image does not fit', 'fatal'];
  const u8 = () => new Uint8Array(memory.buffer);
  const dv = () => new DataView(memory.buffer);
  const text = (p, n) => Buffer.from(memory.buffer, p, n).toString();
  const exit = (code) => { EXIT.postMessage(code); Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0); };
  const argv = ['title.wasm', ...args], envv = Object.entries(env).map(([k, v]) => `${k}=${v}`);
  const strings = (list, ptrs, buf) => { let p = buf; list.forEach((s, i) => { dv().setUint32(ptrs + 4 * i, p, true); const b = Buffer.from(s + '\0'); u8().set(b, p); p += b.length; }); return 0; };
  const sizes = (list, a, b) => { dv().setUint32(a, list.length, true); dv().setUint32(b, list.reduce((n, s) => n + Buffer.byteLength(s) + 1, 0), true); return 0; };
  const sleeper = new Int32Array(new SharedArrayBuffer(4));
  const wasi = {
    args_get: (p, b) => strings(argv, p, b), args_sizes_get: (a, b) => sizes(argv, a, b),
    environ_get: (p, b) => strings(envv, p, b), environ_sizes_get: (a, b) => sizes(envv, a, b),
    fd_write: (fd, iovs, n, written) => {
      let total = 0;
      for (let i = 0; i < n; i++) { const p = dv().getUint32(iovs + 8 * i, true), len = dv().getUint32(iovs + 8 * i + 4, true);
        if (fd === 1 || fd === 2) fs.writeSync(fd, new Uint8Array(memory.buffer, p, len).slice()); total += len; }
      dv().setUint32(written, total, true); return fd === 1 || fd === 2 ? 0 : 8;
    },
    fd_read: (fd, iovs, n, read) => {
      if (fd !== 0) return 8;
      let total = 0;
      for (let i = 0; i < n; i++) { const p = dv().getUint32(iovs + 8 * i, true), len = dv().getUint32(iovs + 8 * i + 4, true);
        const tmp = Buffer.alloc(len); let k = 0; try { k = fs.readSync(0, tmp, 0, len, null); } catch { k = 0; }
        u8().set(tmp.subarray(0, k), p); total += k; if (k < len) break; }
      dv().setUint32(read, total, true); return 0;
    },
    fd_close: () => 0, fd_seek: () => 70, fd_sync: () => 0,
    fd_fdstat_get: (fd, p) => { if (fd > 2) return 8; u8().fill(0, p, p + 24); dv().setUint8(p, 2); return 0; },
    fd_fdstat_set_flags: () => 0, fd_filestat_get: () => 8,
    fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8, path_open: () => 44, path_filestat_get: () => 44,
    proc_exit: (code) => exit(code),
    clock_time_get: (id, prec, out) => { const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(clock() * 1e6)); dv().setBigUint64(out, ns, true); return 0; },
    clock_res_get: (id, out) => { dv().setBigUint64(out, 1000n, true); return 0; },
    random_get: (p, n) => { crypto.getRandomValues(new Uint8Array(memory.buffer, p, n).subarray(0)); return 0; },
    sched_yield: () => 0,
    poll_oneoff: (subs, events, n, nevents) => {
      // only clock subscriptions (nanosleep): block this worker
      let ns = 0n;
      for (let i = 0; i < n; i++) { const base = subs + 48 * i; if (dv().getUint8(base + 8) === 0) { const t = dv().getBigUint64(base + 24, true); if (t > ns) ns = t; } }
      Atomics.wait(sleeper, 0, 0, Number(ns) / 1e6);
      dv().setUint32(nevents, 0, true); return 0;
    },
  };
  // a path in the title's folder, matched without regard to case as Windows does: the real path or null
  const resolve = (name) => {
    let p = dir; name = name.replace(/^c:(\/|$)/i, '');   // (SW_ROOT: a Linux title's root is the folder)
    for (const part of name.split(/[\\/]/).filter(Boolean)) {
      let list; try { list = fs.readdirSync(p); } catch { return null; }
      const real = list.find((f) => f.toLowerCase() === part.toLowerCase()); if (!real) return null;
      p = path.join(p, real);
    }
    return p;
  };
  const fds = new Map(); let wav = null;
  // a stored file's name (its mount, then its path: "pls/...", "game/...") -> where it is kept in SW_SAVES
  const savedPath = (name) => { const [mount, ...rest] = name.split('/'); return path.join(saves, ...(mount === 'pls' ? [] : [`@${mount}`]), ...rest); };
  const open = (name) => {
    if (!fds.has(name)) { const real = resolve(name); let fd = -1; try { if (real) fd = fs.openSync(real, 'r'); } catch {} fds.set(name, fd); }
    return fds.get(name);
  };
  // the title's files from its image's drive: the names it asks for are in its folder, or "c:/..." on the drive
  const imageFiles = drive && {
    read_file: (name, nlen, off, len, dst) => { const b = drive.read(driveName(text(name, nlen), folder), Number(off), Number(len)); if (!b) return 0; u8().set(b, Number(dst)); return b.length; },
    stat: (name, nlen) => BigInt(drive.stat(driveName(text(name, nlen), folder))),
    list: (name, nlen, buf, cap) => { const l = drive.list(driveName(text(name, nlen), folder)); if (l === null) return 0; const b = Buffer.from(l); if (buf && cap >= b.length) u8().set(b, buf); return b.length; },
  };
  // SW_KEYS=<json>: [[ms, code, keyCode, down], ...] times in milliseconds on the virtual clock (SW_CLOCK)
  // (the same list tools/run-chrome.mjs sends the page). Applied to the input block just after each flip, for the next frame.
  const keyScript = env.SW_KEYS ? JSON.parse(env.SW_KEYS) : [];
  let inputBlock = 0, held = new Map(), dumpRaw = null;
  const applyKeys = (ms) => {
    if (!inputBlock) return;
    held.clear();
    for (const ev of keyScript) {
      const t = ev[0], code = ev[1], vk = ev[2], down = ev[3];
      if (t <= ms) {
        if (down) held.set(code, vk & 0xFF); else held.delete(code);
      }
    }
    const view = new DataView(memory.buffer, inputBlock, 128);
    const list = [...held.values()].slice(0, 16);
    view.setUint32(4, list.length, true);
    for (let i = 0; i < 16; i++) view.setUint32(8 + i * 4, i < list.length ? list[i] : 0, true);
    view.setUint32(0, view.getUint32(0, true) + 1, true);
  };

  const host = {
    fault: (kind, a) => { console.error(`pisco: ${kinds[kind]}${kind === 5 ? '' : ` at 0x${BigInt.asUintN(64, a).toString(16)}`}`); exit(120); },
    now_us: () => clock() * 1000,
    read_file: (name, nlen, off, len, dst) => {
      const fd = open(text(name, nlen)); if (fd < 0) { if (process.env.SW_DEBUG_READ) console.error(`read_file ${text(name, nlen)}: not found`); return 0; }
      const got = fs.readSync(fd, new Uint8Array(memory.buffer, Number(dst), Number(len)), 0, Number(len), Number(off));
      if (got < Number(len) && process.env.SW_DEBUG_READ) console.error(`read_file ${text(name, nlen)} off ${off} len ${len} dst ${dst} -> ${got}`);
      return got;
    },
    stat: (name, nlen) => { const real = resolve(text(name, nlen)); if (!real) return -1n; try { const st = fs.statSync(real); return st.isDirectory() ? -2n : BigInt(st.size); } catch { return -1n; } },
    list: (name, nlen, buf, cap) => {
      const real = resolve(text(name, nlen)); if (!real) return 0;
      let out = ''; try { for (const e of fs.readdirSync(real, { withFileTypes: true })) out += e.name + (e.isDirectory() ? '/' : '') + '\n'; } catch { return 0; }
      const b = Buffer.from(out); if (buf && cap >= b.length) u8().set(b, buf); return b.length;
    },
    log: (p, n) => { fs.writeSync(2, `pisco: ${text(p, n)}\n`); },
    gpu_open: (ring) => { GPU_OPEN.postMessage(ring >>> 0); },
    audio_open: () => {}, // no output in Node: the mixer's ring turns on its own
    // SW_AUDIO_WAV=<file> (with a virtual clock): everything mixed, as a 48 kHz stereo 32-bit float WAV
    audio_dump: (p, n) => {
      if (!wav) { wav = { fd: fs.openSync(env.SW_AUDIO_WAV, 'w'), bytes: 0 }; const h = Buffer.alloc(44); h.write('RIFF', 0); h.write('WAVEfmt ', 8); h.writeUInt32LE(16, 16); h.writeUInt16LE(3, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(48000, 24); h.writeUInt32LE(48000 * 8, 28); h.writeUInt16LE(8, 32); h.writeUInt16LE(32, 34); h.write('data', 36); fs.writeSync(wav.fd, h, 0, 44, 0); }
      fs.writeSync(wav.fd, new Uint8Array(memory.buffer, p, 8 * n), 0, 8 * n, 44 + wav.bytes); wav.bytes += 8 * n;
      const size = Buffer.alloc(4); size.writeUInt32LE(36 + wav.bytes); fs.writeSync(wav.fd, size, 0, 4, 4); size.writeUInt32LE(wav.bytes); fs.writeSync(wav.fd, size, 0, 4, 40);
    },
    store_info: (i, name, cap) => { const f = store[i]; if (!f) return -1n; const b = Buffer.from(f.name + '\0'); if (b.length <= cap) u8().set(b, name); return BigInt(f.data.length); },
    store_read: (i, dst) => { u8().set(store[i].data, dst); },
    store_save: (name, len, data, n) => { if (!saves) return; const p = savedPath(text(name, len)); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, new Uint8Array(memory.buffer, data, n)); },
    store_remove: (name, len) => { if (saves) fs.rmSync(savedPath(text(name, len)), { force: true }); },
    frame_dump: (pixels, width, height, frame) => {
      if (!dumpRaw && env.SW_DUMP_RAW) dumpRaw = fs.openSync(env.SW_DUMP_RAW, 'w');
      if (dumpRaw) {
        const n = Number(width) * Number(height) * 4;
        fs.writeSync(dumpRaw, new Uint8Array(memory.buffer, Number(pixels), n), 0, n);
      }
      // SW_CLOCK=<us/frame>: the time of this flip, for the keys the next frame will see
      const step = +(env.SW_CLOCK || 0);
      if (step) applyKeys(Number(frame) * step / 1000);
    },
    input_open: (block) => { inputBlock = block >>> 0; applyKeys(0); }, // SW_KEYS writes here; otherwise readings stay empty
    win32_input_open: () => {}, // the same for a Windows desktop title (its sessions come from SW_SESSION)
    console_open: (ring) => parentPort.postMessage({ console: ring >>> 0 }), // a machine's console (rt/microvm-x64): the main thread writes standard input there
    exit: (code) => exit(code),
    ...imageFiles,
  };
  if (kernel) Object.assign(host, processImports(kernel, { pid, memory: () => memory, module, ask: (m) => parentPort.postMessage(m), end: () => process.exit(0),
    // where a program's module is: a title image's (its config), or beside the program in SW_ROOT
    program: (name) => {
      const p = name.replace(/^c:\//i, '');
      if (programs) return programs['/' + p] ? { blob: programs['/' + p] } : null;
      const f = path.join(dir, p + '.wasm'); return fs.existsSync(f) ? { file: f } : null;
    } }), pid === 1 ? { exit: host.exit } : {});
  const spawn = (arg) => {
    const t = Atomics.add(tids, 0, 1);
    new Worker(new URL(import.meta.url), { workerData: { t0, module, memory, dir, imageDir, folder, tids, tid: t, args, env, saves, store, start: arg }, resourceLimits: { stackSizeMb: STACK_MB } })
      .on('error', (e) => { console.error(e); exit(122); });
    return t;
  };
  // a machine (rt/microvm-x64): each of its kernel's tasks on a wasm stack of its own (JSPI). co_switch suspends the
  // task that switches away and resumes the one switched to, or starts it (rt_co_entry)
  const machine = WebAssembly.Module.exports(module).some((e) => e.name === 'rt_co_entry');
  let coEntry = null;
  if (machine) {
    const parked = new Map();
    host.co_switch = new WebAssembly.Suspending((from, to, start, dead) => {
      const mine = new Promise((resolve) => { if (!dead) parked.set(from, resolve); });   // (a task that died is never resumed: its stack is dropped)
      if (start) coEntry(to).catch((e) => { fs.writeSync(2, String(e.stack || e) + "\n"); exit(123); });
      else { const go = parked.get(to); if (!go) throw new Error(`machine: task ${to} is not waiting`); parked.delete(to); go(); }
      return mine;
    });
  }
  const instance = new WebAssembly.Instance(module, { env: { memory }, wasi_snapshot_preview1: wasi, wasi: { 'thread-spawn': spawn }, host });
  try {
    if (machine) {
      coEntry = WebAssembly.promising(instance.exports.rt_co_entry);
      // its programs in modules of their own: its image's (config.side), and SW_PROGRAMS's (<name>.wasm files here)
      for (const [n, digest] of Object.entries(side || {})) loadSide(fs.readFileSync(path.join(imageDir, blobPath(digest))), instance, memory, n);
      for (const f of (env.SW_PROGRAMS || '').split(/\s+/).filter(Boolean)) {
        const bytes = drive ? drive.read(driveName(f, folder), 0, Infinity) : fs.readFileSync(path.resolve(f));
        loadSide(bytes, instance, memory, path.basename(f, '.wasm').replace(/[^A-Za-z0-9_]/g, '_').replace(/^(\d)/, '_$1'));
      }
      await WebAssembly.promising(instance.exports._start)(); exit(0);
    }
    else if (tid === 0 && mode === 'fork') { instance.exports.rt_fork_resume(); exit(0); }
    else if (tid === 0) { instance.exports._start(); exit(0); }
    else instance.exports.wasi_thread_start(tid, workerData.start);
  } catch (e) {
    fs.writeSync(2, String(e.stack || e) + "\n");   // (synchronously: a worker's console is flushed by the main thread, maybe too late)
    const at = instance.exports.rt_last_pc; // lifted with --trace-pc: the guest block last entered
    if (at) { const pc = new Uint32Array(memory.buffer, Number(at.value), 1)[0]; fs.writeSync(2, `pisco: the last guest block entered: 0x${pc.toString(16)}\n`); }
    exit(123);
  }
}
