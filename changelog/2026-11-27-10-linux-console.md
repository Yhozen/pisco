---
title: A Linux shell to type into, and a Linux image
---
The lifted Linux kernel's console now takes input, so its busybox shell can be typed into: in a terminal (`pisco run`), in the page (keys as a terminal sends them), or through a pipe. And `pisco build recipes/linux` makes it an image like any other title's. Two builds with empty caches give the same manifest, and `pisco run` boots it in Node and in headless Chrome.

- **The 16550 receives:** a ring in the module's memory, written by the host and read by the serial port with its receive interrupt. A byte is handed over only while the driver asks for it, because Linux's driver reads the receiver while it probes.
- **Idle waits:** when someone types, `hlt` waits for a key or the next timer on the host's clock; from a pipe the virtual clock stays.
- **A kernel oops fixed:** a signal frame written with `fxsave` into a copy-on-write page reported its fault at address 0, because `rt/cpu.h`'s helpers that reach memory saw a fallback instruction address. They now take the caller's, for every title.
- **`pisco build` for a machine:** `[lifter] programs` are lifted with the kernel; `[run] initrd` and `cmdline` reach the runtime through the image's config.
- **`tools/run-chrome.mjs`** can type (`TYPE=<text>`). `tests/run.sh kernel` checks the image, its programs against QEMU, and a typed shell session.

Still missing: disks and a network (virtio), programs at the same addresses as one another, and more than one CPU.
