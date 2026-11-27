// SPDX-License-Identifier: GPL-2.0-or-later
// Starts a lifted title in this page: ?wasm=<title.wasm>&exe=<the title's .exe>&args=<space-separated>
// or a title image (web/image.mjs, docs/images.md): ?image=<the image's URL, served by tools/serve.mjs>&args=<more>
// The title's threads (its main thread too) run in workers; this page owns what a console would: the GPU
// (web/gpu/backend.js, on this thread), the speakers (an AudioWorklet playing the title's mixer ring), and the
// keyboard and gamepad (written into the title's input block), or for a Windows desktop title the keyboard and the
// mouse under Pointer Lock (events into its input ring).
// Options: &pace=off (no frame clock), &hash=1 (log a hash of every presented frame),
//          &capture=1 (send presented frames and the audio to the server, which records them: tools/serve.mjs),
//          &SW_<NAME>=<value>: the runtime's settings, as the Node host takes them from the environment. For
//          Windows desktop titles: SW_CLOCK=16667 (a virtual clock: that many microseconds a presented frame) and
//          SW_SESSION_STEPS=<steps> (a scripted session, as web/session.mjs writes one; tools/run-chrome.mjs
//          makes it from SW_SESSION=<session.json>). At each `shot` of a session the frame presented last is
//          sent to the server (POST /capture/shot), which saves it as a PNG.
//          &probe=<n> (log presented frame n's size, centre and corner pixels, and how many pixels differ from the
//          corner: a check that needs no image file)
import { startBackend } from '../gpu/backend.js';
import { fetchImage, blobPath } from '../image.mjs';
import { createKernel } from '../kernel.mjs';
const q = new URLSearchParams(location.search);
const status = document.getElementById('status'), out = document.getElementById('out'), canvas = document.getElementById('screen');
const bus = new BroadcastChannel('pisco');
const workers = [];
let line = '';
const POOL = 32; // guest threads come from workers the page starts ahead of time (see worker.js)
let ctl, boot, memory, device, dir, drive = null, folder = null;
// ---- the title's text: a terminal. Lines of cells, a cursor that moves (CR, LF as CR LF, BS, TAB, and the CSI
// sequences for the cursor, erasing and the SGR colours: 16, 256 and 24-bit, bold, dim, italic, underline, reverse).
// What it does not know (other escapes, OSC) is left out. A Linux title is told how wide it is (SW_TTY, below). ----
const ANSI = ['#000', '#c33', '#3a3', '#c90', '#48c', '#a4a', '#3aa', '#ccc', '#666', '#f55', '#5d5', '#fd5', '#6af', '#f6f', '#5ff', '#fff'];
const colour256 = (n) => n < 16 ? ANSI[n] : n < 232 ? `rgb(${[Math.floor((n - 16) / 36), Math.floor((n - 16) / 6) % 6, (n - 16) % 6].map((v) => v ? 55 + 40 * v : 0)})` : `rgb(${Array(3).fill(8 + 10 * (n - 232))})`;
const term = { lines: [[]], row: 0, col: 0, cols: 100000, rows: 40, wrap: true, pen: {}, style: '', held: '', saved: [0, 0], dirty: new Set(), els: [] };
function sgr(codes) {
  const pen = term.pen;
  for (let i = 0; i < codes.length; i++) {
    const c = codes[i];
    const far = (key) => { if (codes[i + 1] === 5) { pen[key] = colour256(codes[i + 2] | 0); i += 2; } else if (codes[i + 1] === 2) { pen[key] = `rgb(${codes.slice(i + 2, i + 5).map((v) => v | 0)})`; i += 4; } };
    if (c === 0) term.pen = {}; else if (c === 1) pen.bold = true; else if (c === 2) pen.dim = true; else if (c === 3) pen.italic = true; else if (c === 4) pen.underline = true;
    else if (c === 7) pen.reverse = true; else if (c === 22) { delete pen.bold; delete pen.dim; } else if (c === 23) delete pen.italic; else if (c === 24) delete pen.underline; else if (c === 27) delete pen.reverse;
    else if (c >= 30 && c <= 37) pen.fg = ANSI[c - 30]; else if (c === 38) far('fg'); else if (c === 39) delete pen.fg;
    else if (c >= 40 && c <= 47) pen.bg = ANSI[c - 40]; else if (c === 48) far('bg'); else if (c === 49) delete pen.bg;
    else if (c >= 90 && c <= 97) pen.fg = ANSI[c - 82]; else if (c >= 100 && c <= 107) pen.bg = ANSI[c - 92];
  }
  const p = term.pen, fg = p.reverse ? p.bg || '#111' : p.fg, bg = p.reverse ? p.fg || '#ddd' : p.bg;
  term.style = [fg && `color:${fg}`, bg && `background:${bg}`, p.bold && 'font-weight:bold', p.dim && 'opacity:.6', p.italic && 'font-style:italic', p.underline && 'text-decoration:underline'].filter(Boolean).join(';');
}
const lineAt = (r) => { while (term.lines.length <= r) term.lines.push([]); term.dirty.add(r); return term.lines[r]; };
function down(n = 1) { term.row += n; lineAt(term.row); }
function putChar(ch) {
  if (term.col >= term.cols) { if (term.wrap) { term.col = 0; down(); } else term.col = term.cols - 1; }
  const l = lineAt(term.row);
  while (l.length < term.col) l.push({ c: ' ', s: '' });
  l[term.col++] = { c: ch, s: term.style };
}
function csi(params, final) {
  const n = params.split(';').map((v) => parseInt(v, 10)), a = n[0] || 1, top = Math.max(0, term.lines.length - term.rows);
  switch (final) {
    case 'A': term.row = Math.max(top, term.row - a); break;
    case 'B': down(a); break;
    case 'C': term.col = Math.min(term.cols - 1, term.col + a); break;
    case 'D': term.col = Math.max(0, term.col - a); break;
    case 'E': down(a); term.col = 0; break;
    case 'F': term.row = Math.max(top, term.row - a); term.col = 0; break;
    case 'G': term.col = Math.min(term.cols - 1, a - 1); break;
    case 'H': case 'f': term.row = top + (n[0] || 1) - 1; lineAt(term.row); term.col = Math.min(term.cols - 1, (n[1] || 1) - 1); break;
    case 'K': { const l = lineAt(term.row), k = n[0] || 0; if (k === 0) l.length = Math.min(l.length, term.col); else if (k === 1) for (let i = 0; i <= term.col && i < l.length; i++) l[i] = { c: ' ', s: '' }; else l.length = 0; break; }
    case 'J': if ((n[0] || 0) >= 2) { term.lines = [[]]; term.row = term.col = 0; out.textContent = ''; term.els = []; term.dirty = new Set([0]); } else { lineAt(term.row).length = Math.min(lineAt(term.row).length, term.col); term.lines.length = term.row + 1; } break;
    case 'm': sgr(params === '' ? [0] : params.split(/[;:]/).map(Number)); break;
    case 's': term.saved = [term.row, term.col]; break;
    case 'u': [term.row, term.col] = term.saved; lineAt(term.row); break;
    case 'h': case 'l': if (params === '?7') term.wrap = final === 'h'; break;   // (?25: the cursor is not drawn)
  }
}
function print(s) {
  s = term.held + s; term.held = '';
  for (let at = 0; at < s.length;) {
    const ch = s[at];
    if (ch === '\x1b') {
      const m = /^\x1b(?:\[([0-9;:?<=>]*)[ -\/]*([@-~])|\][^\x07\x1b]*(?:\x07|\x1b\\)|[()][0-9A-Za-z]|[@-Z\\^_`a-z{|}~78=>])/.exec(s.slice(at, at + 256));
      if (!m) { if (s.length - at < 256) { term.held = s.slice(at); break; } at++; continue; }   // (not complete yet: kept for the next write)
      if (m[2]) csi(m[1], m[2]);
      at += m[0].length; continue;
    }
    if (ch === '\n') { term.col = 0; down(); } else if (ch === '\r') term.col = 0; else if (ch === '\b') term.col = Math.max(0, term.col - 1);
    else if (ch === '\t') term.col = Math.min(term.cols - 1, (term.col >> 3 << 3) + 8);
    else if (ch >= ' ') { const cp = s.codePointAt(at); const g = String.fromCodePoint(cp); putChar(g); at += g.length; continue; }
    at++;
  }
  render();
}
let pending = false;
function render() {
  if (pending) return; pending = true;
  requestAnimationFrame(() => {
    pending = false;
    // at most so many lines are kept: the oldest go
    const MAX = 20000;
    if (term.lines.length > MAX) { const drop = term.lines.length - MAX; term.lines.splice(0, drop); term.els.splice(0, drop).forEach((e) => e.remove()); term.row -= drop; term.dirty = new Set([...term.dirty].map((r) => r - drop).filter((r) => r >= 0)); }
    for (const r of term.dirty) {
      while (term.els.length <= r) { const e = document.createElement('div'); out.append(e); term.els.push(e); }
      const e = term.els[r], l = term.lines[r] || []; e.textContent = '';
      for (let i = 0; i < l.length;) {
        let j = i, text = ''; while (j < l.length && l[j].s === l[i].s) text += l[j++].c;
        if (l[i].s) { const span = document.createElement('span'); span.style.cssText = l[i].s; span.textContent = text; e.append(span); } else e.append(text);
        i = j;
      }
      if (!l.length) e.append('​');   // (an empty line keeps its height)
    }
    term.dirty.clear();
    if (document.body.classList.contains('text')) scrollTo(0, document.body.scrollHeight);
  });
}
const log = (s) => { console.log(s); print(s + '\n'); };
const startPooled = (slot) => { const w = new Worker('worker.js', { type: 'module' }); workers.push(w); w.postMessage({ ...boot, pool: { ctl, slot } }); };
const capture = q.get('capture') === '1', hashFrames = q.get('hash') === '1', clockStep = +(q.get('SW_CLOCK') || 0), probe = q.get('probe');
const post = (path, body, headers = {}) => fetch(path, { method: 'POST', body, headers }).catch(() => {});

// ---- presented frames: hashes for tests, frames for recordings ----
function onFrame(f) {
  if (probe !== null && String(f.index) === probe) {
    const p = f.pixels, bgra = f.format.startsWith('bgra'), px = (x, y) => { const o = 4 * (y * f.width + x); return bgra ? [p[o + 2], p[o + 1], p[o], p[o + 3]] : [p[o], p[o + 1], p[o + 2], p[o + 3]]; };
    const corner = px(0, 0); let differ = 0;
    for (let i = 0; i < p.length; i += 4) if (p[i] !== p[0] || p[i + 1] !== p[1] || p[i + 2] !== p[2]) differ++;
    console.log(`probe frame ${f.index}: ${f.width}x${f.height} ${f.format} centre ${px(f.width >> 1, f.height >> 1)} corner ${corner} differ ${differ}`);
  }
  if (hashFrames) {
    let h = 2166136261 >>> 0; const p = f.pixels, bgra = f.format.startsWith('bgra');
    for (let i = 0; i < p.length; i += 4) for (const k of bgra ? [0, 1, 2] : [2, 1, 0]) h = Math.imul(h ^ p[i + k], 16777619) >>> 0;
    console.log(`gpu frame ${f.index} ${h.toString(16).padStart(8, '0')}`);
  }
  // wall-clock times (they continue across a reload of the page); with a virtual clock (SW_CLOCK: microseconds a
  // frame) the title's: frame n shows from (n + 1) frames on, and the sound is placed on the same clock (startAudio)
  const time = clockStep ? performance.timeOrigin + (f.index + 1) * clockStep / 1000 : performance.timeOrigin + f.time;
  if (capture) post('/capture/frame', f.pixels, { 'x-frame': JSON.stringify({ index: f.index, time, width: f.width, height: f.height, format: f.format }) });
}
// a session's checkpoint: the frame it names, to the server; and how fast frames came since the one before
let lastShot = { index: 0, time: performance.now() };
async function onShot(f) {
  const now = performance.now(), frames = f.index + 1 - lastShot.index, s = (now - lastShot.time) / 1000;
  const reply = await fetch('/capture/shot', { method: 'POST', body: f.pixels, headers: { 'x-shot': JSON.stringify({ name: f.name, width: f.width, height: f.height, format: f.format }) } }).then((r) => r.text()).catch((e) => e.message);
  console.log(`shot ${f.name}: frame ${f.index + 1} at ${(now / 1000).toFixed(1)} s, ${reply}; ${frames} frames since the last in ${s.toFixed(1)} s (${(frames / s).toFixed(1)} a second)`);
  lastShot = { index: f.index + 1, time: performance.now() };
}
// ---- audio: the title's mixer ring (rt/gdk/xaudio2.c) -> an AudioWorklet; recordings read the same ring ----
async function startAudio(addr) {
  let ctx;
  try {
    ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ctx, 'pisco-ring', { outputChannelCount: [2] });
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
      // the mixer runs `lead` frames ahead of the clock: the chunk's first frame plays at now + (lead - n) / rate.
      // A mix that follows a virtual clock (header word 5) says where its frame 0 is on that clock (words 6, 7)
      const time = u32[5] ? performance.timeOrigin + (u32[6] + u32[7] * 2 ** 32) / 1000 + read / rate * 1000 : performance.timeOrigin + performance.now() + (lead - n) / rate * 1000;
      read = w;
      post('/capture/audio', chunk.buffer, { 'x-audio': JSON.stringify({ time, rate }) });
    }, 100);
  }
}
// ---- input: keys held and the first gamepad, into the title's input block (rt/gdk/gameinput.c) ----
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
// ---- a machine's console (rt/microvm-x64/devices.c): the keys typed, as a terminal sends them, into its ring ----
function startConsole(addr) {
  const u32 = new Uint32Array(memory.buffer, addr, 4), i32 = new Int32Array(memory.buffer, addr, 4), data = new Uint8Array(memory.buffer, addr + 16, u32[2]);
  const put = (s) => { for (const b of new TextEncoder().encode(s)) { if (u32[0] - Atomics.load(u32, 1) >= u32[2]) break; data[u32[0] % u32[2]] = b; Atomics.store(u32, 0, u32[0] + 1); } Atomics.notify(i32, 0); };
  const seq = { Enter: '\r', Backspace: '\x7f', Tab: '\t', Escape: '\x1b', ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
    Home: '\x1b[H', End: '\x1b[F', Delete: '\x1b[3~', PageUp: '\x1b[5~', PageDown: '\x1b[6~' };
  addEventListener('keydown', (e) => {
    let s = seq[e.key];
    if (!s && e.ctrlKey && e.key.length === 1 && /[a-z@\[\\\]^_]/i.test(e.key)) s = String.fromCharCode(e.key.toUpperCase().charCodeAt(0) & 31);   // Ctrl-C: 0x03
    else if (!s && !e.ctrlKey && !e.metaKey && e.key.length === 1) s = e.key;
    if (s) { put(s); e.preventDefault(); }
  });
  addEventListener('paste', (e) => { put(e.clipboardData.getData('text').replace(/\r?\n/g, '\r')); e.preventDefault(); });
}
// ---- input for a Windows desktop title (rt/windows-x86/input.c): the keyboard as DirectInput scan codes, the mouse as
// relative motion under Pointer Lock (a click on the canvas takes the pointer; Escape gives it back, as browsers
// do, and is passed on to the title as its Escape key) ----
const DIK = { Escape: 0x01, Minus: 0x0C, Equal: 0x0D, Backspace: 0x0E, Tab: 0x0F, BracketLeft: 0x1A, BracketRight: 0x1B, Enter: 0x1C, ControlLeft: 0x1D,
  Semicolon: 0x27, Quote: 0x28, Backquote: 0x29, ShiftLeft: 0x2A, Backslash: 0x2B, Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
  NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3A, NumLock: 0x45, ScrollLock: 0x46, Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49,
  NumpadSubtract: 0x4A, Numpad4: 0x4B, Numpad5: 0x4C, Numpad6: 0x4D, NumpadAdd: 0x4E, Numpad1: 0x4F, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52,
  NumpadDecimal: 0x53, IntlBackslash: 0x56, F11: 0x57, F12: 0x58, NumpadEnter: 0x9C, ControlRight: 0x9D, NumpadDivide: 0xB5, PrintScreen: 0xB7,
  AltRight: 0xB8, Pause: 0xC5, Home: 0xC7, ArrowUp: 0xC8, PageUp: 0xC9, ArrowLeft: 0xCB, ArrowRight: 0xCD, End: 0xCF, ArrowDown: 0xD0,
  PageDown: 0xD1, Insert: 0xD2, Delete: 0xD3, MetaLeft: 0xDB, MetaRight: 0xDC, ContextMenu: 0xDD };
'1234567890'.split('').forEach((d, i) => { DIK[`Digit${d}`] = 0x02 + i; });
for (const [row, first] of [['QWERTYUIOP', 0x10], ['ASDFGHJKL', 0x1E], ['ZXCVBNM', 0x2C]]) row.split('').forEach((k, i) => { DIK[`Key${k}`] = first + i; });
for (let i = 1; i <= 10; i++) DIK[`F${i}`] = 0x3A + i;
function startWin32Input(addr) {
  const i32 = new Int32Array(memory.buffer, addr, 4 + 4 * 512), cap = i32[2], held = new Set();
  const push = (kind, a, b) => {
    const w = Atomics.load(i32, 0); if (w - Atomics.load(i32, 1) >= cap) return;   // the title has not read for a while: dropped
    const at = 4 + 4 * (w % cap); i32[at] = kind; i32[at + 1] = a; i32[at + 2] = b; Atomics.store(i32, 0, w + 1);
  };
  const key = (code, down) => { const scan = DIK[code]; if (!scan || held.has(code) === down) return false; if (down) held.add(code); else held.delete(code); push(1, scan, down ? 1 : 0); return true; };
  addEventListener('keydown', (e) => { if (DIK[e.code]) e.preventDefault(); if (!e.repeat) key(e.code, true); });
  addEventListener('keyup', (e) => { key(e.code, false); });
  addEventListener('blur', () => { for (const code of [...held]) key(code, false); });
  const locked = () => document.pointerLockElement === canvas;
  canvas.addEventListener('click', () => { if (!locked()) canvas.requestPointerLock(); });
  let wasLocked = false;
  document.addEventListener('pointerlockchange', () => { if (wasLocked && !locked()) { push(1, 0x01, 1); push(1, 0x01, 0); } wasLocked = locked(); });
  addEventListener('mousemove', (e) => { if (locked() && (e.movementX || e.movementY)) push(2, e.movementX, e.movementY); });
  const button = { 0: 0, 1: 2, 2: 1, 3: 3, 4: 4 };   // the browser's left, middle, right, back, forward -> DirectInput's 0 left, 1 right, 2 middle
  canvas.addEventListener('mousedown', (e) => { if (locked()) { push(3, button[e.button] ?? 0, 1); e.preventDefault(); } });
  addEventListener('mouseup', (e) => { if (locked()) push(3, button[e.button] ?? 0, 0); });
  canvas.addEventListener('wheel', (e) => { if (locked()) { push(4, e.deltaY < 0 ? 120 : -120, 0); e.preventDefault(); } }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
}
// window.swKeys([[ms, code, keyCode, down], ...]): key presses, played once the title reads its input
let scripted = null, playKeys = null;
window.swKeys = (list) => { scripted = list; if (playKeys) playKeys(list); };

// ---- the title's persistent storage (S:\): OPFS, one folder per title ----
let storeDir = null, storeQueue = Promise.resolve();
async function openStore(key) {
  const root = await navigator.storage.getDirectory();
  const top = await root.getDirectoryHandle('pisco', { create: true });
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
    print(m.out); line += m.out;
    const parts = line.split('\n'); line = parts.pop(); for (const l of parts) console.log(l);
  }
  if (m.refill !== undefined) startPooled(m.refill);
  if (m.gpu !== undefined) {
    if (!device) { log('gpu: no WebGPU device: the title cannot draw'); return; }
    startBackend({ memory, ring: m.gpu, device, canvas, dir, log, onFrame: capture || hashFrames || probe !== null ? onFrame : null, keepAll: hashFrames || probe !== null, onShot }).then((s) => {
      window.swGpu = s;
      if (q.get('stats') === '1') setInterval(() => console.log(`gpu stats: ${JSON.stringify({ ...s, errors: s.errors.count })}`), 3000);
    });
  }
  if (m.audio !== undefined) startAudio(m.audio);
  if (m.save !== undefined || m.remove !== undefined) persist(m);
  if (m.input !== undefined) startInput(m.input);
  if (m.win32input !== undefined) startWin32Input(m.win32input);
  if (m.console !== undefined) startConsole(m.console);
  if (m.exit !== undefined) {
    status.textContent = `exit ${m.exit}`; document.title = `exit ${m.exit}`;
    if (line) console.log(line);
    // a title drawn through the explicit records (Direct3D 9): what the back end made of them, in one line
    if (window.swGpu?.explicit?.draws) console.log(`gpu explicit records: ${JSON.stringify(window.swGpu.explicit)}, ${window.swGpu.presents} presents, ${window.swGpu.errors.count} WebGPU errors`);
    // what the title stored last is written to OPFS first (a title that saves as it quits: Narbacular Drop's system.opt)
    storeQueue.then(() => { console.log(`pisco exit ${m.exit}`); bus.postMessage({ stop: true }); });
  }
};
if (!crossOriginIsolated) { status.textContent = 'not cross-origin isolated: shared memory is unavailable'; console.log('pisco exit 125'); }
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
  // a title image: its module, its drive (the file layers, served below <image>/c/) and its settings (exe, registry)
  const image = q.get('image') ? await fetchImage(new URL(q.get('image'), location.href).href).catch((e) => { log(`image: ${e.message}`); return null; }) : null;
  if (q.get('image') && !image) { console.log('pisco exit 125'); throw new Error('no image'); }
  const module = await WebAssembly.compile(image ? image.wasm : await (await fetch(q.get('wasm'))).arrayBuffer());
  if (image) { drive = image.drive; folder = image.settings.folder; dir = drive + folder.split('/').map(encodeURIComponent).join('/') + '/'; console.log(`image: ${image.settings.title} (manifest ${image.digest})`); }
  else dir = new URL(q.get('exe'), location.href).href.replace(/[^/]*$/, ''); // the title's folder
  memory = new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true });
  const tids = new Int32Array(new SharedArrayBuffer(4)); tids[0] = 1;
  const given = (q.get('args') || '').split(' ').filter(Boolean), args = given.length || !image ? given : image.settings.args;   // (arguments given replace the image's)
  status.textContent = 'running';
  ctl = new Int32Array(new SharedArrayBuffer(16 * POOL));
  const env = { ...(image ? image.settings.env : {}) }; for (const [k, v] of q) if (k.startsWith('SW_')) env[k] = k === 'SW_REGISTRY' && env[k] ? `${env[k]}\n${v}` : v;
  if (q.get('trace')) env.SW_TRACE = q.get('trace');
  // a Linux title: the page is its terminal, as wide as the text area (&SW_TTY=0: its output is a pipe instead)
  if (image?.settings.text) {
    document.body.classList.add('text');
    const probe1 = document.createElement('span'); probe1.textContent = 'M'.repeat(50); out.append(probe1); const w = probe1.getBoundingClientRect().width / 50; probe1.remove();
    if (env.SW_TTY === undefined) env.SW_TTY = `${Math.max(40, Math.min(240, Math.floor(out.clientWidth / (w || 8.4))))}x40`;
    const [cols, rows] = env.SW_TTY.split('x').map(Number); if (cols && rows) { term.cols = cols; term.rows = rows; }
  }
  if (q.get('pace') === 'off') env.SW_PACE = 'off';
  if (env.SW_CONSOLE === undefined) env.SW_CONSOLE = 'tty';   // a machine's console: someone may type (rt/microvm-x64)
  let store = [];
  try { store = await openStore(image ? `image:${image.settings.title}` : new URL(q.get('exe'), location.href).pathname); if (store.length) console.log(`storage: ${store.length} file(s) from earlier sessions`); } catch (e) { log(`storage: ${e.message} (saves will not survive a reload)`); }
  // a Linux title: its processes (web/kernel.mjs), each a worker this page starts (the first, a fork's, an execve's)
  const kernel = WebAssembly.Module.exports(module).some((e) => e.name === 'rt_fork_resume') ? createKernel() : null, programs = image?.config.programs || null;
  // a machine's programs in modules of their own (rt/microvm-x64): fetched here, loaded by the worker (web/side.mjs)
  const side = image?.config.side ? await Promise.all(Object.entries(image.config.side).map(async ([n, d]) => [n, await (await fetch(image.url + blobPath(d))).arrayBuffer()])) : [];
  boot = { module, memory, dir, drive, folder, tids, args, env, store, t0: performance.timeOrigin, kernel, programs, side };
  for (let i = 0; i < POOL; i++) startPooled(i);
  const compiled = new Map();
  const moduleOf = (digest) => { if (!compiled.has(digest)) compiled.set(digest, fetch(image.url + blobPath(digest)).then((r) => r.arrayBuffer()).then((b) => WebAssembly.compile(b))); return compiled.get(digest); };
  const startProcess = (data) => {
    const w = new Worker('worker.js', { type: 'module' }); workers.push(w);
    w.onmessage = async (e) => {
      const m = e.data, one = () => new Int32Array(new SharedArrayBuffer(4)).fill(1);
      if (m.fork) startProcess({ ...boot, module: m.fork.module, memory: m.fork.memory, pid: m.fork.pid, mode: 'fork', tids: one(), args: [], tid: 0, pool: { ctl, slot: -1 } });
      if (m.exec) {
        const mod = await moduleOf(m.exec.program.blob);
        startProcess({ ...boot, module: mod, memory: new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true }), pid: m.exec.pid, mode: 'exec', tids: one(),
          args: m.exec.args, env: { ...env, ...m.exec.settings }, tid: 0, pool: { ctl, slot: -1 } });
        w.terminate();
      }
    };
    w.postMessage(data);
  };
  startProcess({ ...boot, tid: 0, pid: 1, mode: 'start', pool: { ctl, slot: -1 } });
}
