---
title: A running Linux in one file
---
The lifted Linux can be snapshotted while it runs into one file, an AgentFS database (SQLite), and goes on from it in
another process: in Node, in the page, or one after the other. A shell typed into in Chrome went on in a terminal with
its files, its variables and a counter still counting in the background, then went back into Chrome.

- **The snapshot is the module's memory**, taken where no host stack holds state; going on drops every host stack, and
  each kernel task starts afresh from its own kernel stack when it is next switched to.
- **One file**: memory as content-addressed, gzipped 64 KiB chunks in AgentFS's file system, so a second snapshot
  writes only what changed (0.3 MiB after an 8.5 MiB first one); a log of snapshots and restores that `agentfs
  timeline` reads.
- **The page keeps it in OPFS** (Turso's wasm engine), with a panel: snapshot, go back to any snapshot, push to or pull
  from the server, download or open the file.

Still missing: Turso's cloud checked for real (it is wired), snapshots of a machine with a network.
