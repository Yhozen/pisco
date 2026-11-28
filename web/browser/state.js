// SPDX-License-Identifier: GPL-2.0-or-later
// A machine's state in the page (web/state.mjs): an AgentFS database in OPFS, through agentfs-sdk on Turso's wasm build
// (bundled into /npm/agentfs-browser.js by tools/setup-agentfs.sh). The panel it adds: take a snapshot, push the file to
// the server or pull one from it (tools/serve.mjs --states: Node's runs read and write the same files), download it,
// open one, go back to any snapshot in it, or boot afresh.
// Options: &state=<name> (the file: pisco-<name>.db; default the image's name; &state=0: none), &fresh=1, &at=<snapshot id>,
//          &pull=1 (the server's file first), &snapshot_on=<text> (a snapshot when the console prints it, then &push=1),
//          &turso=<a database's URL in Turso's cloud> (the file is its replica, on Turso's sync build: ☁ pull and ☁ push; its
//          token is asked once and kept in this browser's localStorage)
import { saveSnapshot, loadSnapshot, listSnapshots, noteRestore, readJSON } from '../state.mjs';

const css = `#state{position:fixed;top:.5em;right:.5em;max-width:30em;background:#1c1c24ee;border:1px solid #445;border-radius:6px;padding:.5em .7em;font:12px/1.4 system-ui,sans-serif;color:#ccd;z-index:9}
#state b{color:#fff}#state button{font:12px system-ui;margin:.15em .15em 0 0;padding:.2em .6em;background:#2d2d3a;color:#eef;border:1px solid #556;border-radius:4px;cursor:pointer}
#state button:hover{background:#3d3d55}#state ol{margin:.4em 0 0;padding-left:1.4em;max-height:12em;overflow:auto}#state li{cursor:pointer}#state li:hover{color:#fff}
#state li.head{color:#8f8}#state .msg{color:#9ab;margin-top:.3em;white-space:pre-wrap}`;

export async function openPageState({ q, image, log, request }) {
  const name = q.get('state') || image.settings.name || 'machine', file = `pisco-${name}.db`;
  const opfs = await navigator.storage.getDirectory();
  const say = (s) => { log(`state: ${s}`); if (msg) msg.textContent = s; };
  let msg = null;
  const turso = q.get('turso');
  const lib = await import(q.get('agentfs') || (turso ? '/npm/agentfs-sync-browser.js' : '/npm/agentfs-browser.js'));
  const tokenKey = `pisco-turso-token:${turso}`, token = turso ? localStorage.getItem(tokenKey) || prompt(`Turso auth token for ${turso}`) || '' : '';
  if (turso && token) localStorage.setItem(tokenKey, token);
  const connect = () => (turso ? lib.connect({ path: file, url: turso, authToken: token, clientName: 'pisco-page' }) : lib.connect(file));
  const bytesOf = async () => new Uint8Array(await (await (await opfs.getFileHandle(file)).getFile()).arrayBuffer());
  const replace = async (bytes) => {
    for (const f of [file, `${file}-wal`]) await opfs.removeEntry(f).catch(() => {});
    const w = await (await opfs.getFileHandle(file, { create: true })).createWritable(); await w.write(bytes); await w.close();
  };
  if (q.get('pull') === '1') {
    const r = await fetch(`/state/${file}`);
    if (r.ok) { await replace(new Uint8Array(await r.arrayBuffer())); say(`pulled ${file} from the server`); } else say(`the server has no ${file} (${r.status})`);
  }
  let db = await connect(), afs = await lib.AgentFS.openWith(db);
  if (turso) { await db.pull(); say(`pulled from ${turso}`); }
  // the file whole, as one SQLite file: its log written into it, then read while the database is closed
  const whole = async () => { await db.exec('PRAGMA wal_checkpoint(TRUNCATE)'); await db.close(); try { return await bytesOf(); } finally { db = await connect(); afs = await lib.AgentFS.openWith(db); } };
  const host = `${navigator.userAgentData?.brands?.find((b) => /Chrom/.test(b.brand))?.brand || 'a browser'} on ${navigator.platform || 'a page'}`;

  // the panel
  const style = document.createElement('style'); style.textContent = css; document.head.append(style);
  const panel = document.createElement('div'); panel.id = 'state'; document.body.append(panel);
  const reload = (params) => { const u = new URL(location.href); for (const k of ['fresh', 'at', 'pull', 'push', 'snapshot_on']) u.searchParams.delete(k); for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v); location.href = u.href; };
  const push = async () => { const b = await whole(); const r = await fetch(`/state/${file}`, { method: 'PUT', body: b }); say(r.ok ? `pushed ${file} (${(b.length / 1048576).toFixed(1)} MiB) to the server: SW_STATE=<states>/${file} goes on from it in Node` : `push: ${r.status} ${await r.text()}`); };
  const download = async () => { const b = await whole(); const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([b])); a.download = file; a.click(); say(`${file}: ${(b.length / 1048576).toFixed(1)} MiB`); };
  const open = () => { const i = document.createElement('input'); i.type = 'file'; i.accept = '.db'; i.onchange = async () => { await db.close(); await replace(new Uint8Array(await i.files[0].arrayBuffer())); reload({}); }; i.click(); };
  let current = null;
  async function draw() {
    const list = await listSnapshots(afs), head = await afs.kv.get('machine:head');
    panel.innerHTML = `<b>${file}</b> · AgentFS in OPFS · ${list.length} snapshot${list.length === 1 ? '' : 's'}<br>`;
    const button = (label, fn) => { const b = document.createElement('button'); b.textContent = label; b.onclick = () => fn().catch((e) => say(e.message)); panel.append(b); };
    button('📸 snapshot', async () => { if (!request()) say('the machine is not running'); else say('a snapshot…'); });
    button('⇪ push', push); button('⇩ pull', async () => reload({ pull: '1' })); button('⬇ .db', download); button('📂 open', async () => open());
    button('↺ boot afresh', async () => reload({ fresh: '1' }));
    if (turso) { button('☁ push', async () => { await db.push(); say(`pushed to ${turso}`); }); button('☁ pull', async () => { await db.pull(); await draw(); say(`pulled from ${turso}`); }); }
    const ol = document.createElement('ol');
    for (const s of list.slice().reverse()) {
      const li = document.createElement('li'); if (s.id === head) li.className = 'head';
      li.textContent = `${s.created.slice(11, 19)} ${s.id === current ? '▶ ' : ''}from ${s.host}${s.parent ? '' : ' (first)'} · ${s.chunks} chunks`;
      li.title = `${s.id}, pc ${s.pc}: click to go on from it`; li.onclick = () => reload({ at: s.id }); ol.append(li);
    }
    panel.append(ol);
    msg = document.createElement('div'); msg.className = 'msg'; panel.append(msg);
  }
  await draw();

  return {
    // the snapshot to go on from, or null: boot
    async restore() {
      if (q.get('fresh') === '1') return null;
      const got = await loadSnapshot(afs, q.get('at') || null, { log: (s) => say(s) }); if (!got) return null;
      const want = await readJSON(afs, '/machine/image.json');
      if (want && want.digest !== image.digest) { say(`${file} is the state of image ${want.digest}, not this one: booting`); return null; }
      await noteRestore(afs, got.manifest.id, host); current = got.manifest.id; await draw();
      return { pc: got.manifest.pc, pages: got.manifest.pages, chunks: got.chunks, screen: got.manifest.screen, side: got.manifest.side, id: got.manifest.id, from: got.manifest.host };
    },
    // the worker's snapshot (host_snapshot): kept in the file
    async save(s) {
      const id = await saveSnapshot(afs, { ...s, host, image: { digest: image.digest, name: image.settings.name } }, { log: say });
      current = id; await draw(); say(`snapshot ${id} kept in ${file}`);
      if (q.get('push') === '1') await push();
      if (turso) { await db.push(); say(`snapshot ${id} pushed to ${turso}`); }
      return id;
    },
  };
}
