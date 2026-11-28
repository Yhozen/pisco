// SPDX-License-Identifier: GPL-2.0-or-later
// A machine's state as one file: an AgentFS database (https://github.com/tursodatabase/agentfs, MIT: a file system, a
// key-value store and a log of calls, in one SQLite file). Either host keeps it: Node in a file of this machine
// (agentfs-sdk on Turso's engine), the page in OPFS (agentfs-sdk on Turso's wasm build), and the same file goes from
// one to the other, or through Turso's cloud. What is in it, all of it readable with AgentFS's own tools:
//
//   /machine/image.json             the image the machine runs (its manifest's digest): a snapshot goes on only in it
//   /machine/snapshots/<id>.json    a snapshot: where the CPU goes on (pc), the memory's size, its 64 KiB chunks by
//                                   hash (the chunks of zeros left out), the console's last screen, its programs'
//                                   modules in the order they were loaded, which host took it and when, its parent
//   /machine/chunks/<sha256>        a chunk of memory, gzipped: kept once however many snapshots hold it
//   kv machine:head                 the snapshot to go on from
//   tool_calls                      every snapshot and every restore, by which host
//
// The machine's side (rt/microvm-x64/snapshot.c): all it is is in the module's memory at the points it snapshots at.
export const CHUNK = 65536;

const hex = (b) => [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, '0')).join('');
export const sha256 = async (bytes) => hex(await crypto.subtle.digest('SHA-256', bytes));
const pipe = async (bytes, stream) => new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(stream)).arrayBuffer());
const gzip = (bytes) => pipe(bytes, new CompressionStream('gzip'));
const gunzip = (bytes) => pipe(bytes, new DecompressionStream('gzip'));

// the memory as it is now: its size in wasm pages and its chunks that are not all zeros (copies). Synchronous: the
// machine waits in host_snapshot while this runs
export function captureMemory(memory) {
  const buf = memory.buffer, pages = buf.byteLength / 65536, words = new BigUint64Array(buf), chunks = [];
  const per = CHUNK / 8;
  for (let i = 0; i < buf.byteLength / CHUNK; i++) {
    let any = false; for (let k = i * per, e = k + per; k < e; k++) if (words[k] !== 0n) { any = true; break; }
    if (any) chunks.push({ index: i, data: new Uint8Array(buf, i * CHUNK, CHUNK).slice() });
  }
  return { pages, chunks };
}
// a snapshot's memory into a new instance's (it grows to the snapshot's size; what the snapshot does not hold is zeros)
export function restoreMemory(memory, pages, chunks) {
  const have = memory.buffer.byteLength / 65536; if (pages > have) memory.grow(pages - have);
  const u8 = new Uint8Array(memory.buffer); u8.fill(0);
  for (const [index, data] of chunks) u8.set(data, index * CHUNK);
}

const enc = new TextEncoder(), dec = new TextDecoder();
const exists = async (afs, p) => { try { await afs.fs.access(p); return true; } catch { return false; } };
export async function readJSON(afs, p) { try { return JSON.parse(dec.decode(await afs.fs.readFile(p))); } catch { return null; } }
const writeJSON = (afs, p, v) => afs.fs.writeFile(p, toBuffer(enc.encode(JSON.stringify(v, null, 1))));
let toBuffer = (u8) => u8;   // (Node's SDK wants a Buffer: state.mjs's caller says how to make one, useBuffer)
export function useBuffer(from) { toBuffer = from; }

// The chunks' files, written and read as AgentFS's own rows (the schema of its SPEC.md, as agentfs-sdk writes them: an
// inode, its directory entry, its data in fs_config's chunk_size), with statements prepared once: through the SDK's
// writeFile, each file is some twenty statements, and a snapshot is hundreds of files
const FILE_MODE = 0o100644;
async function chunkDir(afs) {
  try { await afs.fs.mkdir('/machine'); } catch {}
  try { await afs.fs.mkdir('/machine/chunks'); } catch {}
  return (await afs.fs.stat('/machine/chunks')).ino;
}
async function writeChunkFiles(afs, dir, files) {
  const db = afs.getDatabase(), now = Math.floor(Date.now() / 1000);
  const size = +((await db.prepare("SELECT value FROM fs_config WHERE key = 'chunk_size'").get())?.value || 4096);
  const inode = db.prepare('INSERT INTO fs_inode (mode, nlink, uid, gid, size, atime, mtime, ctime) VALUES (?, 1, 0, 0, ?, ?, ?, ?) RETURNING ino');
  const dentry = db.prepare('INSERT INTO fs_dentry (name, parent_ino, ino) VALUES (?, ?, ?)');
  const data = db.prepare('INSERT INTO fs_data (ino, chunk_index, data) VALUES (?, ?, ?)');
  for (const [name, bytes] of files) {
    const { ino } = await inode.get(FILE_MODE, bytes.length, now, now, now);
    await dentry.run(name, dir, ino);
    for (let k = 0, at = 0; at < bytes.length; k++, at += size) await data.run(ino, k, toBuffer(bytes.subarray(at, Math.min(at + size, bytes.length))));
  }
}
async function readChunkFiles(afs, dir) {
  const rows = await afs.getDatabase().prepare('SELECT d.name AS name, f.data AS data FROM fs_dentry d JOIN fs_data f ON f.ino = d.ino WHERE d.parent_ino = ? ORDER BY d.name, f.chunk_index').all(dir);
  const parts = new Map(); for (const r of rows) { if (!parts.has(r.name)) parts.set(r.name, []); parts.get(r.name).push(new Uint8Array(r.data)); }
  return parts;
}
const join = (list) => { const n = list.reduce((a, b) => a + b.length, 0), out = new Uint8Array(n); let at = 0; for (const b of list) { out.set(b, at); at += b.length; } return out; };

// a snapshot kept: { pc, pages, chunks, screen, host, image, side, note } -> its id. Only chunks no snapshot holds yet
// are written; the rest is the manifest. One transaction, so that a snapshot is there whole or not at all
export async function saveSnapshot(afs, s, { log = () => {} } = {}) {
  const t0 = Date.now(), db = afs.getDatabase();
  const image = await readJSON(afs, '/machine/image.json');
  if (image && s.image && image.digest !== s.image.digest) throw new Error(`this state is of image ${image.digest}, not ${s.image.digest}`);
  const parent = (await afs.kv.get('machine:head')) || null;
  const dir = await chunkDir(afs);
  let known = new Set(); try { known = new Set(await afs.fs.readdir('/machine/chunks')); } catch {}
  const list = [], files = new Map();
  const hashed = await Promise.all(s.chunks.map(async (c) => [c.index, await sha256(c.data), c.data]));
  for (const [index, h, data] of hashed) { list.push([index, h]); if (!known.has(h) && !files.has(h)) files.set(h, data); }
  const zipped = await Promise.all([...files].map(async ([h, data]) => [h, await gzip(data)]));
  const fresh = zipped.length, freshBytes = zipped.reduce((n, [, z]) => n + z.length, 0);
  await db.exec('BEGIN');
  try {
    if (!image && s.image) await writeJSON(afs, '/machine/image.json', s.image);
    await writeChunkFiles(afs, dir, zipped);
    const id = `${new Date().toISOString().replace(/[-:]/g, '').replace(/\..*/, '')}-${hex(crypto.getRandomValues(new Uint8Array(3)))}`;
    await writeJSON(afs, `/machine/snapshots/${id}.json`, { id, parent, created: new Date().toISOString(), host: s.host, note: s.note || '',
      pc: '0x' + BigInt.asUintN(64, BigInt(s.pc)).toString(16), pages: s.pages, side: s.side || [], screen: s.screen || '', chunks: list });
    await afs.kv.set('machine:head', id);
    await db.exec('COMMIT');
    await afs.tools.record('snapshot', Math.floor(t0 / 1000), Math.floor(Date.now() / 1000), { host: s.host, parent }, { id, chunks: list.length, new: fresh, bytes: freshBytes });
    log(`snapshot ${id}: ${list.length} chunks of 64 KiB (${fresh} new, ${(freshBytes / 1048576).toFixed(1)} MiB gzipped) in ${Date.now() - t0} ms`);
    return id;
  } catch (e) { await db.exec('ROLLBACK').catch(() => {}); throw e; }
}

// the snapshot to go on from (the head, or one by id): its manifest and its chunks
export async function loadSnapshot(afs, id = null, { log = () => {} } = {}) {
  const t0 = Date.now();
  id = id || await afs.kv.get('machine:head'); if (!id) return null;
  const m = await readJSON(afs, `/machine/snapshots/${id}.json`); if (!m) throw new Error(`no snapshot ${id} in this state`);
  const stored = await readChunkFiles(afs, (await afs.fs.stat('/machine/chunks')).ino), cache = new Map();
  const chunks = await Promise.all(m.chunks.map(async ([index, h]) => {
    if (!stored.has(h)) throw new Error(`snapshot ${id}: its chunk ${h} is not in this state`);
    if (!cache.has(h)) cache.set(h, gunzip(join(stored.get(h))));
    return [index, await cache.get(h)];
  }));
  log(`snapshot ${id} (from ${m.host}, ${m.created}): ${chunks.length} chunks read in ${Date.now() - t0} ms`);
  return { manifest: m, chunks };
}
export async function noteRestore(afs, id, host) { const t = Math.floor(Date.now() / 1000); await afs.tools.record('restore', t, t, { host }, { id }); }

export async function listSnapshots(afs) {
  let names = []; try { names = await afs.fs.readdir('/machine/snapshots'); } catch { return []; }
  const all = await Promise.all(names.filter((n) => n.endsWith('.json')).map((n) => readJSON(afs, `/machine/snapshots/${n}`)));
  return all.filter(Boolean).map(({ chunks, screen, ...m }) => ({ ...m, chunks: chunks.length })).sort((a, b) => a.created.localeCompare(b.created));
}
export { exists };
