// SPDX-License-Identifier: GPL-2.0-or-later
// virtio's block device on its MMIO transport (virtio 1.2, sections 4.2 and 5.2), at 0xd0000000 on the IOAPIC's pin
// 5: the kernel finds it on its command line (virtio_mmio.device=512@0xd0000000:5, which the runtime adds when there
// is a disk). The disk: SW_DISK=<file> (a file beside the kernel, read whole) or SW_DISK=<n>M (n MiB of zeros). Its
// writes are kept in this memory: the file is not changed. One queue; requests are done when the kernel notifies.
#include "machine.h"

HOST("stat") s64 host_stat(const char *name, u32 len);
enum { BASE_PA = 0xD0000000u, QSIZE = 256, SECTOR = 512 };
static u8 *disk; static u64 disk_size;
static u32 status, isr, dev_feat_sel, drv_feat_sel, queue_ready, queue_num = QSIZE, config_gen;
static u64 drv_features, desc, avail, used; static u16 last_avail;
static const u64 FEATURES = 1ull << 32 | 1ull << 6 | 1ull << 9;   // VERSION_1, BLK_SIZE, FLUSH

int virtio_present(void) { return disk != 0; }
u64 virtio_base(void) { return BASE_PA; }
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
}

static void irq(void) { ioapic_line(5, isr != 0); }
static void pread_(u64 pa, void *b, unsigned n) { phys_read(pa, b, n); }
static void pwrite_(u64 pa, const void *b, unsigned n) { phys_write(pa, b, n); }

// a request: a header (type, sector), the data, a status byte; each a descriptor of the chain
static u32 request(u16 head) {
  struct { u64 addr; u32 len; u16 flags, next; } d[64]; unsigned n = 0; u16 i = head;
  for (;;) { if (n == 64) rt_fatal("virtio: a request of more than 64 descriptors"); pread_(desc + 16ull * i, &d[n], 16); if (!(d[n].flags & 1)) { n++; break; } i = d[n].next; n++; }
  if (n < 2) rt_fatal("virtio: a request without a status");
  struct { u32 type, reserved; u64 sector; } h; pread_(d[0].addr, &h, 16);
  u64 at = h.sector * SECTOR; u32 done = 0; u8 st = 0;
  for (unsigned k = 1; k + 1 < n; k++) {
    u32 len = d[k].len;
    if (h.type == 0 || h.type == 1) {
      if (at + len > disk_size) { st = 1; break; }
      if (h.type == 0) pwrite_(d[k].addr, disk + at, len); else pread_(d[k].addr, disk + at, len);
      at += len; if (h.type == 0) done += len;
    } else if (h.type == 8) { static const char id[20] = "pisco-disk"; pwrite_(d[k].addr, id, len < 20 ? len : 20); done += len < 20 ? len : 20; }
    else if (h.type != 4) st = 2;   // (flush: nothing to do; anything else is not supported)
  }
  pwrite_(d[n - 1].addr, &st, 1);
  return done + 1;
}
static void notify(void) {
  if (!queue_ready) return;
  u16 idx; pread_(avail + 2, &idx, 2);
  while (last_avail != idx) {
    u16 head; pread_(avail + 4 + 2ull * (last_avail % queue_num), &head, 2);
    u32 len = request(head);
    u16 uidx; pread_(used + 2, &uidx, 2);
    u32 elem[2] = { head, len }; pwrite_(used + 4 + 8ull * (uidx % queue_num), elem, 8);
    uidx++; pwrite_(used + 2, &uidx, 2);
    last_avail++;
  }
  isr |= 1; irq();
}

u32 virtio_read(u32 off) {
  switch (off) {
  case 0x000: return 0x74726976; case 0x004: return 2; case 0x008: return 2; case 0x00C: return 0x4F435349;   // "virt", v2, block, "PISC"
  case 0x010: return (u32)(FEATURES >> (32 * (dev_feat_sel & 1))) * (dev_feat_sel < 2);
  case 0x034: return QSIZE; case 0x044: return queue_ready;
  case 0x060: return isr; case 0x070: return status; case 0x0FC: return config_gen;
  case 0x100: return (u32)(disk_size / SECTOR); case 0x104: return (u32)(disk_size / SECTOR >> 32);   // capacity, in sectors
  case 0x114: return SECTOR;   // blk_size
  }
  return 0;
}
void virtio_write(u32 off, u32 v) {
  switch (off) {
  case 0x014: dev_feat_sel = v; break;
  case 0x020: if (drv_feat_sel < 2) drv_features = (drv_features & ~(0xFFFFFFFFull << (32 * drv_feat_sel))) | (u64)v << (32 * drv_feat_sel); break;
  case 0x024: drv_feat_sel = v; break;
  case 0x030: break;   // one queue
  case 0x038: queue_num = v ? v : QSIZE; break;
  case 0x044: queue_ready = v & 1; break;
  case 0x050: notify(); break;
  case 0x064: isr &= ~v; irq(); break;
  case 0x070: status = v; if (!v) { queue_ready = 0; last_avail = 0; isr = 0; drv_features = 0; irq(); } break;   // a reset
  case 0x080: desc = (desc & ~0xFFFFFFFFull) | v; break;  case 0x084: desc = (desc & 0xFFFFFFFFull) | (u64)v << 32; break;
  case 0x090: avail = (avail & ~0xFFFFFFFFull) | v; break; case 0x094: avail = (avail & 0xFFFFFFFFull) | (u64)v << 32; break;
  case 0x0A0: used = (used & ~0xFFFFFFFFull) | v; break;  case 0x0A4: used = (used & 0xFFFFFFFFull) | (u64)v << 32; break;
  }
}
