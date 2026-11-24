// SPDX-License-Identifier: GPL-2.0-or-later
// Runs a lifted title in headless Chrome and prints its output: node tools/run-chrome.mjs <title.wasm> <title.exe> [args...]
// Starts tools/serve.mjs and Chrome, follows the page console through the DevTools protocol. WebGPU runs on
// SwiftShader (Chrome's CPU Vulkan), as in Horizon's WEB.md, so no GPU is needed.
// Environment: TIMEOUT (s, default 600), PAGE ('pace=off&hash=1': more page options), and the runtime's own SW_*
//   settings as the Node host takes them (SW_TRACE, SW_STRICT, SW_CLOCK=<microseconds a frame>: a virtual clock, ...);
//   SW_SESSION=<session.json> plays a scripted session (Windows desktop titles), and the frame presented at each of
//   its `shot` steps is saved as <the wasm's folder>/shots/<name>.png;
//   CAPTURE=<out.mp4> record the presented frames and the audio; DURATION=<s> then stop the title (it is
//   recorded until then); KEYS=<json> key presses for the page, [[ms, code, keyCode, down, label?], ...] (a label
//   marks the recording); RELOAD_AT=<s> load the page again then (a new title process, the same browser profile:
//   what the title stored survives), with KEYS2 for the new page; CHROME_PROFILE=<folder> keep the browser's profile
//   there (OPFS: what titles stored) and use it again next time, with PORT=<n> for the same origin (OPFS is per origin)
import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs'; import os from 'node:os';
import { sessionSteps } from '../web/session.mjs';
const [wasm, exe, ...args] = process.argv.slice(2);
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const port = +(process.env.PORT || 8000 + Math.floor(Math.random() * 1000)), dbg = port + 1000;
const capture = process.env.CAPTURE ? path.resolve(process.env.CAPTURE) : null;
const server = spawn(process.execPath, [path.join(root, 'tools/serve.mjs'), port, path.join(root, 'web'), path.dirname(path.resolve(wasm)), path.dirname(path.resolve(exe)), ...(capture ? ['--capture', capture] : []),
  '--shots', path.join(path.dirname(path.resolve(wasm)), 'shots')], { stdio: 'inherit' });
// the runtime's settings for the page: every SW_* of the environment (SW_TRACE has its own, older, page option), the session as its steps
const settings = Object.entries(process.env).filter(([k]) => k.startsWith('SW_') && k !== 'SW_TRACE' && k !== 'SW_SESSION');
if (process.env.SW_SESSION) settings.push(['SW_SESSION_STEPS', sessionSteps(JSON.parse(fs.readFileSync(process.env.SW_SESSION, 'utf8')))]);
const keepProfile = !!process.env.CHROME_PROFILE;
const profile = keepProfile ? path.resolve(process.env.CHROME_PROFILE) : fs.mkdtempSync(path.join(os.tmpdir(), 'sw-chrome-'));
fs.mkdirSync(profile, { recursive: true });
const gpuFlags = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--use-gl=angle', '--autoplay-policy=no-user-gesture-required'];
const chrome = spawn('google-chrome', ['--headless=new', `--remote-debugging-port=${dbg}`, `--user-data-dir=${profile}`, '--no-first-run', '--no-sandbox', ...gpuFlags, 'about:blank'], { stdio: 'ignore' });
let ending = false;
const done = async (code) => {
  if (ending) return; ending = true;
  if (capture) { try { console.log(await (await fetch(`http://127.0.0.1:${port}/capture/end`, { method: 'POST' })).text()); } catch (e) { console.error(`capture: ${e.message}`); } }
  chrome.kill(); server.kill();
  if (keepProfile) await new Promise((r) => (chrome.exitCode !== null ? r() : chrome.on('exit', r)));   // the profile is written when Chrome ends
  else try { fs.rmSync(profile, { recursive: true, force: true }); } catch {}
  process.exit(code);
};
setTimeout(() => { console.error('timed out'); done(124); }, +(process.env.TIMEOUT || 600) * 1000);
if (process.env.DURATION) setTimeout(() => { console.log(`stopping after ${process.env.DURATION} s`); done(0); }, +process.env.DURATION * 1000);
let targets;
for (let i = 0; i < 100 && !targets; i++) { await new Promise((r) => setTimeout(r, 200)); try { targets = await (await fetch(`http://127.0.0.1:${dbg}/json`)).json(); } catch {} }
const page = targets.find((t) => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
let id = 0, url = ''; const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.method === 'Runtime.consoleAPICalled') {
    const s = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
    const x = s.match(/^series-web exit (-?\d+)$/);
    if (x) done(+x[1]); else console.log(s);
  }
  if (m.method === 'Runtime.exceptionThrown') console.error('page exception:', m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
};
ws.onopen = () => {
  send('Runtime.enable');
  url = `http://127.0.0.1:${port}/browser/index.html?wasm=/files/0/${path.basename(wasm)}&exe=/files/1/${path.basename(exe)}&args=${encodeURIComponent(args.join(' '))}${process.env.SW_TRACE ? `&trace=${process.env.SW_TRACE}` : ''}${capture ? '&capture=1' : ''}${settings.map(([k, v]) => `&${k}=${encodeURIComponent(v)}`).join('')}${process.env.PAGE ? '&' + process.env.PAGE : ''}`;
  send('Page.navigate', { url });
  if (process.env.KEYS) setTimeout(() => send('Runtime.evaluate', { expression: `window.swKeys && window.swKeys(${process.env.KEYS})` }), 3000);
  if (process.env.RELOAD_AT) setTimeout(async () => {
    console.log('reloading the page');
    if (capture) await fetch(`http://127.0.0.1:${port}/capture/mark`, { method: 'POST', body: JSON.stringify({ label: 'reload', time: Date.now() }) }).catch(() => {});
    send('Page.navigate', { url });
    if (process.env.KEYS2) setTimeout(() => send('Runtime.evaluate', { expression: `window.swKeys && window.swKeys(${process.env.KEYS2})` }), 3000);
  }, +process.env.RELOAD_AT * 1000);
};
