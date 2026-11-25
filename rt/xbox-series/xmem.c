// SPDX-License-Identifier: GPL-2.0-or-later
// XMem: the console's memory allocator, its GPU-visible region and its physical pages.
// Behaviour follows XCL's findings on retail titles (github.com/ivall/xcl, GPL-2.0).
#include "../gdk/gdk.h"

static u64 xmem_align(u64 attrs) {
  u32 a = (attrs >> 24) & 0x1F, page = (attrs >> 14) & 3;
  u64 al = a ? 1ull << a : 16;
  if (page == 2 && al < (4u << 20)) al = 4u << 20;      // XALLOC_PAGESIZE_4MB
  if (page == 1 && al < 65536) al = 65536;              // 64 KB pages
  return al;
}
// ---- graphics memory: the console's GPU-visible region, from 16 GiB up (XMemAlloc / XMemVirtualAlloc with a
// graphics memory type, D3D12.X descriptor heaps and buffers). wasm32 keeps only the low 32 bits of a guest
// address, so a block of the region is ordinary linear memory whose address carries a tag above bit 32: the
// title sees an address in the region, every access lands in the block. Tags 4..7 are XCL's four 4 GiB
// windows (window = address >> 32 & 3); window 3 is the coherent one readback and upload buffers use.
// Blocks come from arenas, first fit in 64 KiB units, so consecutive allocations are adjacent and freed
// neighbours merge, as on the console.
typedef struct Range { u64 at, size; struct Range *next; } Range;
typedef struct Arena { u64 base, size; Range *free; struct Arena *next; } Arena;
static Arena *arenas; static Range *gfx_used, *gfx_reserved;
static pthread_mutex_t gfx_lock = PTHREAD_MUTEX_INITIALIZER;
#define GFX_ARENA (256ull << 20)
#define GFX_UNBACKED (1ull << 30)     // reservations larger than this get address space only
static u64 gfx_va_next = 9ull << 32;  // where unbacked reservations go (window 1, above the blocks' tags)
static u64 gfx_carve(Arena *a, u64 size, u64 align) {
  for (Range **pr = &a->free; *pr; pr = &(*pr)->next) {
    Range *r = *pr; u64 at = (r->at + align - 1) & ~(align - 1);
    if (at + size > r->at + r->size) continue;
    u64 end = r->at + r->size;
    if (at > r->at) { r->size = at - r->at; if (at + size < end) { Range *t = malloc(sizeof *t); *t = (Range){at + size, end - at - size, r->next}; r->next = t; } }
    else if (at + size < end) { r->at = at + size; r->size = end - at - size; }
    else { *pr = r->next; free(r); }
    return at;
  }
  return 0;
}
u64 rt_gfx_alloc(u64 size, u64 align, u32 window) {
  size = (size + 0xFFFF) & ~0xFFFFull; if (align < 0x10000) align = 0x10000;
  pthread_mutex_lock(&gfx_lock);
  u64 at = 0;
  for (Arena *a = arenas; a && !at; a = a->next) at = gfx_carve(a, size, align);
  if (!at) {
    u64 n = size + align > GFX_ARENA ? size + align : GFX_ARENA;
    void *p = aligned_alloc(0x10000, (size_t)n);
    if (p) {
      Arena *a = calloc(1, sizeof *a); a->base = (uptr)p; a->size = n;
      a->free = malloc(sizeof(Range)); *a->free = (Range){a->base, n, 0};
      Arena **pa = &arenas; while (*pa) pa = &(*pa)->next; *pa = a; // older arenas first: addresses stay in order
      at = gfx_carve(a, size, align);
    }
  }
  if (at) { Range *u = malloc(sizeof *u); *u = (Range){at, size, gfx_used}; gfx_used = u; }
  pthread_mutex_unlock(&gfx_lock);
  if (!at) return 0;
  memset((void *)(uptr)at, 0, (size_t)size);
  return at | (u64)(4 + (window & 3)) << 32;
}
// the block or reservation `a` (tag ignored) starts, freed; 0 when `a` is not one
int rt_gfx_free(u64 a) {
  if (a >> 32 >= 8) { // an unbacked reservation
    pthread_mutex_lock(&gfx_lock);
    for (Range **pr = &gfx_reserved; *pr; pr = &(*pr)->next) if ((*pr)->at == a) { Range *r = *pr; *pr = r->next; free(r); pthread_mutex_unlock(&gfx_lock); return 1; }
    pthread_mutex_unlock(&gfx_lock); return 0;
  }
  if (a >> 32 < 4) return 0;
  u64 at = (u32)a;
  pthread_mutex_lock(&gfx_lock);
  Range *u = 0;
  for (Range **pr = &gfx_used; *pr; pr = &(*pr)->next) if ((*pr)->at == at) { u = *pr; *pr = u->next; break; }
  if (!u) { pthread_mutex_unlock(&gfx_lock); return 0; }
  for (Arena *ar = arenas; ar; ar = ar->next) {
    if (at < ar->base || at >= ar->base + ar->size) continue;
    Range **pr = &ar->free; while (*pr && (*pr)->at < at) pr = &(*pr)->next;
    u->next = *pr; *pr = u;
    // merge with the next range, then with the previous one
    if (u->next && u->at + u->size == u->next->at) { Range *n = u->next; u->size += n->size; u->next = n->next; free(n); }
    for (Range *p = ar->free; p && p != u; p = p->next) if (p->next == u && p->at + p->size == u->at) { p->size += u->size; p->next = u->next; free(u); break; }
    break;
  }
  pthread_mutex_unlock(&gfx_lock);
  return 1;
}
// is [a, a + n) inside a block (any tag)?
static int gfx_inside(u64 a, u64 n) {
  if (a >> 32 < 4 || a >> 32 >= 8) return 0;
  u64 at = (u32)a; int in = 0;
  pthread_mutex_lock(&gfx_lock);
  for (Range *u = gfx_used; u; u = u->next) if (at >= u->at && at + n <= u->at + u->size) { in = 1; break; }
  pthread_mutex_unlock(&gfx_lock);
  return in;
}
static int is_graphics(u64 attrs) { return ((attrs >> 29) & 0xF) != 0; } // XALLOC_ATTRIBUTES' memory type
IMP(kernelx, XMemAlloc) {
  u64 al = xmem_align(ARG(1)), n = ARG(0) ? ARG(0) : 1;
  if (is_graphics(ARG(1))) { RET(rt_gfx_alloc(n, al, 0)); return; }
  void *p = aligned_alloc(al, (n + al - 1) & ~(al - 1));
  if (p) memset(p, 0, n);
  RET((uptr)p);
}
IMP(kernelx, XMemFree) { if (!rt_gfx_free(ARG(0))) free((void *)GP(ARG(0))); }
IMP(kernelx, XMemAllocDefault) { imp_kernelx_XMemAlloc(c); }
IMP(kernelx, XMemFreeDefault) { imp_kernelx_XMemFree(c); }
IMP(kernelx, XMemSetAllocationHooks) {}
IMP(kernelx, XMemVirtualAlloc) { // (address, size, type, XALLOC_ATTRIBUTES, protect) -> VirtualAlloc(address, size, type, protect)
  u64 addr = ARG(0), size = ARG(1);
  if (addr && gfx_inside(addr, size)) { RET(addr); return; } // a commit inside a block: already there
  if (is_graphics(ARG(3)) && !addr) {
    if (size > GFX_UNBACKED) {
      // wasm32 has 4 GiB in all: a reservation this large gets addresses that nothing backs (a commit in it fails)
      pthread_mutex_lock(&gfx_lock);
      u64 va = gfx_va_next; gfx_va_next += (size + 0xFFFFFF) & ~0xFFFFFFull;
      Range *r = malloc(sizeof *r); *r = (Range){va, size, gfx_reserved}; gfx_reserved = r;
      pthread_mutex_unlock(&gfx_lock);
      rt_log("XMemVirtualAlloc: a graphics reservation of %llu MiB gets address space only (0x%llx)", size >> 20, va);
      RET(va); return;
    }
    RET(rt_gfx_alloc(size, xmem_align(ARG(3)), 0)); return;
  }
  extern void imp_kernel32_VirtualAlloc(Cpu *);
  u64 prot = ARG(4); M64(R[RSP] + 0x20) = prot; R[9] = prot; imp_kernel32_VirtualAlloc(c);
}


// ---- XMem physical pages: each page has a home buffer; mapping it somewhere copies its data there, and
// mapping it elsewhere (or unmapping it) copies the data back first. A page is in one place at a time. ----
#define PAGE 0x10000ull
typedef struct { u8 *home; u64 at; int used; } Phys;
static Phys *phys; static u64 phys_n;
static pthread_mutex_t phys_lock = PTHREAD_MUTEX_INITIALIZER;
static void page_unmap(Phys *p) { if (p->at) { memcpy(p->home, (void *)GP(p->at), PAGE); p->at = 0; } }
IMP(xmem, XMemGetWorkingSetStatistics) { M64(ARG(1)) = 13824ull << 20; M64(ARG(1) + 8) = 512ull << 20; RET(1); }
IMP(xmem, XMemAllocatePhysicalPages) { // (attributes, &count, pages[])
  u64 n = M64(ARG(1));
  pthread_mutex_lock(&phys_lock);
  for (u64 i = 0; i < n; i++) {
    phys = realloc(phys, sizeof(Phys) * (size_t)(phys_n + 1));
    phys[phys_n] = (Phys){calloc(1, PAGE), 0, 1};
    M64(ARG(2) + 8 * i) = ++phys_n; // page numbers start at 1
  }
  pthread_mutex_unlock(&phys_lock);
  RET(1);
}
// where physical page `num` (1-based) is now: its mapping, or its home buffer (DirectStorage writes there)
u8 *rt_phys_page(u64 num) {
  pthread_mutex_lock(&phys_lock);
  u8 *p = num && num <= phys_n ? (phys[num - 1].at ? (u8 *)(uptr)phys[num - 1].at : phys[num - 1].home) : 0;
  pthread_mutex_unlock(&phys_lock);
  return p;
}
IMP(xmem, XMemMapPhysicalPages) { // (address, count, pages[] or null to unmap)
  u64 va = ARG(0), n = ARG(1), pages = ARG(2);
  if (!va) { va = rt_gfx_alloc(n * PAGE, PAGE, 0); if (!va) { RET(0); return; } } // the GPU-visible region
  pthread_mutex_lock(&phys_lock);
  for (u64 i = 0; i < n; i++) {
    u64 at = (uptr)GP(va + i * PAGE); // pages are tracked by where they are in linear memory
    for (u64 k = 0; k < phys_n; k++) if (phys[k].at == at) page_unmap(&phys[k]); // what was mapped there goes home
    u64 num = pages ? M64(pages + 8 * i) : 0;
    if (!num || num > phys_n) { memset((void *)GP(at), 0, PAGE); continue; }
    Phys *p = &phys[num - 1];
    page_unmap(p);
    memcpy((void *)GP(at), p->home, PAGE); p->at = at;
  }
  pthread_mutex_unlock(&phys_lock);
  RET(va);
}
IMP(xmem, XMemFreePhysicalPages) {
  pthread_mutex_lock(&phys_lock);
  for (u64 i = 0; i < ARG(0); i++) { u64 num = M64(ARG(1) + 8 * i); if (num && num <= phys_n) { phys[num - 1].used = 0; phys[num - 1].at = 0; } }
  pthread_mutex_unlock(&phys_lock);
  RET(1);
}
