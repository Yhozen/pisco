// SPDX-License-Identifier: GPL-2.0-or-later
// The benchmark of docs/research/tabc-poc.md: the same workloads (tools/tabc-poc/workload) in one container each
// time, on one side, as JSON in out/tabc-poc/results/<side>.json:
//   node tools/tabc-poc/bench.mjs <side> [runs, default 5] [workload...]
//   node tools/tabc-poc/bench.mjs report         the tables of the doc, from the results there
// Sides:
//   pisco        out/images/alpine (tools/tabc-poc/build.sh) in headless Chrome: the lifted Linux, a fresh tab and
//                browser profile each run, Chrome started with tabc's own flags
//   tabc-arm64   tabc run (TABC, default tabc on PATH) on alpine:3.24.4, its arm64 image: unkernel, aarch64 on kwemu
//   tabc-x86     the same container, with alpine:3.24.4's amd64 layer in a chroot run by qemu-user (qemu-x86_64,
//                registered with binfmt_misc): what an amd64 image costs on tabc today
//   tabc-src     tabc-arm64 with tabc built from source (tools/tabc-pisco/setup.sh, SHARE_STOCK=1), unchanged
//   tabc-pisco   that tabc with PISCO (docs/research/tabc-pisco.md): alpine:3.24.4's amd64 image, its programs lifted
//   native       the amd64 layer in a chroot on this machine (sudo)
//   pisco-k      an experiment: out/images/alpine-k, the benchmark also lifted into the kernel's module (INKERNEL=1)
// Each run is one container from nothing: the browser started, the kernel booted, the workload run, the container
// ended. Recorded: when the first output came and when it ended (from the start of the browser or of `tabc run`),
// the workload's own times (busybox `time`, inside the guest), and for `idle` the memory (PSS) of every process of
// the container at 12 s, as tabc's tools/bench/memory.py measures it. Environment: TABC_VOL (default
// ~/ext/tabc-vol: bench-aarch64, qemu-x86_64, x86root/, prelude.sh), NATIVE_ROOT (default out/tabc-poc/native),
// TABC_CHROMIUM (Chrome for tabc; the pisco side uses google-chrome on PATH); for experiments, PISCO_PAGE (more
// page options, e.g. SW_MEMORY=128), PISCO_CHROME_FLAGS (more Chrome flags) and BENCH_OUT (another results file).
import { spawn, execFileSync } from 'node:child_process'; import fs from 'node:fs'; import path from 'node:path'; import os from 'node:os';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const results = path.join(root, 'out/tabc-poc/results');
const WORKLOADS = ['echo', 'awk', 'sha256', 'gzip', 'bench', 'fork', 'exec', 'idle'];
// what tabc starts Chrome with (internal/supervisor/chrome.go), so that both sides run the same browser the same way
const TABC_FLAGS = ['--headless=new', '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--disable-component-update',
  '--disable-default-apps', '--disable-extensions', '--disable-sync', '--disable-gpu', '--disable-dev-shm-usage', '--metrics-recording-only', '--mute-audio',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'];
const CMDLINE = 'console=ttyS0,115200 panic=-1 no_timer_check vdso=0 tsc=reliable 8250.nr_uarts=1 norandmaps quiet';
const IMAGE = 'alpine:3.24.4', VOL = process.env.TABC_VOL || path.join(os.homedir(), 'ext/tabc-vol');
const NATIVE = path.resolve(process.env.NATIVE_ROOT || path.join(root, 'out/tabc-poc/native'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// the command a container runs for a workload, as an argument list for `sh -c '...' sh <args>` or after " -- "
const command = (w) => w === 'echo' ? 'echo hi' : w === 'idle' ? 'sleep 20' : `workload ${w}`;

// ---- memory: the proportional set size of a process and everything below it (and its sessions) ----
function procs() {
  const m = new Map();
  for (const p of fs.readdirSync('/proc')) {
    if (!/^\d+$/.test(p)) continue;
    try { const s = fs.readFileSync(`/proc/${p}/stat`, 'utf8'); const f = s.slice(s.lastIndexOf(')') + 2).split(' '); m.set(+p, { ppid: +f[1], sid: +f[3] }); } catch {}
  }
  return m;
}
function treePss(pid) {
  const all = procs(), mine = new Set([pid]);
  for (let grew = true; grew;) { grew = false; for (const [p, i] of all) if (!mine.has(p) && (mine.has(i.ppid) || (mine.has(i.sid) && i.sid !== process.pid))) { mine.add(p); grew = true; } }
  let kb = 0; const rows = [];
  for (const p of mine) {
    try {
      const v = +fs.readFileSync(`/proc/${p}/smaps_rollup`, 'utf8').match(/^Pss:\s+(\d+)/m)[1]; kb += v;
      const cmd = fs.readFileSync(`/proc/${p}/cmdline`, 'utf8').replace(/\0/g, ' ');
      rows.push({ pid: p, mib: +(v / 1024).toFixed(1), kind: (cmd.match(/--type=(\S+)/) || [, cmd.includes('chrome') ? 'browser' : path.basename(cmd.split(' ')[0])])[1] });
    } catch {}
  }
  return { mib: +(kb / 1024).toFixed(1), rows };
}

// ---- one run of a process that prints the container's output: its lines with their times ----
function runProcess(cmd, args, { env = {}, idle = false } = {}) {
  return new Promise((resolve) => {
    const t0 = performance.now(), lines = []; let buf = '', mem = null;
    const p = spawn(cmd, args, { env: { ...process.env, ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
    const take = (d) => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { lines.push([performance.now() - t0, buf.slice(0, i).replace(/\r$/, '')]); buf = buf.slice(i + 1); } };
    p.stdout.on('data', take); p.stderr.on('data', take);
    if (idle) setTimeout(() => { mem = treePss(p.pid); }, 12000);
    const kill = setTimeout(() => p.kill('SIGKILL'), 900000);
    p.on('exit', (code) => { clearTimeout(kill); resolve({ code, end: performance.now() - t0, lines, mem }); });
  });
}

// ---- the pisco side: serve.mjs, then a fresh Chrome per run, driven through the DevTools protocol ----
let server = null, port = 0;
async function pisco(w, image = 'out/images/alpine') {
  if (!server) {
    port = 8800 + Math.floor(Math.random() * 100);
    server = spawn(process.execPath, [path.join(root, 'tools/serve.mjs'), String(port), path.join(root, 'web'), path.join(root, image)], { stdio: 'ignore' });
    await sleep(500);
  }
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'pisco-bench-')), dbg = port + 1000 + Math.floor(Math.random() * 1000);
  const t0 = performance.now(), lines = []; let mem = null, net = 0, navigated = 0;
  const chrome = spawn('google-chrome', [...TABC_FLAGS, ...(process.env.PISCO_CHROME_FLAGS ? process.env.PISCO_CHROME_FLAGS.split(' ') : []), `--user-data-dir=${profile}`, `--remote-debugging-port=${dbg}`, 'about:blank'],
    { stdio: 'ignore', env: { ...process.env, HOME: profile, XDG_CONFIG_HOME: path.join(profile, 'config'), XDG_CACHE_HOME: path.join(profile, 'cache') } });
  let page = null;
  for (let i = 0; i < 400 && !page; i++) { await sleep(10); try { page = (await (await fetch(`http://127.0.0.1:${dbg}/json`)).json()).find((t) => t.type === 'page'); } catch {} }
  const ws = new WebSocket(page.webSocketDebuggerUrl); let id = 0;
  const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));
  const done = new Promise((resolve) => {
    const kill = setTimeout(() => resolve('timeout'), 900000);
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.method === 'Network.loadingFinished') net += m.params.encodedDataLength;
      if (m.method !== 'Runtime.consoleAPICalled') return;
      const s = m.params.args.map((a) => a.value ?? a.description ?? '').join(' '), t = performance.now() - t0;
      for (const l of s.split('\n')) lines.push([t, l.replace(/\r$/, '')]);
      if (w === 'idle' && s.includes('container: up')) setTimeout(() => { mem = treePss(chrome.pid); }, Math.max(0, 12000 - t));
      if (/^pisco exit -?\d+$/.test(s)) { clearTimeout(kill); resolve('exit'); }
    };
  });
  await new Promise((r) => { ws.onopen = r; });
  send('Runtime.enable'); send('Network.enable');
  const url = `http://127.0.0.1:${port}/browser/index.html?image=/files/0/&SW_CMDLINE=${encodeURIComponent(`${CMDLINE} -- ${command(w)}`)}${process.env.PISCO_PAGE ? '&' + process.env.PISCO_PAGE : ''}`;
  navigated = performance.now() - t0; send('Page.navigate', { url });
  const how = await done, end = performance.now() - t0;
  chrome.kill('SIGKILL'); await new Promise((r) => (chrome.exitCode !== null ? r() : chrome.on('exit', r)));
  fs.rmSync(profile, { recursive: true, force: true });
  return { code: how === 'exit' ? 0 : 124, end, navigated, net, lines, mem };
}

// ---- tabc: one `tabc run` per container, arm64 or the amd64 layer under qemu-user ----
function tabc(w, arch, env = {}) {
  const bin = env.TABC || process.env.TABC || 'tabc', base = ['run', '--cpus', '1', '-m', '256m'];
  if (w === 'echo' || w === 'idle') return runProcess(bin, [...base, IMAGE, 'sh', '-c', command(w)], { env, idle: w === 'idle' });
  const prelude = arch === 'arm64' ? 'cp /opt/x/bench-aarch64 /usr/local/bin/bench; cp /opt/x/workload /usr/local/bin/workload; workload "$@"'
    : '. /opt/x/prelude.sh; chroot /x86 /usr/local/bin/workload "$@"';
  return runProcess(bin, [...base, '-v', `${VOL}:/opt/x:ro`, IMAGE, 'sh', '-c', prelude, 'sh', w], { env });
}

// ---- tabc-pisco: tabc 1.0.0 built from source with PISCO in it (tools/tabc-pisco/setup.sh into TABC_PISCO, default
// out/tabc-pisco) on alpine:3.24.4's amd64 image, its programs run lifted as tasks of the same kernel (unkernel's,
// patched so that kw_run asks PISCO first); a program met for the first time is lifted on the host ----
const tabcPiscoDir = () => path.resolve(process.env.TABC_PISCO || path.join(root, 'out/tabc-pisco'));
function tabcPisco(w) {
  const dir = tabcPiscoDir(), vol = path.join(dir, 'vol');
  if (!fs.existsSync(path.join(vol, 'workload'))) {
    fs.mkdirSync(vol, { recursive: true });
    fs.copyFileSync(path.join(root, 'out/tabc-poc/bench/bench-x86_64'), path.join(vol, 'bench-x86_64'));
    fs.copyFileSync(path.join(root, 'tools/tabc-poc/workload'), path.join(vol, 'workload'));
    for (const f of ['bench-x86_64', 'workload']) fs.chmodSync(path.join(vol, f), 0o755);
  }
  const env = { TABC_SHARE: path.join(dir, 'share'), TABC_IMAGE_ARCH: 'amd64', XDG_CACHE_HOME: path.join(dir, 'cache'),
    TABC_PISCO_LIFT: path.join(root, 'tools/tabc-pisco/lift-side.sh') };
  const bin = path.join(dir, 'tabc'), base = ['run', '--cpus', '1', '-m', '256m'];
  if (w === 'echo' || w === 'idle') return runProcess(bin, [...base, IMAGE, 'sh', '-c', command(w)], { env, idle: w === 'idle' });
  const prelude = 'cp /opt/x/bench-x86_64 /usr/local/bin/bench; cp /opt/x/workload /usr/local/bin/workload; workload "$@"';
  return runProcess(bin, [...base, '-v', `${vol}:/opt/x:ro`, IMAGE, 'sh', '-c', prelude, 'sh', w], { env });
}
// tabc-src: tabc-arm64 with the same tabc built from source (setup.sh with SHARE_STOCK=1), its own page and kernel
const tabcSrc = (w) => { const dir = tabcPiscoDir();
  return tabc(w, 'arm64', { TABC: path.join(dir, 'tabc'), TABC_SHARE: path.join(dir, 'share-stock'), XDG_CACHE_HOME: path.join(dir, 'cache-stock') }); };

// ---- native: the amd64 layer in a chroot ----
function nativeRun(w) {
  if (w === 'idle') return Promise.resolve({ code: 0, end: 0, lines: [], mem: null });
  return runProcess('sudo', ['chroot', NATIVE, '/bin/sh', '-c', command(w)]);
}
function nativeSetup() {
  if (!fs.existsSync(path.join(NATIVE, 'bin/busybox'))) {
    fs.mkdirSync(NATIVE, { recursive: true });
    execFileSync('tar', ['-xzf', path.join(root, 'out/tabc-poc/oci/amd64/layer0.tar.gz'), '-C', NATIVE]);
    fs.copyFileSync(path.join(root, 'out/tabc-poc/bench/bench-x86_64'), path.join(NATIVE, 'usr/local/bin/bench'));
    fs.copyFileSync(path.join(root, 'tools/tabc-poc/workload'), path.join(NATIVE, 'usr/local/bin/workload'));
    fs.chmodSync(path.join(NATIVE, 'usr/local/bin/bench'), 0o755); fs.chmodSync(path.join(NATIVE, 'usr/local/bin/workload'), 0o755);
  }
  const mounted = fs.readFileSync('/proc/mounts', 'utf8');
  for (const [what, type, dir, opt] of [['proc', 'proc', 'proc', ''], ['tmp', 'tmpfs', 'tmp', ''], ['/dev', '', 'dev', 'bind']]) {
    if (mounted.includes(` ${path.join(NATIVE, dir)} `)) continue;
    execFileSync('sudo', ['mount', ...(opt ? ['-o', opt] : ['-t', type]), what, path.join(NATIVE, dir)]);
  }
}

// ---- what a run's lines say ----
function parse(r, w) {
  const first = r.lines.find(([, l]) => (w === 'echo' ? l === 'hi' : /^w |container: up|checksum|^299995/.test(l)));
  const times = {}, bench = {};
  for (const [, l] of r.lines) {
    let m = l.match(/^w (\S+) ([\d.]+)$/); if (m) times[m[1]] = +m[2];
    m = l.match(/^(\w+)\s+.*\s(\d+) ms\s+checksum (\w+)$/); if (m) bench[m[1]] = { ms: +m[2], checksum: m[3] };
  }
  return { first: first ? +first[0].toFixed(1) : null, end: +r.end.toFixed(1), navigated: r.navigated, net: r.net, code: r.code, times, bench, mem: r.mem,
    lines: w === 'echo' ? r.lines.filter(([, l]) => !/^pisco: machine: a/.test(l)).map(([t, l]) => [+t.toFixed(1), l]) : undefined,
    ok: w === 'echo' ? !!first : w === 'idle' ? !!r.mem : Object.keys(times).length > 0 };
}

async function main() {
  const [side, n = '5', ...ws] = process.argv.slice(2);
  if (side === 'report') return report();
  const runs = +n, list = ws.length ? ws : WORKLOADS;
  const run = { pisco: (w) => pisco(w), 'pisco-k': (w) => pisco(w, 'out/images/alpine-k'), 'tabc-arm64': (w) => tabc(w, 'arm64'), 'tabc-x86': (w) => tabc(w, 'x86'), 'tabc-src': (w) => tabcSrc(w), 'tabc-pisco': (w) => tabcPisco(w), native: (w) => nativeRun(w) }[side];
  if (!run) { console.error('bench.mjs: pisco, pisco-k, tabc-arm64, tabc-x86, tabc-src, tabc-pisco, native or report'); process.exit(2); }
  if (side === 'native') nativeSetup();
  fs.mkdirSync(results, { recursive: true });
  const file = path.join(results, process.env.BENCH_OUT || `${side}.json`), out = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : { side, runs: {} };
  out.host = { node: process.version, chrome: execFileSync('google-chrome', ['--version']).toString().trim(), cpu: os.cpus()[0].model, cpus: os.cpus().length, kernel: os.release(), date: new Date().toISOString() };
  for (const w of list) {
    out.runs[w] = [];
    for (let i = 0; i < runs; i++) {
      const r = parse(await run(w), w); out.runs[w].push(r);
      console.log(`${side} ${w} #${i + 1}: first ${r.first} ms, end ${r.end} ms${Object.keys(r.times).length ? ', ' + JSON.stringify(r.times) : ''}${r.mem ? `, ${r.mem.mib} MiB` : ''}${r.ok ? '' : ' (FAILED)'}`);
      fs.writeFileSync(file, JSON.stringify(out, null, 1));
    }
  }
  if (server) server.kill();
}

// ---- the doc's tables: median (best) of the runs ----
function report() {
  const sides = ['native', 'pisco', 'pisco-k', 'tabc-arm64', 'tabc-x86', 'tabc-src', 'tabc-pisco'].filter((s) => fs.existsSync(path.join(results, `${s}.json`)));
  const data = Object.fromEntries(sides.map((s) => [s, JSON.parse(fs.readFileSync(path.join(results, `${s}.json`), 'utf8'))]));
  const med = (v) => { v = v.filter((x) => x != null).sort((a, b) => a - b); return v.length ? (v.length % 2 ? v[v.length >> 1] : (v[v.length / 2 - 1] + v[v.length / 2]) / 2) : null; };
  const cell = (v, f = (x) => x) => { v = v.filter((x) => x != null); return v.length ? `${f(med(v))} (${f(Math.min(...v))})` : '–'; };
  const row = (name, get, f) => `| ${name} | ${sides.map((s) => cell((data[s].runs[get.w] || []).filter((r) => r.ok).map(get.v), f)).join(' | ')} |`;
  const s2 = (x) => x.toFixed(2), ms = (x) => String(Math.round(x));
  const head = `| | ${sides.join(' | ')} |\n|---|${sides.map(() => '---').join('|')}|`;
  console.log('Cold start, s: median (best)\n' + head);
  console.log(row('`echo hi`: first output', { w: 'echo', v: (r) => r.first / 1000 }, s2));
  console.log(row('`echo hi`: container ended', { w: 'echo', v: (r) => r.end / 1000 }, s2));
  console.log('\nWorkloads, s (busybox `time` in the guest): median (best)\n' + head);
  for (const w of ['awk', 'sha256', 'gzip', 'fork', 'exec', 'bench']) console.log(row(w, { w, v: (r) => r.times[w] }, s2));
  console.log('\nThe compiled benchmark, ms: median (best)\n' + head);
  for (const k of ['calls', 'bytes', 'sort', 'float', 'memory']) console.log(row(k, { w: 'bench', v: (r) => r.bench[k]?.ms }, ms));
  console.log('\nMemory of an idle container at 12 s, MiB (PSS, every process)\n' + head);
  console.log(row('idle', { w: 'idle', v: (r) => r.mem?.mib }, (x) => x.toFixed(0)));
  for (const s of sides) { const r = (data[s].runs.echo || [])[0]; if (r?.net) console.log(`${s}: ${(r.net / 1e6).toFixed(1)} MB over the network for one container`); }
  for (const s of sides) { const c = new Set(); for (const r of data[s].runs.bench || []) for (const v of Object.values(r.bench)) c.add(v.checksum); if (c.size) console.log(`${s}: benchmark checksums ${[...c].join(' ')}`); }
}
await main();
