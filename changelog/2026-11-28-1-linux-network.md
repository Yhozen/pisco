---
title: A network for the lifted Linux
---
The lifted Linux reaches the network in Node (`SW_NET=1`): the `microvm-x64` machine has virtio's network device, and
unkernel's user-mode network stack (MIT) terminates its frames on the host. A shell gets a DHCP lease, looks a name up
and fetches a page from a web server on the host, as with QEMU's user networking.

- **virtio, for several devices**: the machine's MMIO transport now carries the disk and the network, a page and an
  interrupt line each, each announced on the kernel's command line.
- **Frames both ways**: the guest's go to the host as they are sent; the host's arrive in a ring in the module's
  memory, with a bell that wakes an idle machine.
- **Reused, not rewritten**: ARP, DHCP, DNS and TCP over the host's sockets are unkernel's (`web/third_party/unkernel/`).

Still missing: the network in the page.
