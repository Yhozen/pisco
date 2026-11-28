---
date: 2026-11-28
read:
  agentfs: https://github.com/tursodatabase/agentfs 0a014eb (2026-06-03, v0.6.4); npm agentfs-sdk 0.6.4
  turso: npm @tursodatabase/database 0.4.4 (agentfs-sdk's), database-wasm 0.8.4, sync 0.8.4, sync-wasm 0.8.4
  unkernel: https://github.com/pithings/unkernel c7ad163 (2026-11-28)
  v86: https://github.com/copy/v86 (src/state.js, src/browser/starter.js), read 2026-11-28
status: a proof of concept in this pull request (rt/microvm-x64/snapshot.c, web/state.mjs, web/browser/state.js)
---

# An app's state as one file: AgentFS, Turso and snapshots of a running machine

The question: can everything an app is while it runs be one file, a SQLite database, that lives in OPFS in the page,
in a file on a computer, or in Turso's cloud, so that the app goes on wherever the file is opened? For the machine
runtime (Linux, lifted: [linux.md](../linux.md)) the answer in this pull request is yes. A running Linux, with its
shell, its processes and its tmpfs, is snapshotted into an AgentFS database and goes on from it in another process,
in Node or in the page. This page reports what was read before that design and why it took that shape.

## Short answer

- **AgentFS is the file's format.** It is a file system (inodes, directory entries, 4 KiB data chunks), a key-value
  store and a log of calls, all in one SQLite file (its `SPEC.md`, v0.4). Its TypeScript SDK runs in Node on Turso's
  engine and in the page on Turso's wasm build. Its CLI reads the same file: `agentfs fs <file> ls`, `agentfs
  timeline <file>`, and it can mount the file with FUSE. Nothing in AgentFS is specific to agents.
- **A snapshot needs a moment when no host stack holds state.** That is unkernel's lesson (it "parks" every task and
  then snapshots), and PISCO's machine already had what it takes. Lifted code can be entered at any return address,
  a task that was never run starts on a fresh host stack, and every parked task left from `__switch_to_asm`, its
  registers on the kernel's own stack. A snapshot is the module's memory taken at a poll, at a function's start or
  at a halt, when no task waits in a fault's handler. Going on from it means putting the memory back into a new
  instance and dropping every host stack. Each task then starts afresh when it is next switched to.
- **The memory is stored as content-addressed 64 KiB chunks.** Chunks of zeros are left out, and each one is gzipped
  and stored once as `/machine/chunks/<sha256>`, as unkernel's `KWSNAP1` and v86's state leave out zeros. A
  snapshot's manifest lists the chunks it holds. A second snapshot writes only the chunks that changed, which also
  suits Turso's sync, since it pushes changed rows.
- **Turso's cloud is how the file crosses devices**: the page's file and Node's can each be a replica of one database
  (`@tursodatabase/sync`, `sync-wasm`: `pull()`, `push()`). It is wired here, but no cloud database was at hand to
  check it. The development server does the same for one machine: `/state/<file>`, which the page pushes and pulls and
  Node reads.

## AgentFS

- **What it is.** One SQLite database holds a file system (`fs_inode`, `fs_dentry`, `fs_data(ino, chunk_index, data)`
  with 4 KiB chunks, `fs_symlink`), a key-value store (`kv_store`, values as JSON text) and an insert-only log of calls
  (`tool_calls`). The overlay tables (`fs_whiteout`, `fs_origin`) exist only in its Rust and Go SDKs. The
  licence is MIT by its packages and README; the repository has no LICENSE file.
- **The SDKs.** TypeScript (`agentfs-sdk`), Rust, Python, Go. The TypeScript SDK's browser build has only
  `AgentFS.openWith(db)`: the page brings the database, here `@tursodatabase/database-wasm`'s `connect(name)`,
  which keeps the file in OPFS. Node's build is `AgentFS.open({ path })` on Turso's native engine.
- **Sync.** Only the Rust SDK and the CLI push and pull themselves. In TypeScript, a `sync-wasm` (page) or `sync`
  (Node) database is handed to `openWith()`. That is what this pull request does, but it has not been checked against
  a cloud database.
- **What it does not have**: snapshots or branches of its own, and no overlay in TypeScript. Both are left to us. A
  machine's snapshots are files in it, and their parents make a tree.
- **Caveats met.** `writeFile` runs no transaction of its own, so a snapshot wraps its writes in one. The SDK's own
  dependency is Turso's engine 0.4.4, while its wasm build is 0.8.4; both read and write the same SQLite 3 file in
  WAL mode. `PRAGMA wal_checkpoint(TRUNCATE)` before closing gives one file that can be copied whole. The page's
  database is a flat name in the OPFS root, and Turso's worker holds it while it is open.

## Turso in the page

- `@tursodatabase/database-wasm` runs Turso's engine (Rust, `wasm32-wasip1-threads`) in a worker of its own, which
  owns the OPFS files (`<name>`, `<name>-wal`, sync access handles). It needs cross-origin isolation, which the page
  already has for shared memory. Its `bundle` build is one ES module with the worker and the wasm inlined, so
  `tools/setup-agentfs.sh` bundles it with the SDK into one file, `out/npm/agentfs-browser.js` (16 MB, served by
  `tools/serve.mjs` below `/npm/`).
- `sync-wasm`'s `connect({ path, url, authToken })` gives the same database with `pull()` and `push()`. It keeps its
  metadata in `localStorage`, so it runs on the page's main thread, where the state is handled anyway.

## unkernel's snapshots, and what was taken from them

unkernel (Linux compiled to wasm, each kernel task on a JSPI stack of its own) saves and restores a running machine:

- **Stacks cannot be saved**, so before a snapshot every task is brought to a "park" point where all of its state is
  in wasm memory (its patches 0027 and 0048, `.agents/kernel/parking.md`); the old stacks are dropped and each task
  goes on later on a new one.
- **Memory**: free pages left out, and caches dropped first (593 to 65 MiB in their test); a JSON header and the
  non-zero 64 KiB chunks (`js/core/snapshot.ts`).
- **Devices** save their own state, and a snapshot is refused while a queue is busy. **Disks are not copied**: a disk
  records an id and a write counter, and a restore refuses a disk written since.

PISCO's machine needed far less. Its tasks park only in `__switch_to_asm`, and the machine starts a task that was
never run by entering the code after the stack switch on a new host stack. A parked task looks just like that: its
callee-saved registers and its return addresses are on its kernel stack, and the lifter makes every return address
an entry (`c->resume`). What a fresh stack cannot do is go back into the middle of a lifted function. A fault's
handler returns to the faulting instruction, and only the frame that raised it can go on there, so a snapshot waits
until no task is inside such a handler (`machine_resumable`, by a flag on each level). Interrupts return to a poll
(a function's start or a loop's head), system calls to the instruction after `syscall`, and a halt now to the
instruction after `hlt`, made an entry for this. Devices need no saving: the 16550, the APIC, virtio's queues and
the disk's bytes are all in the module's memory, and so is the runtime's own C heap.

What is outside memory, and what is done about it:

| Outside the memory | Done |
|---|---|
| the host stacks (JSPI) of every task | dropped; each task starts afresh when it is switched to (run.c `machine_restart`) |
| `__stack_pointer`, `__tls_base` (wasm globals) | the restore runs on the module's own C stack; the thread pointer is kept in memory and set back |
| the table slots and memory of programs in modules of their own (bash) | loaded again in the same order before the memory is put back: the same slots, the same addresses |
| programs lifted while the machine ran (Node) | not kept: a snapshot is refused after one |
| the host's clock | the machine's clock goes on from its own time (apic.c `apic_clock_restored`) |
| the console's screen (the host's terminal) | the last 64 KiB of output kept with the snapshot and shown again |
| a network's peers (their connections are the host's sockets) | not kept: no snapshot of a machine with a network |

## Prior art for a running machine in a file

- **v86** saves and restores its emulated PC (`save_state`, `restore_state`, an `initial_state` to boot from). It has
  no suspended stacks, because its CPU is an interpreter and a JIT over a flat memory.
- **CheerpX** keeps copy-on-write disks in IndexedDB; no snapshot of a running machine was found.
- **container2wasm** pre-initialises the emulator at build time (Wizer); it has no snapshot at run time.
- None keeps a machine in SQLite, and none moves one between a browser and a native process. unkernel's IndexedDB
  session is the closest.

## What was not done

- **The guest's own files in AgentFS.** The machine's disk and tmpfs live in its memory, in the snapshot's chunks, not
  as AgentFS files. A virtio-9p (or virtio-fs) device whose files are AgentFS's would let the guest mount the database
  and `agentfs mount` show the same tree on the host.
- **The other runtimes.** The Windows and console runtimes keep what an app saves through `store_save` (OPFS, or
  `SW_SAVES`), which could be AgentFS files just as easily. A snapshot of a running game would also need its GPU's
  state, which lives in the host's back end and not in memory.
- **Turso's cloud** wired but not checked; **dropping caches** before a snapshot (unkernel's 593 to 65 MiB) not done.
