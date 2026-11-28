// SPDX-License-Identifier: GPL-2.0-or-later
// shot.mjs <port> <out.png> [js to evaluate first]: a screenshot of tabc's page over the DevTools protocol
const [port, out, js] = process.argv.slice(2);
const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = list.find((t) => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.onopen = r);
let id = 0; const wait = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (wait.has(d.id)) { wait.get(d.id)(d); wait.delete(d.id); } };
const call = (method, params = {}) => new Promise((r) => { wait.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
if (js) { const r = await call('Runtime.evaluate', { expression: js, awaitPromise: true }); console.log(JSON.stringify(r.result?.result?.value ?? r.result)); }
const r = await call('Page.captureScreenshot', { format: 'png' });
(await import('node:fs')).writeFileSync(out, Buffer.from(r.result.data, 'base64'));
ws.close();
