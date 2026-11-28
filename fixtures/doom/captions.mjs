// SPDX-License-Identifier: GPL-2.0-or-later
// node captions.mjs <raw.mp4> <marks.json> <out.mp4>: the recording with captions at its marks (what the scripted
// keys did: menu, new game, save, reload, load), an app card, and a line saying what runs.
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import { execFileSync } from 'node:child_process';
const [raw, marksPath, out] = process.argv.slice(2);
const marks = JSON.parse(fs.readFileSync(marksPath, 'utf8'));
const at = (label) => marks.find((m) => m.label === label)?.at;
const font = ['/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf', '/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf'].find((f) => fs.existsSync(f)) || '';
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'captions-'));
const filters = [];
let n = 0;
const text = (s, from, to, { y = 'h-th-24', size = 26, box = 0.62 } = {}) => {
  if (from === undefined || to === undefined) return;
  const f = path.join(tmp, `t${n++}.txt`); fs.writeFileSync(f, s);
  filters.push(`drawtext=fontfile=${font}:textfile=${f}:fontsize=${size}:fontcolor=white:x=(w-tw)/2:y=${y}:box=1:boxcolor=black@${box}:boxborderw=12:enable='between(t,${from.toFixed(2)},${to.toFixed(2)})'`);
};
text('Doom as an Xbox console app, running in a web browser', 0.3, 7, { y: '40', size: 34 });
text('doomgeneric + Freedoom, built against D3D12.X, GameInput, XAudio2 and XGameSave', 0.3, 7, { y: '96', size: 24 });
text('x86-64 code recompiled to WebAssembly  -  D3D12.X to WebGPU  -  XAudio2 to an AudioWorklet', 0.3, 7, { y: '140', size: 22 });
text('Headless Chrome, WebGPU on SwiftShader (no GPU). Every frame here is read back from WebGPU.', 0.3, 7, { y: '180', size: 22 });
text('App screen and attract demo', 7.2, (at('menu') ?? 14) - 0.2, { size: 24 });
text('GameInput: keyboard - Esc, New Game, episode, skill', at('menu'), (at('new game') ?? at('menu')) + 2.5);
text('Playing: move, fire (sound effects through XAudio2)', (at('new game') ?? 0) + 2.6, (at('automap') ?? 0) - 0.1);
text('Automap', at('automap'), (at('automap') ?? 0) + 3);
text('F2: save to the XGameSave folder S:\\GameSaves (kept in the browser, OPFS)', at('save'), (at('saved') ?? at('save')) + 3);
text('Page reloaded: a new app process starts from nothing but its package and its saves', at('reload'), (at('load') ?? (at('reload') ?? 0) + 8) - 0.2);
text('F3: load - the save came back from browser storage', at('load'), (at('loaded') ?? at('load')) + 4);
execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-i', raw, '-vf', filters.join(','), '-c:v', 'libx264', '-crf', '20', '-preset', 'medium', '-pix_fmt', 'yuv420p', '-c:a', 'copy', '-movflags', '+faststart', out], { stdio: 'inherit' });
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`captions: ${marks.map((m) => `${m.label}@${m.at.toFixed(1)}s`).join(' ')}`);
