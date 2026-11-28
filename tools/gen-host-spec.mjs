// SPDX-License-Identifier: GPL-2.0-or-later
// node tools/gen-host-spec.mjs [--check]: docs/spec/host.md, the host interface (version 0), from the sources: every
// HOST("name") the runtime imports (rt/), the WASI functions the Node host answers (web/run-node.mjs), the rings'
// headers, and the SW_* variables with what kind of setting each is. With --check it writes nothing and fails when the
// page is not what the sources give, or when a SW_* variable of the sources has no row below (tests/run.sh spec).
import fs from 'node:fs'; import path from 'node:path';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const rel = (p) => path.relative(root, p);
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) => e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]);

// ---- the SW_* variables: each one's kind, and what it does. The kinds:
//   config   the host sets it from the image's config: part of the host interface
//   process  the runtime passes it to the process an execve starts (through the host): part of the host interface
//   run      an option of one run, given by whoever runs the image (pisco run, the page's URL)
//   debug    a switch for finding a fault: not part of any interface, may change at any time
//   tool     pisco's or a script's own, not the runtime's
//   none     not a variable: a Win32 constant, a C macro
const SW = {
  SW_REGISTRY: ['config', 'the registry the app sees, from the config\'s `registry`: one value a line, `<key>\\t<name>\\tdword|sz\\t<value>`'],
  SW_EXE: ['config', 'a Linux app: the path of its program from the drive\'s root (`run.exe`), which says the root is the image\'s drive; then each execve\'s'],
  SW_INITRD: ['config', 'the machine: the kernel\'s initramfs, from `run.initrd`'],
  SW_CMDLINE: ['config', 'the machine: the kernel\'s command line, from `run.cmdline`'],
  SW_PROGRAMS: ['config', 'the machine: programs in modules of their own to load (the config\'s `side`, and `<name>.wasm` files beside the module)'],
  SW_ARGV0: ['process', 'the first argument of the program an execve starts'],
  SW_CWD: ['process', 'the working folder of the program an execve starts'],
  SW_ENVIRON: ['process', 'the environment of the program an execve starts, its values separated by 0x1e'],
  SW_FDS: ['process', 'the open descriptors an execve passes on'],
  SW_CLOCK: ['run', 'the virtual clock: microseconds a presented frame (a run is then the same every time)'],
  SW_SESSION: ['run', 'a session to play ([sessions.md](sessions.md)): the host turns it into SW_SESSION_STEPS'],
  SW_SESSION_STEPS: ['run', 'the steps of a session as the runtime\'s player reads them, separated by `;`'],
  SW_RANDOM: ['run', 'the values the app\'s random source answers first (a session\'s `random`)'],
  SW_SEED: ['run', 'the seed of the random source after SW_RANDOM\'s values (default 1)'],
  SW_SAVES: ['run', 'Node: the folder that keeps what the app writes'],
  SW_STRICT: ['run', 'stop at the first call the runtime does not answer, instead of going on'],
  SW_PACE: ['run', '`off`: a present does not wait for the next 60 Hz frame'],
  SW_EXIT_FRAMES: ['run', 'end the run after this many presents'],
  SW_MAX_FRAMES: ['run', 'PS5: end the run after this many flips'],
  SW_KEYS: ['run', 'a scripted keyboard: `[[ms, code, keyCode, down], ...]` on the virtual clock'],
  SW_TTY: ['run', 'a Linux app: the standard streams are a terminal (`1`, or `<columns>x<rows>`)'],
  SW_CONSOLE: ['run', 'the machine: `tty` when someone may type into its console'],
  SW_NET: ['run', 'the machine: `1`, a virtio network (Node: the guest at 10.0.2.15, its TCP through this machine\'s sockets)'],
  SW_DISK: ['run', 'the machine: a virtio disk, a file or `<n>M` of zeros'],
  SW_MEMORY: ['run', 'the machine: its memory'],
  SW_ROOT: ['run', 'Node: a folder of this machine as a Linux app\'s root (without an image); for pisco, the repository'],
  SW_AUDIO_WAV: ['debug', 'write what the mixer mixes to this WAV file'],
  SW_BREAK: ['debug', 'with a lift traced by pc: the registers when the block at this address is entered'],
  SW_CALL_STATS: ['debug', 'count every import\'s calls'],
  SW_D3D9_FRAMES: ['debug', 'Direct3D 9\'s calls of every frame'],
  SW_D3D9_SHADERS: ['debug', 'each Direct3D 9 shader bytecode\'s length and hash'],
  SW_D3D9_STATS: ['debug', 'Direct3D 9\'s calls between checkpoints'],
  SW_DEBUG_READ: ['debug', 'Node: every file read that finds nothing or less than asked'],
  SW_DUMP_RAW: ['debug', 'Node: every presented frame as packed RGBA, to this file'],
  SW_FILES: ['debug', 'every file the app opens'],
  SW_FMOD_TRACE: ['debug', 'every FMOD call, to set against Wine\'s +snoop'],
  SW_GL_STATS: ['debug', 'OpenGL\'s calls between checkpoints'],
  SW_GPU_TRACE: ['debug', 'the null GPU back end: every record'],
  SW_GPU_TRACE_FRAME: ['debug', 'the null GPU back end: the records of this frame'],
  SW_HLSL_CAPTURE: ['debug', 'D3DCompile logs each source it has no shader for (pisco\'s warm-up uses it)'],
  SW_SHOT_EVERY: ['debug', 'the page saves every nth flip'],
  SW_TRACE: ['debug', 'trace system calls, the machine\'s events, side modules'],
  SW_TRACE_DLL: ['debug', 'trace the calls into these DLLs'],
  SW_NO_CARGO: ['tool', 'pisco: do not build the lifter first'],
  SW_AUDIO_CODECS: ['none', 'a C macro: the runtime is built with FFmpeg\'s decoders'],
  SW_HIDE: ['none', 'Win32\'s ShowWindow constant'],
};

// what a host function does, where its declaration's comment does not say it in one place
const NOTES = {
  audio_dump: 'the frames the mixer mixed, for SW_AUDIO_WAV',
  audio_open: 'the host starts playing the mixer\'s ring (rt/mixer.h)',
  exec: 'whether the file at a name is executable: 1, 0, or -1 when the host does not know (a Linux drive)',
  fault: 'the module stops: a fault of this kind (a table rt/rt.h has) at this address',
  frame_dump: 'a presented frame, for SW_DUMP_RAW',
  log: 'a line for the host\'s log',
  now_us: 'the host\'s clock, in microseconds',
  proc_ppid: 'the number of the process that started this one',
  program_image: 'the machine met code no program it has holds: the program\'s image, read from memory, and its functions\' addresses (u64s); the host may lift it into a module of its own and load it (web/lift.mjs); 1 when it did',
  store_read: 'the bytes of stored file `index`',
  store_remove: 'what the app wrote at a path is gone',
  store_save: 'what the app wrote at a path: kept by the host, apart from the image',
};

// ---- the host functions: HOST("name") in rt/
const host = [];
for (const f of walk(path.join(root, 'rt')).filter((f) => /\.(c|h)$/.test(f)).sort()) {
  const lines = fs.readFileSync(f, 'utf8').split('\n');
  lines.forEach((line, i) => {
    const m = line.match(/HOST\("([a-z0-9_]+)"\)\s*(.*?);\s*(?:\/\/\s*(.*))?$/);
    if (!m || line.startsWith('#define')) return;
    // its note: the comment after it, or the comment lines just above it
    let note = NOTES[m[1]] || m[3] || '';
    if (!note) { const above = []; for (let j = i - 1; j >= 0 && /^\s*\/\/ /.test(lines[j]) && above.length < 3; j--) above.unshift(lines[j].replace(/^\s*\/\/ /, '')); note = above.join(' '); }
    host.push({ name: m[1], sig: m[2].replace(/__attribute__\(\(noreturn\)\)\s*/, '').trim(), note, file: rel(f) });
  });
}
const byName = new Map(); for (const h of host) { if (!byName.has(h.name)) byName.set(h.name, { ...h, files: [] }); byName.get(h.name).files.push(h.file); }
const folder = (f) => { const p = f.split('/'); return p.length > 2 ? p[1] : 'core'; };

// ---- the WASI functions the Node host answers
const node = fs.readFileSync(path.join(root, 'web/run-node.mjs'), 'utf8');
const at = node.indexOf('const wasi = {'); let depth = 0, end = at;
for (let i = node.indexOf('{', at); i < node.length; i++) { if (node[i] === '{') depth++; else if (node[i] === '}' && --depth === 0) { end = i; break; } }
const wasi = [...new Set([...node.slice(at, end).matchAll(/\n\s{4}([a-z_]+)\s*:/g)].map((m) => m[1]))].sort();

// ---- the SW_* variables the sources name
const found = new Set();
for (const d of ['rt', 'web', 'crates', 'tools']) for (const f of walk(path.join(root, d)).filter((f) => /\.(c|h|mjs|js|rs|sh|py)$/.test(f) && !f.endsWith('gen-host-spec.mjs'))) for (const m of fs.readFileSync(f, 'utf8').matchAll(/\bSW_[A-Z0-9_]+\b/g)) found.add(m[0]);
const unclassed = [...found].filter((v) => !SW[v]).sort(), gone = Object.keys(SW).filter((v) => !found.has(v)).sort();

const cell = (s) => s.replace(/\|/g, '\\|');
const page = `# The host interface, version 0 (not stable)

<!-- generated by tools/gen-host-spec.mjs from the sources: do not edit; tests/run.sh spec checks it is current -->

What a host gives an app's module, and what a module asks of it. An image says which version it was built for
(\`abi\` in its config: \`pisco.host/0\`); a host must refuse one it does not know. **Version 0 may change in any pull
request**: it becomes version 1, and stable, when a second host (the native one of [docs/hosts.md](../hosts.md))
runs the same images as the browser and Node.

A module is WebAssembly with threads: it imports \`env.memory\` (shared, up to 4 GiB), \`wasi.thread-spawn\`, the
WASI functions below (\`wasi_snapshot_preview1\`) and the \`host\` functions below, and exports \`wasi_thread_start\`,
\`rt_last_pc\` and \`__heap_base\`. A program in a module of its own (a side module, \`dylink.0\`) imports the first
module's functions, memory and table ([web/side.mjs](../../web/side.mjs)).

## \`host\` functions

${byName.size} functions, by the runtime that imports them (a module imports only its base's).

| Function | Signature (C) | Imported by | Means |
|---|---|---|---|
${[...byName.values()].sort((a, b) => a.name.localeCompare(b.name)).map((h) => `| \`${h.name}\` | \`${cell(h.sig)}\` | ${[...new Set(h.files.map(folder))].join(', ')} | ${cell(h.note)} |`).join('\n')}

## WASI functions

The Node host answers these ${wasi.length} of \`wasi_snapshot_preview1\` (the page's worker the same):
${wasi.map((w) => `\`${w}\``).join(' ')}.

## Rings

The GPU, sound and input go through rings in the module's memory, which a host reads and writes from other threads:
the GPU ring ([rt/gpu_ring.h](../../rt/gpu_ring.h), its records [rt/d3d12_cmd.h](../../rt/d3d12_cmd.h); the host's back
end [web/gpu/](../../web/gpu/)), the mixer's ring ([rt/mixer.h](../../rt/mixer.h)) and the input block
([rt/input.h](../../rt/input.h)); a machine's console and network rings (\`ConsoleRing\`, \`NetRing\`:
[rt/microvm-x64/machine.h](../../rt/microvm-x64/machine.h)). Their layouts are those headers' and are version 0 too.

## \`SW_*\` variables

Settings reach the runtime as environment variables. Their kinds: **config** (the host sets it from the image's
config) and **process** (the runtime passes it to a program an execve starts) are the interface; **run** is an option
of one run; **debug** is for finding a fault and may change at any time; **tool** is \`pisco\`'s or a script's own.

| Variable | Kind | Means |
|---|---|---|
${Object.entries(SW).filter(([, [k]]) => k !== 'none').sort(([a], [b]) => a.localeCompare(b)).map(([v, [k, t]]) => `| \`${v}\` | ${k} | ${cell(t)} |`).join('\n')}
`;
const out = path.join(root, 'docs/spec/host.md');
let bad = 0;
if (unclassed.length) { console.log(`SW_* variables with no row in tools/gen-host-spec.mjs: ${unclassed.join(' ')}`); bad = 1; }
if (gone.length) { console.log(`rows in tools/gen-host-spec.mjs for SW_* variables no source names: ${gone.join(' ')}`); bad = 1; }
if (process.argv.includes('--check')) {
  const have = fs.existsSync(out) ? fs.readFileSync(out, 'utf8') : '';
  if (have !== page) { console.log('docs/spec/host.md is not what the sources give: node tools/gen-host-spec.mjs'); bad = 1; }
  else if (!bad) console.log(`docs/spec/host.md is current: ${byName.size} host functions, ${wasi.length} WASI functions, ${Object.keys(SW).length - 2} SW_* variables, each with its kind`);
} else { fs.writeFileSync(out, page); console.log(`wrote ${rel(out)}`); }
process.exit(bad);
