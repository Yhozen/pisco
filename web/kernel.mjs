// SPDX-License-Identifier: GPL-2.0-or-later
// The processes of a Linux title (rt/linux-x64/process.c), as both hosts keep them (web/run-node.mjs, and the page's
// workers: web/browser/worker.js). A process is an instance of its program's module with memory of its own, in a
// worker of its own; what joins them is here, in one SharedArrayBuffer every worker sees: the process table (who is
// running, who has ended and with what code) and the pipes (a ring of bytes each, and how many ends of each kind
// are open, in every process together). Every call blocks its worker with Atomics.wait, as a system call blocks.
// Starting a worker is the supervisor's (the main thread or the page): a worker asks for it with a message
// ({ fork } or { exec }), and the supervisor answers with startProcess.
const PROCS = 4096, PIPES = 256, RING = 65536;
// Int32 words: 0 the next pid, 1 the lock, 2 bumped when a process ends (waiters wait on it), then 4 a process
// (state: 0 none, 1 running, 2 ended, 3 reaped; its parent; its exit code), then 8 a pipe (in use; readers;
// writers; bytes read; bytes written; bumped on every change)
const P0 = 16, Q0 = P0 + 4 * PROCS, WORDS = Q0 + 8 * PIPES;
const EPIPE = -32, ECHILD = -10, EMFILE = -24;

export function createKernel() {
  const sab = new SharedArrayBuffer(4 * WORDS + PIPES * RING);
  const k = new Int32Array(sab, 0, WORDS);
  k[0] = 2;                                // pid 1 is the first process
  k[P0 + 4] = 1;                           // running, no parent
  return sab;
}

// The host functions of one process (the `host` import module's proc_* and pipe_*). `memory()` is its memory
// (its buffer may grow); `ask(message)` posts to the supervisor; `program(path)` says where a program's module is (or
// null: none); `end(code)` ends this worker.
export function processImports(sab, { pid, memory, ask, module, program, end }) {
  const k = new Int32Array(sab, 0, WORDS), ring = (id) => new Uint8Array(sab, 4 * WORDS + id * RING, RING);
  const lock = () => { while (Atomics.compareExchange(k, 1, 0, 1) !== 0) Atomics.wait(k, 1, 1, 1); };
  const unlock = () => { Atomics.store(k, 1, 0); Atomics.notify(k, 1, 1); };
  const proc = (p) => P0 + 4 * p, pipe = (id) => Q0 + 8 * id;
  const touch = (id) => { Atomics.add(k, pipe(id) + 5, 1); Atomics.notify(k, pipe(id) + 5); };
  const mem = () => new Uint8Array(memory().buffer);
  return {
    proc_pid: () => pid,
    proc_ppid: () => k[proc(pid) + 1],
    proc_fork: () => {
      lock(); const child = k[0]++; unlock();
      if (child >= PROCS) return -11;     // EAGAIN
      k[proc(child)] = 1; k[proc(child) + 1] = pid; k[proc(child) + 2] = 0;
      // the child's memory: this process's, as it is now (the parent waits in fork until this returns)
      const now = memory(), pages = now.buffer.byteLength / 65536;
      const copy = new WebAssembly.Memory({ initial: pages, maximum: 65536, shared: true });
      new Uint8Array(copy.buffer).set(new Uint8Array(now.buffer));
      ask({ fork: { pid: child, memory: copy, module } });
      return child;
    },
    // execve: the supervisor starts the program's module in a new worker with this pid, and ends this one
    proc_exec: (path, plen, args, alen, env, elen) => {
      const text = (p, n) => new TextDecoder().decode(mem().slice(p, p + n));
      const list = (p, n) => (n ? text(p, n).split('\0').slice(0, -1) : []);
      const settings = Object.fromEntries(list(env, elen).map((s) => [s.slice(0, s.indexOf('=')), s.slice(s.indexOf('=') + 1)]));
      const where = program(text(path, plen));
      if (!where) return -8;             // ENOEXEC: an ELF there, but no module was made of it
      ask({ exec: { pid, program: where, args: list(args, alen), settings } });
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);   // (the supervisor ends this worker)
      return 0;
    },
    proc_wait: (want, nohang, codeAt) => {
      for (;;) {
        lock();
        const gen = Atomics.load(k, 2); let any = false;
        for (let p = 2; p < k[0] && p < PROCS; p++) {
          const at = proc(p);
          if (k[at + 1] !== pid || (want > 0 && p !== want) || k[at] === 0 || k[at] === 3) continue;
          any = true;
          if (k[at] === 2) { k[at] = 3; unlock(); new DataView(memory().buffer).setUint32(codeAt, k[at + 2], true); return p; }
        }
        unlock();
        if (!any) return ECHILD;
        if (nohang) return 0;
        Atomics.wait(k, 2, gen);
      }
    },
    // the host's `exit`, for a process other than the first
    exit: (code) => {
      lock(); k[proc(pid)] = 2; k[proc(pid) + 2] = code; unlock();
      Atomics.add(k, 2, 1); Atomics.notify(k, 2);
      end(code);
    },
    pipe_new: () => {
      lock();
      for (let id = 0; id < PIPES; id++) {
        const at = pipe(id);
        if (k[at]) continue;
        k[at] = 1; k[at + 1] = 1; k[at + 2] = 1; k[at + 3] = 0; k[at + 4] = 0;
        unlock(); return id;
      }
      unlock(); return EMFILE;
    },
    pipe_ref: (id, end, delta) => {
      lock(); const at = pipe(id);
      k[at + 1 + end] += delta;
      if (k[at + 1] <= 0 && k[at + 2] <= 0) k[at] = 0;
      unlock(); touch(id);
    },
    pipe_read: (id, dst, n) => {
      const at = pipe(id), r = ring(id);
      for (;;) {
        lock();
        const seq = Atomics.load(k, at + 5), have = (k[at + 4] - k[at + 3]) | 0;
        if (have > 0) {
          const take = Math.min(n, have), m = mem(); let from = k[at + 3] & (RING - 1);
          for (let i = 0; i < take; i++) { m[dst + i] = r[from]; from = (from + 1) & (RING - 1); }
          k[at + 3] = (k[at + 3] + take) | 0; unlock(); touch(id); return take;
        }
        if (k[at + 2] <= 0) { unlock(); return 0; }   // no writer left: the end of the stream
        unlock(); Atomics.wait(k, at + 5, seq);
      }
    },
    pipe_write: (id, src, n) => {
      const at = pipe(id), r = ring(id);
      for (;;) {
        lock();
        const seq = Atomics.load(k, at + 5);
        if (k[at + 1] <= 0) { unlock(); return EPIPE; }
        const room = RING - ((k[at + 4] - k[at + 3]) | 0);
        if (room > 0) {
          const put = Math.min(n, room), m = mem(); let to = k[at + 4] & (RING - 1);
          for (let i = 0; i < put; i++) { r[to] = m[src + i]; to = (to + 1) & (RING - 1); }
          k[at + 4] = (k[at + 4] + put) | 0; unlock(); touch(id); return put;
        }
        unlock(); Atomics.wait(k, at + 5, seq);
      }
    },
    pipe_poll: (id, end) => {
      const at = pipe(id), have = (k[at + 4] - k[at + 3]) | 0;
      return end === 0 ? (have > 0 || k[at + 2] <= 0 ? 1 : 0) : (have < RING || k[at + 1] <= 0 ? 1 : 0);
    },
  };
}
