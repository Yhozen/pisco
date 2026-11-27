// SPDX-License-Identifier: GPL-2.0-or-later
// The machine's memory: RAM and the APIC's page, reached through the kernel's page tables (four levels, 4 KiB,
// 2 MiB and 1 GiB pages), with a software TLB for the kernel's code and one for user code (rt/cpu.h). A miss
// walks the tables, sets their accessed and dirty bits, and fills the entry; one that faults runs the kernel's
// page fault handler (cpu_deliver) and tries again when it comes back. An access to the APIC is never cached.
#include "machine.h"

TlbEntry mmu_k[4096], mmu_u[4096];
u8 *ram; u64 ram_size;

void mmu_flush(void) { memset(mmu_k, 0, sizeof mmu_k); memset(mmu_u, 0, sizeof mmu_u); }
void rt_invlpg(Cpu *c, u64 a) {
  (void)c; u64 tag = (a >> 12) + 1; unsigned i = (a >> 12) & 4095;
  if (mmu_k[i].rtag == tag || mmu_k[i].wtag == tag) mmu_k[i] = (TlbEntry){0};
  if (mmu_u[i].rtag == tag || mmu_u[i].wtag == tag) mmu_u[i] = (TlbEntry){0};
}

static inline int in_ram(u64 pa, unsigned n) { return pa < ram_size && n <= ram_size - pa; }
static u64 pt_read(u64 pa) { u64 v = ~0ull; if (in_ram(pa, 8)) memcpy(&v, ram + pa, 8); return v; }
static void pt_write(u64 pa, u64 v) { if (in_ram(pa, 8)) memcpy(ram + pa, &v, 8); }

void phys_read(u64 pa, void *out, unsigned n) {
  if (in_ram(pa, n)) { memcpy(out, ram + pa, n); return; }
  if ((pa & ~0xFFFull) == (mc.apic_base & ~0xFFFull)) { u32 v = apic_read((u32)(pa & 0xFF0)); memcpy(out, &v, n < 4 ? n : 4); if (n > 4) memset((u8 *)out + 4, 0, n - 4); return; }
  memset(out, 0xFF, n);
}
void phys_write(u64 pa, const void *in, unsigned n) {
  if (in_ram(pa, n)) { memcpy(ram + pa, in, n); return; }
  if ((pa & ~0xFFFull) == (mc.apic_base & ~0xFFFull)) { u32 v = 0; memcpy(&v, in, n < 4 ? n : 4); apic_write((u32)(pa & 0xFF0), v); return; }
}

int mmu_walk(u64 va, int write, int user, u64 *pa, int *writable) {
  int err = (write ? PF_W : 0) | (user ? PF_U : 0);
  if ((u64)((s64)(va << 16) >> 16) != va) return err | 0x8000;   // not canonical: a #GP (the caller says so)
  u64 table = mc.cr3 & 0x000FFFFFFFFFF000ull, e = 0, size = 4096; int rw = 1, us = 1, level;
  u64 at[4]; int n = 0;
  for (level = 3; level >= 0; level--) {
    u64 slot = table + ((va >> (12 + 9 * level)) & 511) * 8;
    e = pt_read(slot);
    if (!(e & 1)) return err;
    rw &= (e >> 1) & 1; us &= (e >> 2) & 1; at[n++] = slot;
    if (level > 0 && level < 3 && (e & 0x80)) { size = 1ull << (12 + 9 * level); break; }
    table = e & 0x000FFFFFFFFFF000ull;
  }
  if (user && !us) return err | PF_P;
  if (write && !rw && (user || (mc.cr0 & 0x10000))) return err | PF_P;
  // accessed bits on the way down, dirty on the page itself
  for (int k = 0; k < n; k++) { u64 v = pt_read(at[k]); u64 want = v | 0x20 | (k == n - 1 && write ? 0x40 : 0); if (want != v) pt_write(at[k], want); }
  u64 base = e & 0x000FFFFFFFFFF000ull & ~(size - 1);
  *pa = base + (va & (size - 1));
  *writable = rw && ((e & 0x40) || write) && (us || !user);
  if (!rw && !(mc.cr0 & 0x10000) && !user) *writable = 1;   // a kernel without CR0.WP writes anywhere
  return -1;
}

// the TLB entry of a page that is RAM; MMIO and pages beyond RAM are never cached
static void fill(TlbEntry *t, u64 va, u64 pa, int writable) {
  if (!in_ram(pa & ~0xFFFull, 4096)) return;
  TlbEntry *e = &t[(va >> 12) & 4095]; u64 tag = (va >> 12) + 1;
  e->rtag = tag; e->wtag = writable ? tag : 0;
  e->add = (u32)(uptr)(ram + (pa & ~0xFFFull)) - (u32)(va & ~0xFFFull);
}

// one access within a page: translated (the handler of a fault run until it comes back), then done
static void access(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip, void *buf, int write) {
  int user = t == mmu_u;
  for (int tries = 0;; tries++) {
    u64 pa; int writable;
    int r = mmu_walk(a, write, user, &pa, &writable);
    if (r < 0) {
      fill(t, a, pa, writable);
      if (write) phys_write(pa, buf, n); else phys_read(pa, buf, n);
      return;
    }
    if (r & 0x8000) rt_fatal("machine: a non-canonical address 0x%llx at 0x%llx", a, ip);
    if (tries > 8) rt_fatal("machine: the page fault at 0x%llx (address 0x%llx) does not go away", ip, a);
    mc.cr2 = a;
    cpu_deliver(c, 14, 1, (u32)r, ip, ip);
  }
}
void mmu_read(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip, void *out) {
  unsigned first = 4096 - (unsigned)(a & 4095);
  if (n <= first) { access(c, t, a, n, ip, out, 0); return; }
  access(c, t, a, first, ip, out, 0); access(c, t, a + first, n - first, ip, (u8 *)out + first, 0);
}
void mmu_write(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip, const void *in) {
  unsigned first = 4096 - (unsigned)(a & 4095);
  // both pages first, so that a fault on the second leaves memory as it was
  if (n > first) { u8 probe[16]; access(c, t, a + first, 1, ip, probe, 0); }
  if (n <= first) { access(c, t, a, n, ip, (void *)in, 1); return; }
  access(c, t, a, first, ip, (void *)in, 1); access(c, t, a + first, n - first, ip, (u8 *)in + first, 1);
}
uptr mmu_ptr(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip) {
  TlbEntry *e = &t[(a >> 12) & 4095];
  for (int tries = 0; tries < 2; tries++) {
    if (e->wtag == ((a + n - 1) >> 12) + 1) return (uptr)((u32)a + e->add);
    u8 tmp[16]; mmu_read(c, t, a, n, ip, tmp); mmu_write(c, t, a, n, ip, tmp);   // the page made present and dirty
  }
  rt_fatal("machine: an atomic access at 0x%llx (0x%llx) that is not in RAM or crosses pages", a, ip);
}

// the runtime's own accesses: the kernel's rights, no fault taken
void vread(u64 va, void *out, unsigned n) {
  while (n) {
    unsigned k = 4096 - (unsigned)(va & 4095); if (k > n) k = n;
    u64 pa; int w; if (mmu_walk(va, 0, 0, &pa, &w) >= 0) rt_fatal("machine: the runtime cannot read 0x%llx (not mapped)", va);
    phys_read(pa, out, k); va += k; out = (u8 *)out + k; n -= k;
  }
}
static void vwrite(u64 va, const void *in, unsigned n) {
  while (n) {
    unsigned k = 4096 - (unsigned)(va & 4095); if (k > n) k = n;
    u64 pa; int w; if (mmu_walk(va, 1, 0, &pa, &w) >= 0) rt_fatal("machine: the runtime cannot write 0x%llx (not mapped)", va);
    phys_write(pa, in, k); va += k; in = (const u8 *)in + k; n -= k;
  }
}
u64 vread64(u64 va) { u64 v; vread(va, &v, 8); return v; }
u16 vread16(u64 va) { u16 v; vread(va, &v, 2); return v; }
void vwrite64(u64 va, u64 v) { vwrite(va, &v, 8); }
