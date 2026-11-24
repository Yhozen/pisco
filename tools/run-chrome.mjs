// Runs a lifted title in headless Chrome and prints its output: node tools/run-chrome.mjs <title.wasm> <title.exe> [args...]
// Starts tools/serve.mjs and Chrome, follows the page console through the DevTools protocol. WebGPU runs on
// SwiftShader (Chrome's CPU Vulkan), as in Horizon's WEB.md, so no GPU is needed.
// Environment: TIMEOUT (s, default 600), SW_TRACE, PAGE ('pace=off&hash=1': more page options),
//   CAPTURE=<out.mp4> record the presented frames and the audio; DURATION=<s> then stop the title (it is
//   recorded until then); KEYS=<json> key presses for the page, [[ms, code, keyCode, down, label?], ...] (a label
//   marks the recording); RELOAD_AT=<s> load the page again then (a new title process, the same browser profile:
//   what the title stored survives), with KEYS2 for the new page
import { spawn } from 'node:child_process'; import path from 'node:path'; import fs from 'node:fs'; import os from 'node:os';
const [wasm, exe, ...args] = process.argv.slice(2);
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const port = 8000 + Math.floor(Math.random() * 1000), dbg = port + 1000;
const capture = process.env.CAPTURE ? path.resolve(process.env.CAPTURE) : null;
const server = spawn(process.execPath, [path.join(root, 'tools/serve.mjs'), port, path.join(root, 'web'), path.dirname(path.resolve(wasm)), path.dirname(path.resolve(exe)), ...(capture ? ['--capture', capture] : [])], { stdio: 'inherit' });
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'sw-chrome-'));
const gpuFlags = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-webgpu-adapter=swiftshader', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--use-gl=angle', '--autoplay-policy=no-user-gesture-required'];
const chrome = spawn('google-chrome', ['--headless=new', `--remote-debugging-port=${dbg}`, `--user-data-dir=${profile}`, '--no-first-run', '--no-sandbox', ...gpuFlags, 'about:blank'], { stdio: 'ignore' });
let ending = false;
const done = async (code) => {
  if (ending) return; ending = true;
  if (capture) { try { console.log(await (await fetch(`http://127.0.0.1:${port}/capture/end`, { method: 'POST' })).text()); } catch (e) { console.error(`capture: ${e.message}`); } }
  chrome.kill(); server.kill(); try { fs.rmSync(profile, { recursive: true, force: true }); } catch {} process.exit(code);
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
  url = `http://127.0.0.1:${port}/browser/index.html?wasm=/files/0/${path.basename(wasm)}&exe=/files/1/${path.basename(exe)}&args=${encodeURIComponent(args.join(' '))}${process.env.SW_TRACE ? `&trace=${process.env.SW_TRACE}` : ''}${capture ? '&capture=1' : ''}${process.env.PAGE ? '&' + process.env.PAGE : ''}`;
  send('Page.navigate', { url });
  if (process.env.KEYS) setTimeout(() => send('Runtime.evaluate', { expression: `window.swKeys && window.swKeys(${process.env.KEYS})` }), 3000);
  if (process.env.RELOAD_AT) setTimeout(async () => {
    console.log('reloading the page');
    if (capture) await fetch(`http://127.0.0.1:${port}/capture/mark`, { method: 'POST', body: JSON.stringify({ label: 'reload', time: Date.now() }) }).catch(() => {});
    send('Page.navigate', { url });
    if (process.env.KEYS2) setTimeout(() => send('Runtime.evaluate', { expression: `window.swKeys && window.swKeys(${process.env.KEYS2})` }), 3000);
  }, +process.env.RELOAD_AT * 1000);
};
