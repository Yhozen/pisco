// SPDX-License-Identifier: GPL-2.0-or-later
// virtio's network device (virtio 1.2, section 5.1) on the transport of virtio.c, with SW_NET=1: Ethernet frames to
// and from the host, which terminates them (web/net-node.mjs: unkernel's user-mode network stack; the guest is
// 10.0.2.15 behind a gateway at 10.0.2.2 and a name server at 10.0.2.3, as with QEMU's user networking). Queue 0
// receives, queue 1 transmits. A frame the guest sends goes to the host at once (net_send). A frame for the guest
// the host writes into a ring in this memory, and the machine takes it when it polls, into a buffer the driver made
// available. No offloads (the guest computes whole checksums) and no merged buffers: a frame and its header fit in
// one buffer.
//
// While the machine idles it waits on the ring's bell, which the host rings for a frame and for a key typed, and
// a network's peers are on the host's clock: the machine is paced (run.c).
#include "machine.h"

HOST("net_open") void host_net_open(u32 ring);               // the host writes frames for the guest there
HOST("net_send") void host_net_send(u32 frame, u32 len);     // a frame the guest sends
enum { HDR = 12, MAX_FRAME = 65536 };                         // virtio_net_hdr_v1; a frame without offloads is far smaller
static NetRing *ring;
static const u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static u8 buf[HDR + MAX_FRAME];

static void notify(VDev *d, unsigned q);
static u32 config(VDev *d, u32 off) {
  (void)d;
  if (off == 0) return mac[0] | mac[1] << 8 | mac[2] << 16 | (u32)mac[3] << 24;
  if (off == 4) return mac[4] | mac[5] << 8;   // (and no link status: the link is up)
  return 0;
}
static VDev dev = { .id = 1, .features = 1ull << 5, .nq = 2, .q = { { .num = 256 }, { .num = 256 } }, .config = config, .notify = notify };   // MAC

int net_present(void) { return ring != 0; }
volatile u32 *net_bell(void) { return ring ? &ring->bell : 0; }

static void transmit(void) {
  VQueue *q = &dev.q[1]; u16 head; int any = 0;
  while (virtio_pop(q, &head)) {
    VDesc d[64]; unsigned n = virtio_chain(q, head, d, 64); u32 len = 0;
    for (unsigned k = 0; k < n; k++) {
      if (d[k].flags & 2) continue;
      u32 m = d[k].len < sizeof buf - len ? d[k].len : (u32)sizeof buf - len;
      phys_read(d[k].addr, buf + len, m); len += m;
    }
    if (len > HDR) host_net_send((u32)(uptr)(buf + HDR), len - HDR);
    virtio_push(q, head, 0); any = 1;
  }
  if (any) virtio_interrupt(&dev);
}
void net_poll(void) {
  if (!ring) return;
  VQueue *q = &dev.q[0]; int any = 0; u16 head;
  while (ring->r != __atomic_load_n(&ring->w, __ATOMIC_ACQUIRE) && virtio_pop(q, &head)) {
    u32 at = ring->r % ring->cap, len; memcpy(&len, ring->data + at, 4); at = (at + 4) % ring->cap;
    u32 n = len < MAX_FRAME ? len : MAX_FRAME;
    memset(buf, 0, HDR); buf[10] = 1;   // the header: no offloads, one buffer
    for (u32 k = 0; k < n; k++) buf[HDR + k] = ring->data[(at + k) % ring->cap];
    __atomic_store_n(&ring->r, ring->r + 4 + ((len + 3) & ~3u), __ATOMIC_RELEASE);
    VDesc d[64]; unsigned nd = virtio_chain(q, head, d, 64); u32 put = 0, total = HDR + n;
    for (unsigned k = 0; k < nd && put < total; k++) {
      if (!(d[k].flags & 2)) continue;
      u32 m = d[k].len < total - put ? d[k].len : total - put;
      phys_write(d[k].addr, buf + put, m); put += m;
    }
    virtio_push(q, head, put); any = 1;
  }
  if (any) virtio_interrupt(&dev);
}
static void notify(VDev *d, unsigned q) { (void)d; if (q == 1) transmit(); else net_poll(); }

void net_init(void) {
  const char *e = getenv("SW_NET"); if (!e || !*e || !strcmp(e, "0")) return;
  ring = aligned_alloc(64, sizeof *ring); if (!ring) rt_fatal("machine: no memory for the network's ring");
  memset(ring, 0, sizeof *ring); ring->cap = sizeof ring->data;
  host_net_open((u32)(uptr)ring);
  virtio_add(&dev);
}
