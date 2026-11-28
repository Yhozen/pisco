// SPDX-License-Identifier: GPL-2.0-or-later
// tests/registry.mjs <folder> <port file>: a small OCI registry (distribution-spec's push and pull: blobs by
// digest, monolithic uploads, manifests by tag or digest), in memory and on 127.0.0.1, for tests/store.sh. It asks
// for a bearer token as ghcr.io does: pulling needs any token (anonymous ones are given), pushing one got with the
// basic credentials pisco:secret. Writes the port it listens on to <port file>.
import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';

const [, , , portFile] = process.argv;
const blobs = new Map(), manifests = new Map(), uploads = new Map();
const tokens = new Map();   // token -> 'pull' | 'pull,push'
const sha = (b) => 'sha256:' + crypto.createHash('sha256').update(b).digest('hex');
let base;

const server = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks), url = new URL(req.url, base);
    if (url.pathname === '/token') {
      const auth = req.headers.authorization || '';
      const ok = auth === 'Basic ' + Buffer.from('pisco:secret').toString('base64');
      if (auth && !ok) return res.writeHead(401).end();
      const scope = url.searchParams.get('scope') || '';
      const t = crypto.randomUUID();
      tokens.set(t, ok && scope.endsWith(':pull,push') ? 'pull,push' : 'pull');
      return res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ token: t }));
    }
    const m = url.pathname.match(/^\/v2\/(.+?)\/(blobs|manifests)\/(uploads\/?)?([^/]*)$/);
    if (url.pathname === '/v2/' || !m) return res.writeHead(url.pathname === '/v2/' ? 200 : 404).end();
    const [, repo, kind, upload, ref] = m;
    const writes = req.method !== 'GET' && req.method !== 'HEAD';
    const t = tokens.get((req.headers.authorization || '').replace(/^Bearer /, ''));
    if (!t || (writes && t !== 'pull,push')) {
      return res.writeHead(401, { 'www-authenticate': `Bearer realm="${base}/token",service="test",scope="repository:${repo}:${writes ? 'pull,push' : 'pull'}"` }).end();
    }
    if (kind === 'blobs' && upload !== undefined) {
      if (req.method === 'POST') { const id = crypto.randomUUID(); uploads.set(id, repo); return res.writeHead(202, { location: `/v2/${repo}/blobs/uploads/${id}` }).end(); }
      if (req.method === 'PUT' && uploads.has(ref)) {
        const d = url.searchParams.get('digest');
        if (sha(body) !== d) return res.writeHead(400).end('DIGEST_INVALID');
        uploads.delete(ref); blobs.set(`${repo}@${d}`, body);
        return res.writeHead(201, { location: `/v2/${repo}/blobs/${d}` }).end();
      }
      return res.writeHead(404).end();
    }
    if (kind === 'blobs') {
      const b = blobs.get(`${repo}@${ref}`);
      if (!b) return res.writeHead(404).end();
      return res.writeHead(200, { 'content-length': b.length }).end(req.method === 'HEAD' ? undefined : b);
    }
    if (req.method === 'PUT') {
      const doc = JSON.parse(body);
      for (const d of [doc.config, ...doc.layers]) if (!blobs.has(`${repo}@${d.digest}`)) return res.writeHead(400).end('BLOB_UNKNOWN');
      manifests.set(`${repo}:${ref}`, body); manifests.set(`${repo}:${sha(body)}`, body);
      return res.writeHead(201, { 'docker-content-digest': sha(body) }).end();
    }
    const b = manifests.get(`${repo}:${ref}`);
    if (!b) return res.writeHead(404).end();
    res.writeHead(200, { 'content-type': 'application/vnd.oci.image.manifest.v1+json', 'docker-content-digest': sha(b) }).end(req.method === 'HEAD' ? undefined : b);
  });
});
server.listen(0, '127.0.0.1', () => { base = `http://127.0.0.1:${server.address().port}`; fs.writeFileSync(portFile, String(server.address().port)); });
