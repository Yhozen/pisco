# The machine: `microvm-x64` (experimental)

**Experimental: this page may change in any pull request, without a new schema.** A builder must say, when it
builds an app of this base, that the base is experimental.

`base/microvm-x64` is a small x86-64 machine (long mode, page tables, a local APIC, a serial port, virtio's disk and network), so
that the app is an operating system's kernel and the programs it runs: Linux 6.18 and busybox today
([docs/linux.md](../../linux.md)).

## `[run]`

| Key | Type | Required | Means |
|---|---|---|---|
| `kernel` | string | yes | the kernel the machine boots: an ELF on the drive (`/vmlinux`) |
| `initrd` | string | no | the kernel's initramfs, on the drive (`/initramfs.cpio`) |
| `cmdline` | string | no | the kernel's command line |

## The drive

The app's files are the machine's boot files (the kernel, the initramfs), not a file system the app sees: the kernel
sees the initramfs, and a disk if the host gives it one. Programs the kernel runs are found in the initramfs.

An app that may use the machine's network device says `[permissions] network = true`
([recipe.md](../recipe.md#permissions)); a host gives the device to no other.

## Not in the specification

What `pisco` does for this base is its own: `[hints.pisco] programs` and `modules` (programs compiled ahead of time
into the kernel's module, or into modules of their own), lifting a program when it first runs, the disk
(`SW_DISK`), how the network is given (`SW_NET`). [docs/linux.md](../../linux.md) describes them.
