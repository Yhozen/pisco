# Developing on boat.dev sandboxes (2026-11-27, experimental)

**Experimental and optional.** Nothing requires it: `tests/run.sh` on your own machine (and CI) stays the check a
change passes. An agent or a person may use `tools/boat.sh` when it helps, mostly to run the slow steps on several
machines at once, or to try a change on Ubuntu's gcc 13, and may as well not.

The slow part of working on PISCO is compiling lifted C: a lifter change means clang again over every lifted
function of every app touched (a minute or two a program on four cores). [boat.dev](https://boat.dev) sandboxes
are bare-metal Ubuntu machines started in about a second, billed by the second, that keep their disk as a snapshot:
`tools/boat.sh` makes one with everything built (the snapshot `pisco-dev`), starts copies of it with this working
tree on them, and runs commands and tests there. It needs the `boat` CLI, signed in (`boat login`, or
`BOAT_DEV_API_KEY` in the environment).

```sh
tools/boat.sh bake                    # the snapshot pisco-dev, from this working tree (about 10 minutes)
id=$(tools/boat.sh new)               # a sandbox from it, with this working tree: about 15 s
tools/boat.sh run $id 'tests/run.sh linux'   # this working tree onto it again, then the command in ~/pisco
tools/boat.sh test                    # every test step on 7 sandboxes at once, from the snapshot: about 75 s
tools/boat.sh rm $id
```

Bake again when the toolchains or the third-party apps' builds change (`tools/setup-toolchain.sh`,
`tools/build-*.sh`): a sandbox from an old snapshot still gets this working tree, and `boat.sh test` builds the
lifter and pisco again from it, but the Linux apps' roots and pisco's cache are the snapshot's.

## Measured

On 2026-11-26/27, with the $20 plan (its largest size is `large`: 8 vCPU, 16 GB; `xlarge`, 16 vCPU, needs the $100
plan). "This workspace" is the 4-vCPU cloud VM the earlier work was done on.

| | this workspace (4 vCPU) | boat `large` (8 vCPU) |
|---|---|---|
| a machine ready | | 1.1 s new; 15.5 s from the snapshot with this working tree copied on |
| toolchains, cargo, the test fixtures, from nothing | about 10 minutes | 1 m 43 s; the whole bake (and fastfetch's and neofetch's builds, the tests, the snapshot) 9 m 56 s |
| `tools/build-app.sh` fastfetch (lift, compile, link) | 66 s | 36 s |
| `pisco build recipes/neofetch` from an empty cache (two programs) | 111 s | 27 s |
| the default test steps | 50 s (its objects already built) | 61 s (from nothing) |
| every test step (default, images, chrome, fastfetch, neofetch) | 15 to 20 minutes, one after another | 74 s on 7 sandboxes |

The tests were not what was slow: lifting and compiling is, and the tests that lift (`images`, `chrome`, the Linux
apps' steps) are the long ones. A second kind of speed came for nothing: the sandboxes are Ubuntu 24.04 with gcc
13, where this workspace has Amazon Linux with gcc 11, and the same programs built by the newer gcc showed three
lifter bugs at once (below).

## The first task done this way: cowsay

[cowsay](cowsay.md), a Perl script, ported from a sandbox started from the snapshot: perl and cowsay built from
their sources, lifted, imaged, and checked against the same perl run natively, in Node and in headless Chrome:
14 m 25 s from the first command to the last check passing. Three things went wrong on the way, each found and fixed
on the sandbox in minutes:

- perl's `Configure` on Ubuntu took glibc's libraries for musl's (fixed in `tools/build-cowsay.sh`: `-Dlibc`);
- gcc 13 marks a switch's jump `notrack` (Intel CET's prefix, a DS segment prefix to the decoder), and the lifter
  did not take that `jmp [table+index*8]` for a switch: perl's parser became a recursion again, too deep for a
  browser worker's stack;
- gcc 13 puts a jump table's block at the end of the function, far from the `cmp; jbe` that bounds it, and the
  lifter looked for the bound only just before the jump (busybox's `rpm`, which neofetch runs).

## Costs

The $20 plan includes about 555 sandbox-hours a month at the `default` size (`large` counts twice). A bake is about
ten minutes of one `large` sandbox; a full test run about 7 `large` sandboxes for two minutes; a working session on
one `large` sandbox an hour. A saved snapshot is included (ten per $20 of plan). Sandboxes stop by themselves one
hour after their start unless extended (`boat extend <id> --hours N`), and `boat.sh test` deletes its own.
