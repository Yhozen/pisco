// SPDX-License-Identifier: GPL-2.0-or-later
// A static server with the headers shared memory needs (cross-origin isolation).
// node tools/serve.mjs <port> <dir>... [--capture <out.mp4>] [--shots <dir>] — the first dir is the root;
// /files/<n>/ maps to the n-th extra dir. With --capture, the page's presented frames (POST /capture/frame) go to
// ffmpeg at a fixed frame rate by their timestamps (a frame is repeated or dropped so the video runs in real
// time), the audio (POST /capture/audio, stereo float) is kept beside it, and POST /capture/end writes <out.mp4>
// with both. With --shots, a frame the page sends under a name (POST /capture/shot: a session's checkpoint)
// is saved as <dir>/<name>.png.
// An extra dir that is a title image (web/image.mjs) is served as it is (index.json, blobs/...), and its C: drive, the
// file layers stacked, below /files/<n>/c/ (with ?stat and ?list as for a folder).
import { isImageDir, openImageDir } from '../web/image.mjs';
import http from 'node:http'; import fs from 'node:fs'; import path from 'node:path'; import zlib from 'node:zlib'; import { spawn } from 'node:child_process';
const argv = process.argv.slice(2);
const ci = argv.indexOf('--capture'); const captureOut = ci >= 0 ? argv.splice(ci, 2)[1] : null;
const si = argv.indexOf('--shots'); const shotsDir = si >= 0 ? argv.splice(si, 2)[1] : null;
const [port, root, ...extra] = argv;
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json', '.wgsl': 'text/plain' };
const FPS = 35; // Doom's tic rate; other titles get every 35th of a second
let cap = null, audioChunks = [], marks = []; // audio chunks (with their times) may come before the first frame
function captureFrame(meta, body) {
  // a frame cut short (the page stopped while sending it) or arriving after the end is dropped
  if (body.length !== meta.width * meta.height * 4 || cap?.ending) return;
  if (!cap) {
    const tmp = captureOut + '.video.mp4';
    const ff = spawn('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', meta.format.startsWith('bgra') ? 'bgra' : 'rgba', '-s', `${meta.width}x${meta.height}`, '-r', String(FPS), '-i', '-',
      '-vf', `scale=${meta.width * 2}:${meta.height * 2}:flags=neighbor`, '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', '-preset', 'veryfast', tmp], { stdio: ['pipe', 'inherit', 'inherit'] });
    cap = { ff, tmp, t0: meta.time, written: 0, last: null, frames: 0, done: new Promise((r) => ff.on('close', r)) };
  }
  if (cap.ending) return;   // a frame that arrives while the recording is being finished
  cap.frames++;
  // fill the video up to this frame's time with the previous frame, then this one
  const k = Math.round((meta.time - cap.t0) * FPS / 1000);
  while (cap.last && cap.written < k) { cap.ff.stdin.write(cap.last); cap.written++; }
  if (cap.written <= k) { cap.ff.stdin.write(body); cap.written++; }
  cap.last = body;
}
async function captureEnd() {
  if (!cap) return 'no frames';
  cap.ending = true; cap.ff.stdin.end(); await cap.done;
  const pcm = captureOut + '.audio.f32';
  // the audio track: each chunk at its time on the video's clock (gaps stay silent, as across a reload)
  const rate = audioChunks[0]?.rate || 48000, total = Math.ceil(cap.written / FPS * rate);
  const audio = new Float32Array(total * 2);
  for (const c of audioChunks) {
    const at = Math.round((c.time - cap.t0) / 1000 * rate), f = new Float32Array(c.data.buffer, c.data.byteOffset, c.data.length / 4);
    for (let i = 0; i < f.length / 2; i++) { const k = at + i; if (k >= 0 && k < total) { audio[2 * k] = f[2 * i]; audio[2 * k + 1] = f[2 * i + 1]; } }
  }
  let args = ['-y', '-loglevel', 'error', '-i', cap.tmp];
  if (audioChunks.length) {
    fs.writeFileSync(pcm, Buffer.from(audio.buffer));
    args = args.concat(['-f', 'f32le', '-ar', String(rate), '-ac', '2', '-i', pcm, '-c:v', 'copy', '-c:a', 'aac', '-b:a', '160k', '-shortest']);
  } else args = args.concat(['-c', 'copy']);
  fs.writeFileSync(captureOut + '.marks.json', JSON.stringify(marks.map((m) => ({ label: m.label, at: (m.time - cap.t0) / 1000 })), null, 1));
  args.push(captureOut);
  await new Promise((r) => spawn('ffmpeg', args, { stdio: 'inherit' }).on('close', r));
  for (const f of [cap.tmp, pcm]) fs.rmSync(f, { force: true });
  return `${captureOut}: ${cap.frames} frames presented, ${cap.written} video frames at ${FPS} fps, ${(audioChunks.reduce((n, c) => n + c.data.length, 0) / 8 / rate).toFixed(1)} s of audio, ${marks.length} marks`;
}
// A frame as an 8-bit RGB PNG (what the oracle's screenshots are: tools/oracle-wine/play.py): alpha is left out
function savePng(file, meta, body) {
  const { width: w, height: h } = meta, bgra = meta.format.startsWith('bgra'), rows = Buffer.alloc(h * (1 + 3 * w));
  for (let y = 0, o = 0; y < h; y++) { rows[o++] = 0; for (let x = 0, i = 4 * y * w; x < w; x++, i += 4) { rows[o++] = body[i + (bgra ? 2 : 0)]; rows[o++] = body[i + 1]; rows[o++] = body[i + (bgra ? 0 : 2)]; } }
  const chunk = (type, data) => { const b = Buffer.alloc(12 + data.length); b.writeUInt32BE(data.length, 0); b.write(type, 4, 'latin1'); data.copy(b, 8); b.writeUInt32BE(zlib.crc32(b.subarray(4, 8 + data.length)), 8 + data.length); return b; };
  const head = Buffer.alloc(13); head.writeUInt32BE(w, 0); head.writeUInt32BE(h, 4); head.set([8, 2, 0, 0, 0], 8);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat([Buffer.from('\x89PNG\r\n\x1a\n', 'latin1'), chunk('IHDR', head), chunk('IDAT', zlib.deflateSync(rows, { level: 6 })), chunk('IEND', Buffer.alloc(0))]));
}
function saveShot(meta, body) {
  if (!shotsDir) return 'not saved (the server has no --shots folder)';
  if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$/.test(meta.name) || body.length !== 4 * meta.width * meta.height) return 'not saved (a name or a size the server does not take)';
  const file = path.join(shotsDir, `${meta.name}.png`); savePng(file, meta, body);
  return `saved as ${path.relative(process.cwd(), file)}`;
}

const images = new Map(); // n -> the image in extra dir n (null: a plain folder)
const imageOf = (n) => { if (!images.has(n)) images.set(n, isImageDir(extra[n]).then((is) => (is ? openImageDir(extra[n], { wasm: false }) : null))); return images.get(n); };
http.createServer(async (req, res) => {
  let p = decodeURIComponent(new URL(req.url, 'http://x').pathname), base = root;
  const head = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp', 'Cross-Origin-Resource-Policy': 'same-origin', 'Content-Type': 'text/plain' };
  if (req.method === 'POST' && p.startsWith('/capture/')) {
    const parts = []; req.on('data', (d) => parts.push(d));
    req.on('end', async () => {
      const body = Buffer.concat(parts); let reply = 'ok';
      if (p === '/capture/shot') reply = saveShot(JSON.parse(req.headers['x-shot']), body);
      else if (!captureOut) reply = 'not recording';
      else if (p === '/capture/frame') captureFrame(JSON.parse(req.headers['x-frame']), body);
      else if (p === '/capture/audio') audioChunks.push({ ...JSON.parse(req.headers['x-audio']), data: body });
      else if (p === '/capture/mark') marks.push(JSON.parse(body.toString()));
      else if (p === '/capture/end') reply = await captureEnd();
      res.writeHead(200, head).end(reply);
    });
    return;
  }
  const m = p.match(/^\/files\/(\d+)(\/.*)$/); if (m) { base = extra[+m[1]]; p = m[2]; }
  const image = m && /^\/c(\/|$)/.test(p) ? await imageOf(+m[1]).catch((e) => { console.error(`serve: ${e.message}`); return null; }) : null;
  if (image) {
    const d = image.drive, name = p.slice(3), query = new URL(req.url, 'http://x').search;
    if (query === '?stat') { const s = d.stat(name); res.writeHead(s === -1 ? 404 : 200, head).end(s === -1 ? '' : String(s)); return; }
    if (query === '?list') { const l = d.list(name); res.writeHead(l === null ? 404 : 200, head).end(l ?? ''); return; }
    const data = d.read(name); if (!data) { res.writeHead(404, head).end(); return; }
    res.writeHead(200, { ...head, 'Content-Type': types[path.extname(name)] || 'application/octet-stream', 'Content-Length': data.length }).end(data);
    return;
  }
  // file names are matched without regard to case, as the title expects of Windows
  let file = path.resolve(base);
  for (const part of path.normalize(p).split(path.sep).filter(Boolean)) {
    let list = []; try { list = fs.readdirSync(file); } catch {}
    file = path.join(file, list.find((f) => f.toLowerCase() === part.toLowerCase()) || part);
  }
  if (!file.startsWith(path.resolve(base))) { res.writeHead(403).end(); return; }
  const query = new URL(req.url, 'http://x').search;
  if (query === '?stat') { fs.stat(file, (err, st) => { if (err) res.writeHead(404, head).end(); else res.writeHead(200, head).end(String(st.isDirectory() ? -2 : st.size)); }); return; }
  if (query === '?list') { fs.readdir(file, { withFileTypes: true }, (err, es) => { if (err) res.writeHead(404, head).end(); else res.writeHead(200, head).end(es.map((e) => e.name + (e.isDirectory() ? '/' : '') + '\n').join('')); }); return; }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404, head).end(); return; }
    res.writeHead(200, { ...head, 'Content-Type': types[path.extname(file)] || 'application/octet-stream', 'Content-Length': st.size });
    fs.createReadStream(file).pipe(res);
  });
}).listen(+port, '127.0.0.1');
