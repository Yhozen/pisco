// SPDX-License-Identifier: GPL-2.0-or-later
// One guest thread. Output and exit go through a BroadcastChannel: the page reads it, whichever worker writes.
// The title's files come from its folder's URL (`dir`), or for a title image from its drive (`drive`: the file layers,
// served by tools/serve.mjs) where its folder is `folder` (web/image.mjs).
import { loadSide } from '../side.mjs';
import { driveName } from '../image.mjs';
import { processImports } from '../kernel.mjs';
const bus = new BroadcastChannel('pisco');
bus.onmessage = (e) => { if (e.data.stop) close(); };
const kinds = ['trap', 'instruction not lifted', 'import not implemented', 'indirect call to unknown code', 'image does not fit', 'fatal'];
// A pooled worker waits on its slot of `ctl` (state, tid, start argument): 1 = idle, 2 = claimed.
onmessage = (e) => {
  const { pool } = e.data;
  if (pool && pool.slot >= 0) {
    const { ctl, slot } = pool, at = slot * 4;
    Atomics.store(ctl, at, 1);
    while (Atomics.load(ctl, at) !== 2) Atomics.wait(ctl, at, 1);
    bus.postMessage({ refill: slot }); // the page starts a new worker for this slot
    run({ ...e.data, tid: ctl[at + 1], start: ctl[at + 2] });
  } else run(e.data);
};
function run(d) {
  const { module, memory, dir, drive = null, folder = null, tids, tid, args, env, start, pool, store = [], t0 = 0, kernel = null, programs = null, side = [], pid = 1, mode = 'start' } = d;
  // one monotonic clock for every thread: each worker's performance.now() counts from its own start
  const clock = () => performance.timeOrigin + performance.now() - t0;
  const u8 = () => new Uint8Array(memory.buffer), dv = () => new DataView(memory.buffer);
  const dec = new TextDecoder(), enc = new TextEncoder();
  const text = (p, n) => dec.decode(new Uint8Array(memory.buffer, p, n).slice());
  const exit = (code) => { bus.postMessage({ exit: code }); Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0); };
  const argv = ['title.wasm', ...args], envv = Object.entries(env).map(([k, v]) => `${k}=${v}`);
  const strings = (list, ptrs, buf) => { let p = buf; list.forEach((s, i) => { dv().setUint32(ptrs + 4 * i, p, true); const b = enc.encode(s + '\0'); u8().set(b, p); p += b.length; }); return 0; };
  const sizes = (list, a, b) => { dv().setUint32(a, list.length, true); dv().setUint32(b, list.reduce((n, s) => n + enc.encode(s).length + 1, 0), true); return 0; };
  const sleeper = new Int32Array(new SharedArrayBuffer(4));
  const wasi = {
    args_get: (p, b) => strings(argv, p, b), args_sizes_get: (a, b) => sizes(argv, a, b),
    environ_get: (p, b) => strings(envv, p, b), environ_sizes_get: (a, b) => sizes(envv, a, b),
    fd_write: (fd, iovs, n, written) => {
      let total = 0, s = '';
      for (let i = 0; i < n; i++) { const p = dv().getUint32(iovs + 8 * i, true), len = dv().getUint32(iovs + 8 * i + 4, true); s += text(p, len); total += len; }
      if (fd === 1 || fd === 2) bus.postMessage({ out: s });
      dv().setUint32(written, total, true); return fd === 1 || fd === 2 ? 0 : 8;
    },
    fd_read: (fd, iovs, n, read) => { dv().setUint32(read, 0, true); return fd === 0 ? 0 : 8; },
    fd_close: () => 0, fd_seek: () => 70, fd_sync: () => 0,
    fd_fdstat_get: (fd, p) => { if (fd > 2) return 8; u8().fill(0, p, p + 24); dv().setUint8(p, 2); return 0; },
    fd_fdstat_set_flags: () => 0, fd_filestat_get: () => 8,
    fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8, path_open: () => 44, path_filestat_get: () => 44,
    proc_exit: (code) => exit(code),
    clock_time_get: (id, prec, out) => { const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(clock() * 1e6)); dv().setBigUint64(out, ns, true); return 0; },
    clock_res_get: (id, out) => { dv().setBigUint64(out, 1000n, true); return 0; },
    random_get: (p, n) => { const t = new Uint8Array(n); crypto.getRandomValues(t); u8().set(t, p); return 0; },
    sched_yield: () => 0,
    poll_oneoff: (subs, events, n, nevents) => {
      let ns = 0n;
      for (let i = 0; i < n; i++) { const base = subs + 48 * i; if (dv().getUint8(base + 8) === 0) { const t = dv().getBigUint64(base + 24, true); if (t > ns) ns = t; } }
      Atomics.wait(sleeper, 0, 0, Number(ns) / 1e6); dv().setUint32(nevents, 0, true); return 0;
    },
  };
  // the title's files, fetched on first use (a synchronous request: workers may block)
  const files = new Map();
  const enc_ = (p) => p.split('/').map(encodeURIComponent).join('/');
  // a name the runtime asks for: in the title's folder, or "c:/..." on the drive (only a title image has one)
  const urlOf = (name) => (drive ? drive + enc_(driveName(name, folder)) : /^c:(\/|$)/i.test(name) ? null : dir + enc_(name));
  const file = (name) => {
    if (!files.has(name)) { const u = urlOf(name); let data = null; if (u) { const x = new XMLHttpRequest(); x.open('GET', u, false); x.responseType = 'arraybuffer'; x.send(); if (x.status === 200) data = new Uint8Array(x.response); } files.set(name, data); }
    return files.get(name);
  };
  // a file's size or kind, or a folder's listing, asked once: the server's files never change while the title runs (its
  // writes stay in rt/vfs.c's overlay), and each request blocks the title's thread for a round trip (Tag reopens its
  // textures dozens of times a second: uncached, its thread spent most of its time waiting here)
  const answers = new Map();
  const ask = (name, what) => {
    const key = `${what}\0${name}`;
    if (!answers.has(key)) { const u = urlOf(name); let r = null; if (u) { const x = new XMLHttpRequest(); x.open('GET', `${u}?${what}`, false); x.send(); if (x.status === 200) r = x.responseText; } answers.set(key, r); }
    return answers.get(key);
  };
  const host = {
    fault: (kind, a) => { bus.postMessage({ out: `pisco: ${kinds[kind]}${kind === 5 ? '' : ` at 0x${BigInt.asUintN(64, a).toString(16)}`}\n` }); exit(120); },
    now_us: () => clock() * 1000,
    read_file: (name, nlen, off, len, dst) => {
      const f = file(text(name, nlen)); if (!f) return 0;
      const o = Number(off), src = f.subarray(o, o + Number(len)); u8().set(src, Number(dst)); return src.length;
    },
    stat: (name, nlen) => { const r = ask(text(name, nlen), 'stat'); return r === null ? -1n : BigInt(r); },
    // a Linux drive's links and executable bits (tools/serve.mjs answers ?link and ?exec)
    link: (name, nlen, buf, cap) => { const r = ask(text(name, nlen), 'link'); if (r === null) return -1; const b = enc.encode(r); if (b.length > cap) return -1; u8().set(b, buf); return b.length; },
    exec: (name, nlen) => { const r = ask(text(name, nlen), 'exec'); return r === null ? -1 : +r; },
    list: (name, nlen, buf, cap) => {
      const r = ask(text(name, nlen), 'list'); if (r === null) return 0;
      const b = enc.encode(r); if (buf && cap >= b.length) u8().set(b, buf); return b.length;
    },
    log: (p, n) => bus.postMessage({ out: `pisco: ${text(p, n)}\n` }),
    // the title's persistent storage: the page loaded it from OPFS before the title started, and saves changes there
    store_info: (i, name, cap) => { const f = store[i]; if (!f) return -1n; const b = enc.encode(f.name + '\0'); if (b.length <= cap) u8().set(b, name); return BigInt(f.data.length); },
    store_read: (i, dst) => { u8().set(store[i].data, dst); },
    store_save: (name, len, data, n) => bus.postMessage({ save: { name: text(name, len), data: new Uint8Array(memory.buffer, data, n).slice() } }),
    store_remove: (name, len) => bus.postMessage({ remove: text(name, len) }),
    gpu_open: (ring) => bus.postMessage({ gpu: ring >>> 0 }),     // the page runs the GPU back end on this ring
    audio_open: (ring) => bus.postMessage({ audio: ring >>> 0 }), // and plays this one
    audio_dump: () => {}, // SW_AUDIO_WAV is Node's: a recording in the browser reads the ring (main.js)
    frame_dump: () => {}, // SW_DUMP_RAW is Node's: an offline render writes presented frames (run-node.mjs)
    input_open: (block) => bus.postMessage({ input: block >>> 0 }), // and writes the keyboard and gamepad here
    win32_input_open: (ring) => bus.postMessage({ win32input: ring >>> 0 }), // a desktop title's keyboard and mouse events (rt/windows-x86/input.c)
    console_open: (ring) => bus.postMessage({ console: ring >>> 0 }), // a machine's console (rt/microvm-x64): the page's keys go there
    exit: (code) => exit(code),
  };
  // a Linux title's process (web/kernel.mjs): the page starts the workers of a fork and of an execve
  if (kernel) Object.assign(host, processImports(kernel, { pid, memory: () => memory, module, ask: (m) => postMessage(m), end: () => close(),
    program: (name) => { const p = '/' + name.replace(/^c:\//i, ''); return programs && programs[p] ? { blob: programs[p] } : null; } }), pid === 1 ? { exit: host.exit } : {});
  // a new guest thread: claim an idle pooled worker (the page cannot start one for us while we block)
  const spawn = (arg) => {
    const { ctl } = pool;
    for (let i = 0; i < ctl.length / 4; i++) {
      if (Atomics.compareExchange(ctl, 4 * i, 1, 3) === 1) {
        const t = Atomics.add(tids, 0, 1);
        ctl[4 * i + 1] = t; ctl[4 * i + 2] = arg;
        Atomics.store(ctl, 4 * i, 2); Atomics.notify(ctl, 4 * i);
        return t;
      }
    }
    return -6; // EAGAIN: no idle worker
  };
  // a machine (rt/microvm-x64): each of its kernel's tasks on a wasm stack of its own (JSPI), as in web/run-node.mjs
  const machine = WebAssembly.Module.exports(module).some((e) => e.name === 'rt_co_entry');
  let coEntry = null;
  if (machine) {
    host.program_image = () => 0;   // (programs lifted when they first run need the lifter: Node only, web/lift.mjs)
    // (a network needs a host that terminates its frames: Node only for now, web/net-node.mjs)
    host.net_open = () => bus.postMessage({ out: 'pisco: no network in the page yet (SW_NET is for Node)\n' });
    host.net_send = () => {};
    const parked = new Map();
    host.co_switch = new WebAssembly.Suspending((from, to, begin, dead) => {
      const mine = new Promise((resolve) => { if (!dead) parked.set(from, resolve); });   // (a task that died is never resumed: its stack is dropped)
      if (begin) coEntry(to).catch((err) => { bus.postMessage({ out: `pisco: ${err && err.stack || err}\n` }); exit(123); });
      else { const go = parked.get(to); if (!go) throw new Error(`machine: task ${to} is not waiting`); parked.delete(to); go(); }
      return mine;
    });
  }
  const instance = new WebAssembly.Instance(module, { env: { memory }, wasi_snapshot_preview1: wasi, wasi: { 'thread-spawn': spawn }, host });
  if (machine) {
    coEntry = WebAssembly.promising(instance.exports.rt_co_entry);
    for (const [n, bytes] of side) loadSide(bytes, instance, memory, n);   // its programs in modules of their own
    WebAssembly.promising(instance.exports._start)().then(() => exit(0), (err) => { bus.postMessage({ out: `pisco: ${err && err.stack || err}\n` }); exit(123); });
    return;
  }
  try {
    if (tid === 0 && mode === 'fork') { instance.exports.rt_fork_resume(); exit(0); }
    else if (tid === 0) { instance.exports._start(); exit(0); }
    else instance.exports.wasi_thread_start(tid, start);
  } catch (err) { bus.postMessage({ out: `pisco: ${err && err.stack || err}\n` }); exit(123); }
}
