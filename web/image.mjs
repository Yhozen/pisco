// SPDX-License-Identifier: GPL-2.0-or-later
// Title images (docs/images.md), as every host reads them: web/run-node.mjs, the browser's page and workers
// (web/browser/main.js, worker.js) and tools/serve.mjs. An image is an OCI image layout (oci-layout, index.json,
// blobs/sha256/...): one manifest, a config (the title's exe, display, CPU, DLL choices, registry) and in order the
// file layers (tar: the base's, the libraries', the game's) and the module (title.wasm).
// The file layers stacked make the title's C: drive, the lower layers of its file system (rt/vfs.c): a later layer's
// file replaces an earlier one's, a ".wh.<name>" entry removes <name>. The title's own writes stay above them, kept
// by the host (SW_SAVES in Node, OPFS in the browser). Names are matched without regard to case, as on Windows.
// Nothing here needs Node; openImageDir (Node only) imports node:fs when it is called.
export const MEDIA = {
  manifest: 'application/vnd.oci.image.manifest.v1+json',
  artifact: 'application/vnd.pisco.app.v1',
  config: 'application/vnd.pisco.app.config.v1+json',
  layer: 'application/vnd.pisco.layer.v1.tar',
  module: 'application/vnd.pisco.module.v1.wasm',
};
const ROLE = 'org.pisco.layer.role';
// the systems whose drive tells case apart and has links and modes (docs/spec/image.md#files)
export const CASE_SENSITIVE = new Set(['linux', 'microvm', 'ps5']);
const utf8 = new TextDecoder();

// ---- the documents ----
// index.json -> the manifest's descriptor (the first, or the one named `ref`)
export function manifestOf(index, ref) {
  const m = index.manifests?.find((d) => !ref || d.annotations?.['org.opencontainers.image.ref.name'] === ref);
  if (!m) throw new Error(`image: no manifest${ref ? ` named ${ref}` : ''} in index.json`);
  return m;
}
export function checkManifest(manifest) {
  if (manifest.artifactType !== MEDIA.artifact) throw new Error(`image: not a pisco app (artifactType ${manifest.artifactType})${manifest.artifactType === 'application/vnd.pisco.title.v1' ? ': an image of an older pisco, build it again' : ''}`);
  const layers = manifest.layers.filter((l) => l.mediaType === MEDIA.layer), module = manifest.layers.find((l) => l.mediaType === MEDIA.module);
  if (!module) throw new Error('image: no module (title.wasm) among its layers');
  return { layers, module, roles: layers.map((l) => l.annotations?.[ROLE] || 'layer') };
}
export const blobPath = (digest) => `blobs/sha256/${digest.replace(/^sha256:/, '')}`;

// What a host needs to start the title: the exe and its folder on the drive ('C:\Program Files\X\x.exe' ->
// 'Program Files/X/x.exe', 'Program Files/X'; a Linux title's '/usr/bin/x' -> 'usr/bin/x', 'usr/bin'), its arguments,
// and the runtime's settings the image makes: SW_REGISTRY, the registry values rt/windows-x86/advapi32.c reads
// ("<key>\t<name>\tdword|sz\t<value>" a line); for a Linux title SW_EXE, its path from the root, which tells the
// runtime that its root is the image's drive (rt/linux-x64/fs.c); for a machine's title (rt/microvm-x64) SW_INITRD and
// SW_CMDLINE, its kernel's initramfs and command line
export function settings(config) {
  const linux = config.os === 'linux' || config.os === 'microvm';
  const exe = linux ? config.run.exe.split('/').filter(Boolean).join('/') : config.run.exe.replace(/^[cC]:\\/, '').split('\\').filter(Boolean).join('/');
  const folder = exe.includes('/') ? exe.slice(0, exe.lastIndexOf('/')) : '';
  const env = {};
  const reg = (config.registry || []).map((v) => `${v.key}\t${v.name}\t${v.type}\t${v.value}`);
  if (reg.length) env.SW_REGISTRY = reg.join('\n');
  if (config.os === 'linux') env.SW_EXE = `/${exe}`;
  if (config.os === 'microvm') {
    if (config.run.initrd) env.SW_INITRD = config.run.initrd.split('/').filter(Boolean).join('/');
    if (config.run.cmdline) env.SW_CMDLINE = config.run.cmdline;
  }
  // (text: a Linux title has no screen of its own, what it shows is what it prints; nor a machine's, its serial console)
  return { name: config.name, text: linux, exe, folder, exeName: exe.slice(folder.length ? folder.length + 1 : 0), args: config.run.args || [], display: config.run.display || null, env };
}
// A name the runtime asks its host for (rt/vfs.c): "c:/<path>" is on the drive, anything else is in the title's folder
export function driveName(name, folder) {
  if (/^c:(\/|$)/i.test(name)) return name.slice(3);
  return folder ? (name ? `${folder}/${name}` : folder) : name;
}

// ---- tar ----
// The entries of a tar: read(offset, length) -> Uint8Array. Plain ustar, with pax and GNU long names; a file's
// executable bit and a symbolic link's target (a Linux drive's: docs/spec/image.md#files).
export function parseTar(read, size) {
  const out = []; let at = 0, long = null, longLink = null;
  const field = (h, a, n) => { const s = h.subarray(a, a + n); const z = s.indexOf(0); return utf8.decode(z < 0 ? s : s.subarray(0, z)); };
  while (at + 512 <= size) {
    const h = read(at, 512);
    if (h.every((b) => b === 0)) break;
    const len = parseInt(field(h, 124, 12).trim() || '0', 8), kind = String.fromCharCode(h[156] || 48);
    let name = field(h, 0, 100); const prefix = field(h, 345, 155); if (prefix) name = `${prefix}/${name}`;
    const body = at + 512;
    if (kind === 'x') { const rec = utf8.decode(read(body, len)); const m = rec.match(/^\d+ path=(.*)$/m); long = m ? m[1] : null; const l = rec.match(/^\d+ linkpath=(.*)$/m); longLink = l ? l[1] : null; }
    else if (kind === 'L') long = utf8.decode(read(body, len)).replace(/\0.*$/s, '');
    else {
      if (long !== null) { name = long; long = null; }
      const mode = parseInt(field(h, 100, 8).trim() || '0', 8);
      if (kind === '0' || kind === '\0' || kind === '5') out.push({ name: name.replace(/\/+$/, ''), offset: body, size: len, dir: kind === '5', exec: kind !== '5' && (mode & 0o100) !== 0 });
      else if (kind === '2') out.push({ name: name.replace(/\/+$/, ''), offset: body, size: 0, dir: false, link: longLink ?? field(h, 157, 100) });
      longLink = null;
    }
    at = body + Math.ceil(len / 512) * 512;
  }
  return out;
}

// ---- the drive: the layers stacked ----
// A Windows drive matches names without regard to case; a Linux one (caseSensitive) exactly, and has symbolic links
// and executable bits (docs/spec/image.md#files)
export class Drive {
  // layers: [{ entries (parseTar's), read(offset, length) -> Uint8Array }], lowest first
  constructor(layers, { caseSensitive = false } = {}) {
    this.cs = caseSensitive; this.links = false;
    this.nodes = new Map([['', { dir: true, name: '', children: new Map() }]]);
    layers.forEach((layer, k) => { for (const e of layer.entries) this.#add(e, k, layer); });
  }
  #key(p) { return this.cs ? p : p.toLowerCase(); }
  #parent(path) {
    const i = path.lastIndexOf('/'); const dir = i < 0 ? '' : path.slice(0, i);
    if (!this.nodes.has(this.#key(dir))) this.#add({ name: dir, dir: true }, -1, null);
    return this.nodes.get(this.#key(dir));
  }
  #add(e, k, layer) {
    const base = e.name.slice(e.name.lastIndexOf('/') + 1), lower = this.#key(e.name);
    if (base.startsWith('.wh.')) { // a whiteout: what the layers below had there is gone
      const gone = this.#key(e.name.slice(0, e.name.length - base.length) + base.slice(4));
      for (const key of [...this.nodes.keys()]) if (key === gone || key.startsWith(gone + '/')) this.nodes.delete(key);
      this.#parent(gone).children.delete(gone.slice(gone.lastIndexOf('/') + 1));
      return;
    }
    if (!e.name) return;
    const parent = this.#parent(e.name), old = this.nodes.get(lower);
    if (e.dir && old?.dir) return;
    const node = e.dir ? { dir: true, name: base, children: new Map() } : e.link !== undefined ? { dir: false, name: base, size: 0, link: e.link }
      : { dir: false, name: base, size: e.size, offset: e.offset, layer: k, read: layer.read, exec: !!e.exec };
    if (e.link !== undefined) this.links = true;
    this.nodes.set(lower, node); parent.children.set(this.#key(base), node);
  }
  // the node at a path; its links followed (the last one too unless `follow` is false), as a kernel resolves a path
  node(path, follow = true) {
    const parts = path.split('/').filter((x) => x && x !== '.');
    if (!this.links) return this.nodes.get(this.#key(parts.join('/'))) || null;
    let cur = [], hops = 0;
    while (parts.length) {
      const p = parts.shift();
      if (p === '..') { cur.pop(); continue; }
      const n = this.nodes.get(this.#key([...cur, p].join('/')));
      if (!n) return null;
      if (n.link === undefined || (!parts.length && !follow)) { cur.push(p); if (!parts.length) return n; continue; }
      if (++hops > 40) return null;
      const t = n.link.split('/').filter((x) => x && x !== '.');
      if (n.link.startsWith('/')) cur = [];
      parts.unshift(...t);
    }
    return this.nodes.get('') || null;
  }
  link(path) { const n = this.node(path, false); return n && n.link !== undefined ? n.link : null; }       // a link's target, or null
  exec(path) { const n = this.node(path); return !n || n.dir ? -1 : n.exec ? 1 : 0; }                     // the executable bit
  stat(path) { const n = this.node(path); return !n ? -1 : n.dir ? -2 : n.size; }                     // as the runtime's host_stat
  list(path) { const n = this.node(path); if (!n?.dir) return null; return [...n.children.values()].map((c) => c.name + (c.dir ? '/' : '') + '\n').join(''); }
  // up to `length` bytes at `offset` of a file, or null
  read(path, offset = 0, length = Infinity) {
    const n = this.node(path); if (!n || n.dir) return null;
    const o = Math.min(offset, n.size), len = Math.min(length, n.size - o);
    return n.read(n.offset + o, len);
  }
  files() { return [...this.nodes.entries()].filter(([, n]) => !n.dir).length; }
}

// ---- Node: an image in a folder ----
// { config, manifest, digest, settings, drive, wasm (bytes; with { wasm: false } not read) }. Every blob is checked
// against its digest unless { verify: false } (the workers of a process that has checked them).
export async function openImageDir(dir, { verify = true, wasm = true } = {}) {
  const fs = await import('node:fs'), path = await import('node:path'), crypto = await import('node:crypto');
  const file = (rel) => path.join(dir, rel);
  const check = (digest) => {
    if (!verify) return;
    const h = crypto.createHash('sha256'), fd = fs.openSync(file(blobPath(digest)), 'r'), buf = Buffer.alloc(1 << 22);
    for (let n; (n = fs.readSync(fd, buf, 0, buf.length, null)) > 0;) h.update(buf.subarray(0, n));
    fs.closeSync(fd);
    if (`sha256:${h.digest('hex')}` !== digest) throw new Error(`image ${dir}: blob ${digest} does not have that digest`);
  };
  const json = (digest) => { check(digest); return JSON.parse(fs.readFileSync(file(blobPath(digest)), 'utf8')); };
  const index = JSON.parse(fs.readFileSync(file('index.json'), 'utf8'));
  const desc = manifestOf(index), manifest = json(desc.digest);
  const { layers, module } = checkManifest(manifest);
  const config = json(manifest.config.digest);
  const drive = new Drive(layers.map((l) => {
    check(l.digest);
    const fd = fs.openSync(file(blobPath(l.digest)), 'r');
    const read = (offset, length) => { const b = Buffer.alloc(length); fs.readSync(fd, b, 0, length, offset); return new Uint8Array(b.buffer, b.byteOffset, length); };
    return { entries: parseTar(read, l.size), read };
  }), { caseSensitive: CASE_SENSITIVE.has(config.os) });
  let bytes = null;
  if (wasm) { check(module.digest); bytes = fs.readFileSync(file(blobPath(module.digest))); }
  return { dir, digest: desc.digest, manifest, config, settings: settings(config), drive, wasm: bytes };
}
export async function isImageDir(p) {
  const fs = await import('node:fs'), path = await import('node:path');
  try { return fs.statSync(path.join(p, 'oci-layout')).isFile() && fs.statSync(path.join(p, 'index.json')).isFile(); } catch { return false; }
}

// ---- the browser: the page reads the documents and the module over HTTP (served by tools/serve.mjs, which serves the
// drive below <image>/c/) ----
export async function fetchImage(url) {
  const base = url.endsWith('/') ? url : url + '/';
  const get = async (rel, digest) => {
    const r = await fetch(base + rel); if (!r.ok) throw new Error(`image: ${base + rel}: ${r.status}`);
    const bytes = new Uint8Array(await r.arrayBuffer());
    if (digest) {
      const h = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map((b) => b.toString(16).padStart(2, '0')).join('');
      if (`sha256:${h}` !== digest) throw new Error(`image: blob ${digest} does not have that digest`);
    }
    return bytes;
  };
  const index = JSON.parse(utf8.decode(await get('index.json')));
  const desc = manifestOf(index), manifest = JSON.parse(utf8.decode(await get(blobPath(desc.digest), desc.digest)));
  const { module } = checkManifest(manifest);
  const config = JSON.parse(utf8.decode(await get(blobPath(manifest.config.digest), manifest.config.digest)));
  const wasm = await get(blobPath(module.digest), module.digest);
  return { url: base, digest: desc.digest, manifest, config, settings: settings(config), wasm, drive: `${base}c/` };
}
