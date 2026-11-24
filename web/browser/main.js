// SPDX-License-Identifier: GPL-2.0-or-later
// Starts a lifted title in this page: ?wasm=<title.wasm>&exe=<the title's .exe>&args=<space-separated>
// The title's threads (its main thread too) run in workers; this page owns what a console would: the GPU
// (web/gpu/backend.js, on this thread), the speakers (an AudioWorklet playing the title's mixer ring), and the
// keyboard and gamepad (written into the title's input block).
// Options: &pace=off (no frame clock), &hash=1 (log a hash of every presented frame),
//          &capture=1 (send presented frames and the audio to the server, which records them: tools/serve.mjs),
//          &SW_<NAME>=<value>: the runtime's settings, as the Node host takes them from the environment. For
//          Windows desktop titles: SW_CLOCK=16667 (a virtual clock: that many microseconds a presented frame) and
//          SW_SESSION_STEPS=<steps> (a scripted session, as web/session.mjs writes one; tools/run-chrome.mjs
//          makes it from SW_SESSION=<session.json>). At each `shot` of a session the frame presented last is
//          sent to the server (POST /capture/shot), which saves it as a PNG.
import { startBackend } from '../gpu/backend.js';
const q = new URLSearchParams(location.search);
const status = document.getElementById('status'), out = document.getElementById('out'), canvas = document.getElementById('screen');
const bus = new BroadcastChannel('series-web');
const workers = [];
let line = '';
const POOL = 32; // guest threads come from workers the page starts ahead of time (see worker.js)
let ctl, boot, memory, device, dir;
const log = (s) => { console.log(s); out.textContent += s + '\n'; };
const startPooled = (slot) => { const w = new Worker('worker.js'); workers.push(w); w.postMessage({ ...boot, pool: { ctl, slot } }); };
const capture = q.get('capture') === '1', hashFrames = q.get('hash') === '1';
const post = (path, body, headers = {}) => fetch(path, { method: 'POST', body, headers }).catch(() => {});

// ---- presented frames: hashes for tests, frames for recordings ----
function onFrame(f) {
  if (hashFrames) {
    let h = 2166136261 >>> 0; const p = f.pixels, bgra = f.format.startsWith('bgra');
    for (let i = 0; i < p.length; i += 4) for (const k of bgra ? [0, 1, 2] : [2, 1, 0]) h = Math.imul(h ^ p[i + k], 16777619) >>> 0;
    console.log(`gpu frame ${f.index} ${h.toString(16).padStart(8, '0')}`);
  }
  // wall-clock times (they continue across a reload of the page)
  if (capture) post('/capture/frame', f.pixels, { 'x-frame': JSON.stringify({ index: f.index, time: performance.timeOrigin + f.time, width: f.width, height: f.height, format: f.format }) });
}
// a session's checkpoint: the frame it names, to the server; and how fast frames came since the one before
let lastShot = { index: 0, time: performance.now() };
async function onShot(f) {
  const now = performance.now(), frames = f.index + 1 - lastShot.index, s = (now - lastShot.time) / 1000;
  const reply = await fetch('/capture/shot', { method: 'POST', body: f.pixels, headers: { 'x-shot': JSON.stringify({ name: f.name, width: f.width, height: f.height, format: f.format }) } }).then((r) => r.text()).catch((e) => e.message);
  console.log(`shot ${f.name}: frame ${f.index + 1} at ${(now / 1000).toFixed(1)} s, ${reply}; ${frames} frames since the last in ${s.toFixed(1)} s (${(frames / s).toFixed(1)} a second)`);
  lastShot = { index: f.index + 1, time: performance.now() };
}
// ---- audio: the title's mixer ring (rt/xaudio2.c) -> an AudioWorklet; recordings read the same ring ----
async function startAudio(addr) {
  let ctx;
  try {
    ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ctx, 'series-web-ring', { outputChannelCount: [2] });
    node.port.postMessage({ buffer: memory.buffer, addr });
    node.connect(ctx.destination);
    if (ctx.state !== 'running') { const resume = () => ctx.resume(); addEventListener('keydown', resume, { once: true }); addEventListener('pointerdown', resume, { once: true }); }
  } catch (e) { log(`audio: ${e.message}`); }
  if (capture) {
    const u32 = new Uint32Array(memory.buffer, addr, 16), cap = u32[2], data = new Float32Array(memory.buffer, addr + 64, cap * 2);
    let read = Atomics.load(u32, 0); const rate = u32[3], lead = 2400;
    setInterval(() => {
      const w = Atomics.load(u32, 0); if (w - read > cap) read = w - cap;
      const n = w - read; if (!n) return;
      const chunk = new Float32Array(n * 2);
      for (let i = 0; i < n; i++) { const at = (read + i) % cap; chunk[2 * i] = data[2 * at]; chunk[2 * i + 1] = data[2 * at + 1]; }
      read = w;
      // the mixer runs `lead` frames ahead of the clock: the chunk's first frame plays at now + (lead - n) / rate
      const time = performance.timeOrigin + performance.now() + (lead - n) / rate * 1000;
      post('/capture/audio', chunk.buffer, { 'x-audio': JSON.stringify({ time, rate }) });
    }, 100);
  }
}
// ---- input: keys held and the first gamepad, into the title's input block (rt/gdk.c) ----
function startInput(addr) {
  const u32 = new Uint32Array(memory.buffer, addr, 32), f32 = new Float32Array(memory.buffer, addr, 32);
  const held = new Map(); let seq = 0;
  const keys = () => { const list = [...held.values()].slice(0, 16); u32[1] = list.length; list.forEach((k, i) => { u32[2 + i] = k; }); u32[0] = ++seq; };
  const scan = (code) => ({ ArrowUp: 0x48, ArrowDown: 0x50, ArrowLeft: 0x4B, ArrowRight: 0x4D, Enter: 0x1C, Escape: 0x01, Space: 0x39 }[code] || 0);
  addEventListener('keydown', (e) => { held.set(e.code, e.keyCode | scan(e.code) << 8); keys(); if (e.keyCode >= 32 && e.keyCode <= 40) e.preventDefault(); });
  addEventListener('keyup', (e) => { held.delete(e.code); keys(); });
  addEventListener('blur', () => { held.clear(); keys(); });
  const poll = () => {
    const pad = navigator.getGamepads ? [...navigator.getGamepads()].find((p) => p && p.connected) : null;
    if (pad) {
      const b = (i) => pad.buttons[i]?.pressed;
      const bits = [[9, 1], [8, 2], [0, 4], [1, 8], [2, 0x10], [3, 0x20], [12, 0x40], [13, 0x80], [14, 0x100], [15, 0x200], [4, 0x400], [5, 0x800], [10, 0x1000], [11, 0x2000]];
      u32[18] = 1; u32[19] = bits.reduce((m, [i, bit]) => (b(i) ? m | bit : m), 0);
      f32[20] = pad.buttons[6]?.value ?? 0; f32[21] = pad.buttons[7]?.value ?? 0;
      f32[22] = pad.axes[0] ?? 0; f32[23] = -(pad.axes[1] ?? 0); f32[24] = pad.axes[2] ?? 0; f32[25] = -(pad.axes[3] ?? 0);
    } else u32[18] = 0;
    requestAnimationFrame(poll);
  };
  poll();
  // scripted input for recordings (window.swKeys): times count from now, when the title opened its input
  playKeys = (list) => { for (const [ms, code, keyCode, down, label] of list) setTimeout(() => {
    if (down) held.set(code, keyCode | scan(code) << 8); else held.delete(code); keys();
    if (label && capture) post('/capture/mark', JSON.stringify({ label, time: performance.timeOrigin + performance.now() }));
  }, ms); };
  if (scripted) playKeys(scripted);
}
// window.swKeys([[ms, code, keyCode, down], ...]): key presses, played once the title reads its input
let scripted = null, playKeys = null;
window.swKeys = (list) => { scripted = list; if (playKeys) playKeys(list); };

// ---- the title's persistent storage (S:\): OPFS, one folder per title ----
let storeDir = null, storeQueue = Promise.resolve();
async function openStore(key) {
  const root = await navigator.storage.getDirectory();
  const top = await root.getDirectoryHandle('series-web', { create: true });
  storeDir = await top.getDirectoryHandle(encodeURIComponent(key), { create: true });
  const files = [];
  for await (const [name, h] of storeDir.entries()) if (h.kind === 'file') files.push({ name: decodeURIComponent(name), data: new Uint8Array(await (await h.getFile()).arrayBuffer()) });
  return files;
}
function persist(m) {
  storeQueue = storeQueue.then(async () => {
    if (!storeDir) return;
    if (m.save) { const h = await storeDir.getFileHandle(encodeURIComponent(m.save.name), { create: true }); const w = await h.createWritable(); await w.write(m.save.data); await w.close(); console.log(`storage: saved ${m.save.name} (${m.save.data.length} bytes)`); }
    else { await storeDir.removeEntry(encodeURIComponent(m.remove)).catch(() => {}); console.log(`storage: removed ${m.remove}`); }
  }).catch((e) => log(`storage: ${e.message}`));
}

bus.onmessage = (e) => {
  const m = e.data;
  if (m.out !== undefined) {
    out.textContent += m.out; line += m.out;
    if (out.textContent.length > 200000) out.textContent = out.textContent.slice(-100000);
    const parts = line.split('\n'); line = parts.pop(); for (const l of parts) console.log(l);
  }
  if (m.refill !== undefined) startPooled(m.refill);
  if (m.gpu !== undefined) {
    if (!device) { log('gpu: no WebGPU device: the title cannot draw'); return; }
    startBackend({ memory, ring: m.gpu, device, canvas, dir, log, onFrame: capture || hashFrames ? onFrame : null, keepAll: hashFrames, onShot }).then((s) => {
      window.swGpu = s;
      if (q.get('stats') === '1') setInterval(() => console.log(`gpu stats: ${JSON.stringify({ ...s, errors: s.errors.count })}`), 3000);
    });
  }
  if (m.audio !== undefined) startAudio(m.audio);
  if (m.save !== undefined || m.remove !== undefined) persist(m);
  if (m.input !== undefined) startInput(m.input);
  if (m.exit !== undefined) {
    status.textContent = `exit ${m.exit}`; document.title = `exit ${m.exit}`;
    if (line) console.log(line);
    // a title drawn through the explicit records (Direct3D 9): what the back end made of them, in one line
    if (window.swGpu?.explicit?.draws) console.log(`gpu explicit records: ${JSON.stringify(window.swGpu.explicit)}, ${window.swGpu.presents} presents, ${window.swGpu.errors.count} WebGPU errors`);
    console.log(`series-web exit ${m.exit}`);
    bus.postMessage({ stop: true });
  }
};
if (!crossOriginIsolated) { status.textContent = 'not cross-origin isolated: shared memory is unavailable'; console.log('series-web exit 125'); }
else {
  // the GPU first: what it can do is known before the title asks
  try {
    const adapter = navigator.gpu && await navigator.gpu.requestAdapter();
    if (adapter) {
      const want = ['texture-compression-bc', 'rg11b10ufloat-renderable', 'float32-filterable', 'depth32float-stencil8', 'bgra8unorm-storage'].filter((f) => adapter.features.has(f));
      device = await adapter.requestDevice({ requiredFeatures: want });
      device.lost.then((i) => log(`gpu: device lost: ${i.message}`));
      console.log(`gpu: ${adapter.info?.vendor || ''} ${adapter.info?.architecture || ''} ${adapter.info?.description || ''} features: ${want.join(' ') || 'none'}`);
    } else log('gpu: WebGPU is not available in this browser');
  } catch (err) { log(`gpu: ${err.message}`); }
  const wasm = await (await fetch(q.get('wasm'))).arrayBuffer();
  const module = await WebAssembly.compile(wasm);
  dir = new URL(q.get('exe'), location.href).href.replace(/[^/]*$/, ''); // the title's folder
  memory = new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true });
  const tids = new Int32Array(new SharedArrayBuffer(4)); tids[0] = 1;
  const args = (q.get('args') || '').split(' ').filter(Boolean);
  status.textContent = 'running';
  ctl = new Int32Array(new SharedArrayBuffer(16 * POOL));
  const env = {}; for (const [k, v] of q) if (k.startsWith('SW_')) env[k] = v;
  if (q.get('trace')) env.SW_TRACE = q.get('trace');
  if (q.get('pace') === 'off') env.SW_PACE = 'off';
  let store = [];
  try { store = await openStore(new URL(q.get('exe'), location.href).pathname); if (store.length) console.log(`storage: ${store.length} file(s) from earlier sessions`); } catch (e) { log(`storage: ${e.message} (saves will not survive a reload)`); }
  boot = { module, memory, dir, tids, args, env, store, t0: performance.timeOrigin };
  for (let i = 0; i < POOL; i++) startPooled(i);
  const w = new Worker('worker.js'); workers.push(w);
  w.postMessage({ ...boot, tid: 0, pool: { ctl, slot: -1 } });
}
