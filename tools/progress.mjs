// SPDX-License-Identifier: GPL-2.0-or-later
// node tools/progress.mjs [--refresh] [--out <dir>]: the progress map (progress.svg, progress.json, badges) — what
// the runtime implements, measured against what real titles use.
//   --refresh  re-reads the build outputs (the lifter's import tables of our titles in out/, XCL's test-title run)
//              into docs/progress-data.json, which is committed; without it the map is drawn from that file and
//              from the runtime's sources, so CI can draw it without building anything.
// Counted as done: an import or vtable method with a runtime function behind it that does more than return
// E_NOTIMPL; an import resolves as the runtime resolves it (API sets, kernelx -> kernel32, …, rt/process.c).
import fs from 'node:fs'; import path from 'node:path';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const argv = process.argv.slice(2);
const out = argv.includes('--out') ? path.resolve(argv[argv.indexOf('--out') + 1]) : path.join(root, 'out/progress');
const dataPath = path.join(root, 'docs/progress-data.json');

// ---- the runtime: every IMP(dll, name) and whether it does something ----
const impls = new Map(); // "dll.dll!name" -> true when it does more than return E_NOTIMPL
for (const f of fs.readdirSync(path.join(root, 'rt')).filter((f) => f.endsWith('.c'))) {
  const src = fs.readFileSync(path.join(root, 'rt', f), 'utf8');
  for (const m of src.matchAll(/^IMP\((\w+), *(\w+)\) *\{(.*)$/gm)) {
    const stub = /^\s*RET\((E_NOTIMPL|0x80004001u?)\);\s*\}/.test(m[3]);
    impls.set(`${m[1]}.dll!${m[2]}`, !stub);
  }
}
const dataImports = new Set(['msvcrt.dll!__initenv', 'msvcrt.dll!_commode', 'msvcrt.dll!_fmode']);
// rt/process.c's find_import, in JavaScript
function resolves(full) {
  if (dataImports.has(full)) return true;
  const has = (n) => impls.get(n) === true;
  if (impls.has(full)) return has(full);
  const [dll, name] = full.split('!');
  const crt = dll.startsWith('api-ms-win-crt-') || ['ucrtbase.dll', 'vcruntime140.dll', 'msvcrt.dll'].includes(dll);
  const core = dll.startsWith('api-ms-win-core-') || ['kernelbase.dll', 'ntdll.dll'].includes(dll) || dll.startsWith('ext-ms-win-');
  const alts = [];
  if (crt) alts.push(`ucrtbase.dll!${name}`, `msvcrt.dll!${name}`);
  if (core) alts.push(`kernel32.dll!${name}`);
  if (dll === 'xmem.dll') alts.push(`kernelx.dll!${name}`);
  if (dll === 'kernelx.dll' || dll === 'xmem.dll' || dll.includes('legacy.dll')) alts.push(`kernel32.dll!${name}`, `advapi32.dll!${name}`);
  if (dll.startsWith('ext-ms-win-rtcore-ntuser') || dll.startsWith('ext-ms-win-ntuser') || dll.startsWith('minuser')) alts.push(`user32.dll!${name}`);
  if (dll === 'advapi32legacy.dll' || dll.startsWith('api-ms-win-security')) alts.push(`advapi32.dll!${name}`);
  return alts.some(has);
}
// the DLL a title's import is counted under (API sets by what they stand for)
function group(dll) {
  if (dll.startsWith('api-ms-win-crt-') || dll === 'ucrtbase.dll') return 'ucrt';
  if (dll.startsWith('api-ms-win-core-') || ['kernelbase.dll', 'kernel32.dll'].includes(dll)) return 'kernel32';
  if (dll.startsWith('ext-ms-win-') || dll === 'minuser.dll') return 'user32';
  return dll.replace(/\.dll$/, '');
}

// ---- --refresh: the build outputs into docs/progress-data.json ----
if (argv.includes('--refresh')) {
  const titles = {
    'Doom (our console title)': 'out/doom/wasm/title.c',
    "XCL's console test title": 'out/regress-xcl/title.c',
    'Stockfish 19 (official build)': 'out/regress-sf/title.c',
    'SimpleTriangleDesktop (clang-cl)': 'out/st-local/title.c',   // tests/gfx/st.sh builds both
    'SimpleTriangleDesktop (MSVC)': 'out/st-boat-msvc/title.c',
    'SimplePBR (clang-cl)': 'out/pbr/title.c',                    // tests/gfx/pbr.sh
  };
  const old = fs.existsSync(dataPath) ? JSON.parse(fs.readFileSync(dataPath, 'utf8')) : {};
  const data = { updated: new Date().toISOString().slice(0, 10), titles: old.titles || {}, xcl: old.xcl || [], ladder: old.ladder, facts: old.facts };
  for (const [name, file] of Object.entries(titles)) {
    const f = path.isAbsolute(file) ? file : path.join(root, file);
    if (!fs.existsSync(f)) { console.log(`progress: ${name}: ${file} not built, kept from before`); continue; }
    const c = fs.readFileSync(f, 'utf8');
    const names = JSON.parse('[' + c.match(/import_names\[\]=\{(.*?)0\};/)[1].replace(/,$/, '') + ']');
    const targets = c.match(/import_targets\[\]=\{(.*?)0\};/)[1].split(',').filter(Boolean).map((t) => BigInt(t.replace(/ull$/, '')));
    data.titles[name] = [...new Set(names.filter((n, i) => !targets[i]))].sort(); // imports between the title's own images are not the runtime's
  }
  const xclSrc = path.join(process.env.XCL || path.join(root, '../xcl'), 'tests/testtitle/src'); // an XCL checkout
  // the Chrome run (tests/gfx/xcl.sh: the GPU checks read pixels back from WebGPU), else the Node one
  const run = ['out/regress-xcl.chrome.log', 'out/regress-xcl.run.log'].map((f) => path.join(root, f)).find((f) => fs.existsSync(f));
  if (fs.existsSync(xclSrc) && run) {
    const checks = [];
    for (const f of ['main.rs']) for (const m of fs.readFileSync(path.join(xclSrc, f), 'utf8').matchAll(/check\("([^"]*)"/g)) checks.push(m[1].replace(/\\\\/g, "\\"));
    const log = fs.readFileSync(run, 'utf8').split('\n');
    data.xcl = checks.map((t) => ({ check: t, pass: log.some((l) => l === `[PASS] ${t}`) }));
  }
  // what the graphics checks measured (tests/gfx/*.sh); kept from before when a check has not run
  const fact = (label, value, ok) => { data.facts = (data.facts || []).filter((f) => f.label !== label); data.facts.push({ label, value, ok }); };
  const logOf = (f) => { const p = path.join(root, f); return fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : null; };
  const st = logOf('out/st-local.chrome.log'), stm = logOf('out/st-boat-msvc.chrome.log'), soak = logOf('out/st-local.soak.log'), eh = logOf('out/msvc-eh.run.log');
  const want = 'centre 187,137,137,255 corner 65,65,65,255 differ 259200';
  if (st && stm) fact('SimpleTriangle pixels (clang-cl, MSVC)', `${[st, stm].filter((l) => l.includes(want)).length} / 2 builds exact`, st.includes(want) && stm.includes(want));
  if (soak) { const h = soak.split('\n').filter((l) => l.startsWith('gpu frame')).map((l) => l.split(' ')[3]); fact('SimpleTriangle soak', `${h.length} frames, ${new Set(h).size} hash`, h.length >= 600 && new Set(h).size === 1); }
  if (eh) { const n = (eh.match(/^\[PASS\]/gm) || []).length; fact('MSVC C++ exceptions (real vcruntime140)', `${n} / 6`, n === 6); }
  const pbr = logOf('out/pbr.chrome.log');
  if (pbr) {
    const h = pbr.split('\n').filter((l) => l.startsWith('gpu frame')).map((l) => l.split(' ')[3]);
    const errs = (pbr.match(/WebGPU error|page exception|has no translation/g) || []).length;
    fact('SimplePBR in Chrome', `${h.length} frames, ${new Set(h.slice(5)).size} hash, ${errs} errors`, h.length >= 30 && new Set(h.slice(5)).size === 1 && errs === 0);
  }
  data.ladder ??= [
    { step: 'SimpleTriangle', note: 'desktop build; shaders translate, CPU bring-up to do', state: 'todo' },
    { step: 'SimplePBR', note: 'desktop build', state: 'todo' },
    { step: 'Doom', note: 'console title: D3D12.X, XAudio2, GameInput, saves', state: 'done' },
    { step: "XCL's graphics checks", note: '19 D3D12.X / audio / storage checks', state: 'todo' },
    { step: 'Minecraft', note: 'waits for the files', state: 'todo' },
  ];
  data.facts ??= [
    { label: 'Stockfish 19 bench', value: '2,497,913 nodes = published', ok: true },
    { label: 'Doom frames vs native', value: '7,123 / 7,123 (WebGPU)', ok: true },
  ];
  fs.writeFileSync(dataPath, JSON.stringify(data, null, 1) + '\n');
  console.log(`progress: ${dataPath} refreshed`);
}

// ---- measures ----
const data = JSON.parse(fs.readFileSync(dataPath, 'utf8'));
const layout = fs.readFileSync(path.join(root, 'rt/d3d12_layout.gen.h'), 'utf8');
const d3d = [];
for (const m of layout.matchAll(/\{"(\w+)", "(\w+)", (\d+), (\w+)_methods/g)) {
  const methods = layout.match(new RegExp(`${m[1]}_methods\\[\\] = \\{([^}]*)\\}`))[1].match(/"(\w+)"/g).map((s) => s.slice(1, -1));
  const done = methods.map((x) => impls.get(`d3d12.dll!${m[1]}_${x}`) === true || impls.get(`d3d12.dll!Object_${x}`) === true);
  d3d.push({ name: m[2], items: methods.map((x, i) => ({ name: x, done: done[i] })) });
}
// the console's own methods we know of (XCL's findings), by slot
const consoleMethods = [['Device #44 CreatePlacedResourceX', 'Device_CreatePlacedResourceX'], ['Device #48 CreateCommandQueueX', 'Device_CreateCommandQueueX'], ['Device #52 SetDebugFlagsX', 'Device_SetDebugFlagsX'],
  ['Device #55 CreateDerivedPipelineStateX', 'Device_CreateDerivedPipelineStateX'], ['Device #56 SerializePipelineStateX', 'Device_SerializePipelineStateX'], ['Device #60 CreateGraphicsPipelineStateX', 'Device_CreateGraphicsPipelineStateX'],
  ['Device #73 CreateCommandListX', 'Device_CreateCommandListX'], ['Device #74 RegisterPagePoolX', 'Device_RegisterPagePoolX'], ['Device #75 UnregisterPagePoolX', 'Device_UnregisterPagePoolX'],
  ['Device #78 SetFrameIntervalX', 'Device_SetFrameIntervalX'], ['Device #79 ScheduleFrameEventX', 'Device_ScheduleFrameEventX'], ['Device #80 WaitFrameEventX', 'Device_WaitFrameEventX'],
  ['Device #81 GetFrameStatisticsX', 'Device_GetFrameStatisticsX'], ['Device #86 CreatePipelineState'], ['Queue #23 SignalX', 'CommandQueue_SignalX'], ['Queue #30 PresentX', 'CommandQueue_PresentX'],
  ['List #60 CopyMemoryToMemoryX', 'CommandList_CopyMemoryToMemoryX'], ['List #61 FillMemoryWith32BitValueX', 'CommandList_FillMemoryWith32BitValueX'], ['List #69 Write32BitValueBottomOfPipeX', 'CommandList_Write32BitValueBottomOfPipeX'],
  ['List #70 Write64BitValueBottomOfPipeX', 'CommandList_Write64BitValueBottomOfPipeX'], ['List #71 Wait32BitValueX', 'CommandList_Wait32BitValueX'], ['List #72 Wait64BitValueX', 'CommandList_Wait64BitValueX'],
  ['List #81 DrawIndexedX', 'CommandList_DrawIndexedX'], ['List #89 Write64BitValueTopOfPipeX', 'CommandList_Write64BitValueTopOfPipeX'], ['List #93 KickoffX', 'CommandList_KickoffX'],
  ['List #94 RSSetDepthBiasX', 'CommandList_RSSetDepthBiasX'], ['List #146 UAV barrier', 'CommandList_UavBarrierX'], ['List #148 AllocateUploadMemory', 'CommandList_AllocateUploadMemoryX'],
  ['DMA list (copy queue)', 'Device_CreateCommandListX']];
d3d.push({ name: 'D3D12.X console methods', items: consoleMethods.map(([n, f]) => ({ name: n, done: !!f && impls.get(`d3d12.dll!${f}`) === true })) });
const union = new Map();
for (const list of Object.values(data.titles)) for (const n of list) union.set(n, resolves(n));
const byDll = new Map();
for (const [n, ok] of union) { const g = group(n.split('!')[0]); if (!byDll.has(g)) byDll.set(g, []); byDll.get(g).push({ name: n.split('!')[1], done: ok }); }
const SHIPPED = new Set(['msvcp140', 'vcruntime140_1', 'msvcp140_1', 'msvcp140_2', 'concrt140']);
const dlls = [...byDll].map(([name, items]) => ({ name, lifted: SHIPPED.has(name), items: items.sort((a, b) => b.done - a.done) }));
const runtimeDlls = dlls.filter((g) => !g.lifted);
const shipped = (n) => SHIPPED.has(group(n.split('!')[0]));
const titleBars = Object.entries(data.titles).map(([name, list]) => ({ name, done: list.filter((n) => !shipped(n) && resolves(n)).length, total: list.filter((n) => !shipped(n)).length, shipped: list.filter(shipped).length }));
const xclGroups = new Map();
for (const c of data.xcl) { const g = c.check.split(':')[0]; if (!xclGroups.has(g)) xclGroups.set(g, []); xclGroups.get(g).push({ name: c.check, done: c.pass }); }
const xcl = [...xclGroups].map(([name, items]) => ({ name, items }));
const sum = (groups) => groups.reduce((a, g) => [a[0] + g.items.filter((i) => i.done).length, a[1] + g.items.length], [0, 0]);

// ---- drawing ----
const W = 1000, BG = '#0d1117', DONE = '#2ea043', TODO = '#6e7681', PART = '#d29922', TEXT = '#e6edf3', DIM = '#8b949e';
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const pct = (d, t) => (t ? (100 * d / t).toFixed(1) : '0.0');
const label = (x, y, s, size = 16, fill = TEXT, weight = 600, anchor = 'start') => `<text x="${x}" y="${y}" font-size="${size}" font-weight="${weight}" fill="${fill}" text-anchor="${anchor}" stroke="${BG}" stroke-width="3" paint-order="stroke">${esc(s)}</text>`;
// squarified treemap of weights into a rectangle
function squarify(items, x, y, w, h) {
  const total = items.reduce((s, i) => s + i.weight, 0), out = [];
  let rest = items.map((i) => ({ ...i, area: i.weight / total * w * h })).sort((a, b) => b.area - a.area);
  while (rest.length) {
    const short = Math.min(w, h); let row = [], best = Infinity;
    for (const it of rest) {
      const r = [...row, it], s = r.reduce((a, b) => a + b.area, 0);
      const worst = Math.max(...r.map((q) => Math.max(short * short * q.area / (s * s), s * s / (short * short * q.area))));
      if (worst > best) break; best = worst; row = r;
    }
    rest = rest.slice(row.length);
    const s = row.reduce((a, b) => a + b.area, 0), thick = s / short;
    let off = 0;
    for (const it of row) { const len = it.area / thick; out.push(w >= h ? { ...it, x, y: y + off, w: thick, h: len } : { ...it, x: x + off, y, w: len, h: thick }); off += len; }
    if (w >= h) { x += thick; w -= thick; } else { y += thick; h -= thick; }
  }
  return out;
}
function tile(t) {
  const n = t.items.length, done = t.items.filter((i) => i.done).length, pad = 3;
  const iw = t.w - 2 * pad, ih = t.h - 2 * pad - 20;
  let s = Math.floor(Math.sqrt(Math.max(1, iw * Math.max(1, ih)) / n));
  while (s > 2 && Math.floor(iw / s) * Math.floor(Math.max(1, ih) / s) < n) s--;
  const cols = Math.max(1, Math.floor(iw / s));
  let g = `<g><title>${esc(`${t.name}: ${done}/${n} (${pct(done, n)}%)`)}</title><rect x="${t.x.toFixed(1)}" y="${t.y.toFixed(1)}" width="${t.w.toFixed(1)}" height="${t.h.toFixed(1)}" fill="#161b22" stroke="${BG}" stroke-width="2"/>`;
  t.items.forEach((it, i) => {
    const cx = t.x + pad + (i % cols) * s, cy = t.y + pad + 20 + Math.floor(i / cols) * s;
    g += `<rect x="${cx.toFixed(1)}" y="${cy.toFixed(1)}" width="${Math.max(1, s - 1)}" height="${Math.max(1, s - 1)}" fill="${t.lifted ? PART : it.done ? DONE : TODO}"><title>${esc(it.name)}</title></rect>`;
  });
  const full = t.lifted ? `${t.name} (recompiled with the title)` : `${t.name} ${done}/${n}`, fit = Math.floor((t.w - 8) / 6.6);
  const name = full.length <= fit ? full : t.name.length <= fit ? t.name : fit >= 4 ? t.name.slice(0, fit - 1) + '…' : '';
  if (name) g += label(t.x + 5, t.y + 16, name, 12, TEXT, 600);
  return g + '</g>';
}
function panel(y, title, groups, h, x = 10, w = W - 20) {
  const [d, t] = sum(groups.filter((g) => !g.lifted));
  let s = label(x, y + 20, `${title}: ${pct(d, t)}% (${d}/${t})`);
  for (const r of squarify(groups.map((g) => ({ ...g, weight: g.items.length + 3 })), x, y + 30, w, h)) s += tile(r);
  return s;
}
let svg = '', y = 10;
svg += label(10, y + 24, 'PISCO: Xbox titles in the browser', 24, TEXT, 700);
svg += label(W - 10, y + 24, `updated ${data.updated}`, 13, DIM, 400, 'end');
y += 40;
// facts and the graphics ladder
let fx = 10;
// one chip per fact, wrapped onto more rows when they do not fit
const chip = (text, fill) => {
  const w = text.length * 7.2 + 24;
  if (fx > 10 && fx + w > W - 10) { fx = 10; y += 34; }
  svg += `<rect x="${fx}" y="${y}" width="${w}" height="26" rx="13" fill="${fill}"/>` + label(fx + 12, y + 18, text, 13, TEXT, 500); fx += w + 10;
};
for (const f of data.facts || []) chip(`${f.label}: ${f.value}`, f.ok ? '#1f6f3a' : '#6e4a0b');
const [xd, xt] = sum(xcl);
chip(`XCL console checks: ${xd}/${xt}`, '#30363d');
y += 40;
svg += label(10, y + 18, 'Graphics ladder', 16);
y += 26;
const steps = data.ladder || [], sw = (W - 20 - 8 * (steps.length - 1)) / steps.length;
steps.forEach((st, i) => {
  const x = 10 + i * (sw + 8), fill = st.state === 'done' ? DONE : st.state === 'doing' ? PART : '#30363d';
  svg += `<g><title>${esc(`${st.step}: ${st.note}`)}</title><rect x="${x}" y="${y}" width="${sw}" height="52" rx="6" fill="${fill}"/>` + label(x + 10, y + 21, `${i + 1}. ${st.step}`, 14, TEXT, 700) + label(x + 10, y + 40, st.note.length > 34 ? st.note.slice(0, 33) + '…' : st.note, 11, TEXT, 400) + '</g>';
});
y += 64;
svg += panel(y, 'D3D12 / D3D12.X methods (vtable slots with a runtime function)', d3d, 300); y += 340;
svg += panel(y, `Runtime imports our titles use (${titleBars.length} titles)`, dlls, 330); y += 370;
// per title
svg += label(10, y + 20, 'Imports of each title the runtime answers', 16); y += 30;
for (const b of titleBars) {
  const bw = W - 20 - 500, f = b.total ? b.done / b.total : 0;
  svg += `<g><title>${esc(`${b.name}: ${b.done}/${b.total}`)}</title>` + label(10, y + 15, b.name, 13, TEXT, 500)
    + `<rect x="270" y="${y + 2}" width="${bw}" height="16" rx="3" fill="${TODO}"/><rect x="270" y="${y + 2}" width="${(bw * f).toFixed(1)}" height="16" rx="3" fill="${f === 1 ? DONE : f >= 0.8 ? DONE : PART}"/>`
    + label(W - 10, y + 15, `${b.done}/${b.total} (${pct(b.done, b.total)}%)${b.shipped ? ` +${b.shipped} C++ runtime` : ''}`, 13, TEXT, 500, 'end') + '</g>';
  y += 24;
}
y += 10;
svg += panel(y, "XCL's console test title (checks passing)", xcl, 200); y += 240;
svg += label(10, y + 4, 'Green: done. Grey: not yet (no runtime function, or one that only returns E_NOTIMPL). Hover a tile or a square for its name.', 12, DIM, 400);
svg += label(10, y + 22, "Amber: Microsoft's C++ runtime DLLs, which ship beside a title and are recompiled with it; not counted in the percentages.", 12, DIM, 400);
y += 34;
const doc = `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${y}" viewBox="0 0 ${W} ${y}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif"><rect width="${W}" height="${y}" fill="${BG}"/>${svg}</svg>\n`;
fs.mkdirSync(out, { recursive: true });
fs.writeFileSync(path.join(out, 'progress.svg'), doc);
// badges (shields-style) and the numbers
const badge = (name, labelText, value, ratio) => {
  const color = ratio >= 0.9 ? '#4c1' : ratio >= 0.6 ? '#97ca00' : ratio >= 0.3 ? '#dfb317' : '#fe7d37';
  const lw = labelText.length * 6.5 + 12, vw = value.length * 6.5 + 12;
  fs.writeFileSync(path.join(out, name), `<svg xmlns="http://www.w3.org/2000/svg" width="${lw + vw}" height="20" role="img" aria-label="${esc(labelText)}: ${esc(value)}"><title>${esc(labelText)}: ${esc(value)}</title><rect width="${lw}" height="20" fill="#555"/><rect x="${lw}" width="${vw}" height="20" fill="${color}"/><g fill="#fff" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle"><text x="${lw / 2}" y="14">${esc(labelText)}</text><text x="${lw + vw / 2}" y="14">${esc(value)}</text></g></svg>\n`);
};
const [dd, dt] = sum(d3d), [id, it] = sum(runtimeDlls);
badge('badge-d3d12.svg', 'D3D12 methods', `${pct(dd, dt)}%`, dd / dt);
badge('badge-imports.svg', 'imports used', `${pct(id, it)}%`, id / it);
badge('badge-xcl.svg', 'XCL checks', `${xd}/${xt}`, xd / xt);
const steps_done = steps.filter((s) => s.state === 'done').length;
badge('badge-ladder.svg', 'graphics ladder', `${steps_done}/${steps.length}`, steps_done / steps.length);
fs.writeFileSync(path.join(out, 'progress.json'), JSON.stringify({ updated: data.updated, d3d12: [dd, dt], imports: [id, it], xcl: [xd, xt], ladder: [steps_done, steps.length], titles: titleBars }, null, 1) + '\n');
fs.writeFileSync(path.join(out, 'progress.html'), `<!doctype html>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PISCO progress</title>
<style>body{font:16px/1.5 system-ui,sans-serif;background:#0d1117;color:#e6edf3;max-width:1020px;margin:2em auto;padding:0 1em}a{color:#58a6ff}img,object{max-width:100%}</style>
<h1>PISCO progress</h1>
<p>What the runtime implements, measured against what real titles use. Hover the map for names. Updated ${data.updated}.
<a href="https://github.com/Yhozen/pisco">Source</a> · <a href="./">Doom video</a> · <a href="progress.json">progress.json</a></p>
<p><img src="badge-ladder.svg" alt="graphics ladder"> <img src="badge-d3d12.svg" alt="D3D12 methods"> <img src="badge-imports.svg" alt="imports"> <img src="badge-xcl.svg" alt="XCL checks"></p>
<object data="progress.svg" type="image/svg+xml" width="1000">progress map</object>
<ul>
<li><b>D3D12 / D3D12.X methods</b>: every vtable slot of the PC interfaces (d3d12.h order) and the console methods known from XCL; green when a runtime function stands behind it.</li>
<li><b>Runtime imports</b>: the union of what the titles below import (${titleBars.map((b) => esc(b.name)).join(', ')}), resolved as the runtime resolves them.</li>
<li><b>XCL's console test title</b>: its 50 checks, from the last run.</li>
</ul>
`);
console.log(`progress: ${out}/progress.svg  D3D12 ${dd}/${dt}, imports ${id}/${it}, XCL ${xd}/${xt}, ladder ${steps_done}/${steps.length}`);
