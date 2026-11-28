// SPDX-License-Identifier: GPL-2.0-or-later
// virtio's MMIO transport (virtio 1.2, section 4.2) for the machine's devices, a page each from 0xd0000000 and an
// IOAPIC pin each from 5, in the order they are added: the block device (below) and the network device (net.c). The
// kernel finds them on its command line (virtio_mmio.device=4K@<page>:<pin>, which the runtime adds for each).
// Requests are done when the kernel notifies (or, for frames that arrive, when the machine polls: net.c).
//
// The block device (section 5.2): SW_DISK=<file> (a file beside the kernel, read whole) or SW_DISK=<n>M (n MiB of
// zeros). Its writes are kept in this memory: the file is not changed. One queue.
#include "machine.h"

enum { BASE_PA = 0xD0000000u, FIRST_PIN = 5, MAX_DEVS = 4, QSIZE = 256, SECTOR = 512 };
static VDev *devs[MAX_DEVS]; static unsigned n_devs;

void virtio_add(VDev *d) {
  if (n_devs == MAX_DEVS) rt_fatal("virtio: more than %d devices", MAX_DEVS);
  d->base = BASE_PA + 0x1000ull * n_devs; d->pin = FIRST_PIN + n_devs; devs[n_devs++] = d;
}
unsigned virtio_count(void) { return n_devs; }
VDev *virtio_dev(unsigned i) { return i < n_devs ? devs[i] : 0; }
VDev *virtio_at(u64 pa) { for (unsigned i = 0; i < n_devs; i++) if ((pa & ~0xFFFull) == devs[i]->base) return devs[i]; return 0; }

static void line(VDev *d) { ioapic_line((int)d->pin, d->isr != 0); }
void virtio_interrupt(VDev *d) { d->isr |= 1; line(d); }

// the next chain the driver made available on the queue (its head), if there is one
int virtio_pop(VQueue *q, u16 *head) {
  if (!q->ready) return 0;
  u16 idx; phys_read(q->avail + 2, &idx, 2);
  if (q->last == idx) return 0;
  phys_read(q->avail + 4 + 2ull * (q->last % q->num), head, 2); q->last++;
  return 1;
}
// its descriptors, in order (at most max)
unsigned virtio_chain(VQueue *q, u16 head, VDesc *d, unsigned max) {
  unsigned n = 0; u16 i = head;
  for (;;) {
    if (n == max) rt_fatal("virtio: a chain of more than %u descriptors", max);
    phys_read(q->desc + 16ull * (i % q->num), &d[n], 16);
    if (!(d[n++].flags & 1)) return n;
    i = d[n - 1].next;
  }
}
// the chain is done: `len` bytes written into it
void virtio_push(VQueue *q, u16 head, u32 len) {
  u16 uidx; phys_read(q->used + 2, &uidx, 2);
  u32 elem[2] = { head, len }; phys_write(q->used + 4 + 8ull * (uidx % q->num), elem, 8);
  uidx++; phys_write(q->used + 2, &uidx, 2);
}

u32 virtio_read(VDev *d, u32 off) {
  VQueue *q = &d->q[d->qsel < d->nq ? d->qsel : 0];
  switch (off) {
  case 0x000: return 0x74726976; case 0x004: return 2; case 0x008: return d->id; case 0x00C: return 0x4F435349;   // "virt", v2, the device, "PISC"
  case 0x010: { u64 f = d->features | 1ull << 32; return d->dev_sel < 2 ? (u32)(f >> (32 * d->dev_sel)) : 0; }   // (VERSION_1 always)
  case 0x034: return d->qsel < d->nq ? QSIZE : 0; case 0x044: return q->ready;
  case 0x060: return d->isr; case 0x070: return d->status; case 0x0FC: return d->gen;
  }
  return off >= 0x100 && d->config ? d->config(d, off - 0x100) : 0;
}
void virtio_write(VDev *d, u32 off, u32 v) {
  VQueue *q = &d->q[d->qsel < d->nq ? d->qsel : 0];
  switch (off) {
  case 0x014: d->dev_sel = v; break;
  case 0x020: if (d->drv_sel < 2) d->drv = (d->drv & ~(0xFFFFFFFFull << (32 * d->drv_sel))) | (u64)v << (32 * d->drv_sel); break;
  case 0x024: d->drv_sel = v; break;
  case 0x030: d->qsel = v; break;
  case 0x038: q->num = v ? v : QSIZE; break;
  case 0x044: q->ready = v & 1; break;
  case 0x050: if (v < d->nq && d->notify) d->notify(d, v); break;
  case 0x064: d->isr &= ~v; line(d); break;
  case 0x070: d->status = v;   // 0: a reset
    if (!v) { for (unsigned i = 0; i < d->nq; i++) d->q[i] = (VQueue){ .num = QSIZE }; d->isr = 0; d->drv = 0; d->qsel = 0; line(d); }
    break;
  case 0x080: q->desc = (q->desc & ~0xFFFFFFFFull) | v; break;   case 0x084: q->desc = (q->desc & 0xFFFFFFFFull) | (u64)v << 32; break;
  case 0x090: q->avail = (q->avail & ~0xFFFFFFFFull) | v; break; case 0x094: q->avail = (q->avail & 0xFFFFFFFFull) | (u64)v << 32; break;
  case 0x0A0: q->used = (q->used & ~0xFFFFFFFFull) | v; break;   case 0x0A4: q->used = (q->used & 0xFFFFFFFFull) | (u64)v << 32; break;
  }
}

// ---- the block device ----
HOST("stat") s64 host_stat(const char *name, u32 len);
static u8 *disk; static u64 disk_size;

// a request: a header (type, sector), the data, a status byte; each a descriptor of the chain
static u32 request(VQueue *q, u16 head) {
  VDesc d[64]; unsigned n = virtio_chain(q, head, d, 64);
  if (n < 2) rt_fatal("virtio: a request without a status");
  struct { u32 type, reserved; u64 sector; } h; phys_read(d[0].addr, &h, 16);
  u64 at = h.sector * SECTOR; u32 done = 0; u8 st = 0;
  for (unsigned k = 1; k + 1 < n; k++) {
    u32 len = d[k].len;
    if (h.type == 0 || h.type == 1) {
      if (at + len > disk_size) { st = 1; break; }
      if (h.type == 0) phys_write(d[k].addr, disk + at, len); else phys_read(d[k].addr, disk + at, len);
      at += len; if (h.type == 0) done += len;
    } else if (h.type == 8) { static const char id[20] = "pisco-disk"; phys_write(d[k].addr, id, len < 20 ? len : 20); done += len < 20 ? len : 20; }
    else if (h.type != 4) st = 2;   // (flush: nothing to do; anything else is not supported)
  }
  phys_write(d[n - 1].addr, &st, 1);
  return done + 1;
}
static void blk_notify(VDev *dev, unsigned qi) {
  VQueue *q = &dev->q[qi]; u16 head; int any = 0;
  while (virtio_pop(q, &head)) { virtio_push(q, head, request(q, head)); any = 1; }
  if (any) virtio_interrupt(dev);
}
static u32 blk_config(VDev *dev, u32 off) {
  (void)dev;
  switch (off) {
  case 0x00: return (u32)(disk_size / SECTOR); case 0x04: return (u32)(disk_size / SECTOR >> 32);   // capacity, in sectors
  case 0x14: return SECTOR;   // blk_size
  }
  return 0;
}
static VDev blk = { .id = 2, .features = 1ull << 6 | 1ull << 9, .nq = 1, .q = { { .num = QSIZE } }, .config = blk_config, .notify = blk_notify };   // BLK_SIZE, FLUSH

void virtio_init(void) {
  const char *e = getenv("SW_DISK"); if (!e || !*e) return;
  char *end; unsigned long long mib = strtoull(e, &end, 10);
  if (end != e && (*end == 'M' || *end == 'm') && !end[1]) { disk_size = mib << 20; disk = calloc(1, disk_size); }
  else {
    s64 n = host_stat(e, (u32)strlen(e)); if (n <= 0) rt_fatal("machine: no disk %s", e);
    disk_size = (u64)n & ~(u64)(SECTOR - 1); disk = calloc(1, disk_size ? disk_size : SECTOR);
    if (disk && host_read_file(e, (u32)strlen(e), 0, disk_size, (u64)(uptr)disk) != disk_size) rt_fatal("machine: cannot read the disk %s", e);
  }
  if (!disk) rt_fatal("machine: no memory for a disk of %llu bytes", disk_size);
  virtio_add(&blk);
}
