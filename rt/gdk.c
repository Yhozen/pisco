// The console's system libraries, offline: kernelx and XMem, XGameRuntime (task queues, XAsync, XUser, saves,
// persistent local storage, package, networking hints), the ntuser and DWM functions a title's window uses,
// and GameInput's object. Behaviour follows XCL's findings on retail titles (github.com/ivall/xcl, GPL-2.0).
#include <sched.h>
#include <time.h>
#include "rt.h"
#include "vfs.h"

#define S_OK 0
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_PENDING 0x8000000Au
#define E_ABORT 0x80004004u
#define E_INVALIDARG 0x80070057u
#define E_GAMEUSER_NO_NETWORK 0x80072EFDu
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg); // process.c: a guest-capable thread running native code
static u64 now_us(void) { return (u64)host_now_us(); }

// ---- kernelx ----
IMP(kernelx, GetConsoleType) { RET(6); }                       // Xbox Series X
IMP(kernelx, GetSystemOSVersion) { u64 p = ARG(0); M8(p) = 10; M8(p + 1) = 0; M16(p + 2) = 25398; M16(p + 4) = 0; }
IMP(kernelx, TitleMemoryStatus) {
  u64 p = ARG(0), total = 13824ull << 20;
  // dwLength, dwReserved, then ullTotalMem, ullAvailMem, legacy..., title, ...
  for (int i = 0; i < 9; i++) M64(p + 8 + 8 * i) = total / (i ? 2 : 1);
  RET(1);
}
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
IMP(kernelx, SetThreadName) {
  Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_THREAD) { RET(0); return; }
  free(o->thread->name); o->thread->name = rt_narrow(GWSTR(ARG(1)));
  RET(1);
}
IMP(kernelx, GetThreadName) { // (thread, buffer, length, &returned)
  Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_THREAD) { RET(0); return; }
  const char *n = o->thread->name ? o->thread->name : "";
  u64 l = strlen(n), cap = ARG(2);
  for (u64 i = 0; i < l && i + 1 < cap; i++) GWSTR(ARG(1))[i] = (u8)n[i];
  if (cap) GWSTR(ARG(1))[l < cap ? l : cap - 1] = 0;
  if (ARG(3)) M64(ARG(3)) = l;
  RET(1);
}
IMP(kernel32, SetThreadDescription) { char *n = rt_narrow(GWSTR(ARG(1))); Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0)); if (o && o->type == OBJ_THREAD) { free(o->thread->name); o->thread->name = n; } else free(n); RET(0); }

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

// ---- XTaskQueue: a queue has a work port and a completion port; a composite queue uses other queues' ports ----
enum { MODE_MANUAL = 0, MODE_THREADPOOL = 1, MODE_SERIALIZED = 2, MODE_IMMEDIATE = 3 };
typedef struct Item { u64 owner, callback, context, due; struct Item *next; } Item;
typedef struct Port { int mode; Item *items; pthread_mutex_t m; pthread_cond_t cv; int running; } Port;
typedef struct Queue { u32 magic; Port *ports[2]; int refs, terminated; } Queue;
#define QMAGIC 0x51554555u
static Port *port_new(int mode) { Port *p = calloc(1, sizeof *p); p->mode = mode; pthread_mutex_init(&p->m, 0); pthread_cond_init(&p->cv, 0); return p; }
static Queue *queue_of(u64 h) { Queue *q = (Queue *)GP(h); return h && q->magic == QMAGIC ? q : 0; }
static u64 queue_new(int work, int completion) {
  Queue *q = calloc(1, sizeof *q); q->magic = QMAGIC; q->refs = 1;
  q->ports[0] = port_new(work); q->ports[1] = port_new(completion);
  return (uptr)q;
}
static u64 process_queue;
static u64 default_queue(void) { if (!process_queue) process_queue = queue_new(MODE_THREADPOOL, MODE_THREADPOOL); return process_queue; }
static void run_item(Cpu *c, Item *it, int canceled) { rt_guest_call(c, it->callback, it->context, (u64)canceled, 0, 0); free(it); }
typedef struct { Port *port; } PoolArg;
// a thread-pool worker: runs the port's due items until none is left
static void pool_worker(Cpu *c, void *arg) {
  Port *p = ((PoolArg *)arg)->port; free(arg);
  for (;;) {
    pthread_mutex_lock(&p->m);
    Item **best = 0; u64 now = now_us(), wait = 0;
    for (Item **pi = &p->items; *pi; pi = &(*pi)->next) { if ((*pi)->due <= now) { best = pi; break; } u64 w = (*pi)->due - now; if (!wait || w < wait) wait = w; }
    if (!best) {
      if (!p->items) { p->running--; pthread_mutex_unlock(&p->m); return; }
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)(wait % 1000000) * 1000; ts.tv_sec += (time_t)(wait / 1000000 + ts.tv_nsec / 1000000000); ts.tv_nsec %= 1000000000;
      pthread_cond_timedwait(&p->cv, &p->m, &ts); pthread_mutex_unlock(&p->m); continue;
    }
    Item *it = *best; *best = it->next;
    pthread_mutex_unlock(&p->m);
    run_item(c, it, 0);
  }
}
static void submit(Cpu *c, Queue *owner, Port *p, u64 delay_ms, u64 cb, u64 ctx) {
  Item *it = calloc(1, sizeof *it); it->owner = (uptr)owner; it->callback = cb; it->context = ctx; it->due = now_us() + delay_ms * 1000;
  if (p->mode == MODE_IMMEDIATE && !delay_ms) { run_item(c, it, 0); return; }
  pthread_mutex_lock(&p->m);
  Item **tail = &p->items; while (*tail) tail = &(*tail)->next; *tail = it;
  int spawn = (p->mode == MODE_THREADPOOL || p->mode == MODE_SERIALIZED || p->mode == MODE_IMMEDIATE) && (p->mode == MODE_THREADPOOL ? p->running < 8 : p->running == 0);
  if (spawn) p->running++;
  pthread_cond_broadcast(&p->cv);
  pthread_mutex_unlock(&p->m);
  if (spawn) { PoolArg *a = malloc(sizeof *a); a->port = p; rt_spawn_native(pool_worker, a); }
}
IMP(xgameruntime, XTaskQueueCreate) { M64(ARG(2)) = queue_new((int)ARG(0), (int)ARG(1)); RET(S_OK); }
IMP(xgameruntime, XTaskQueueGetPort) { Queue *q = queue_of(ARG(0)); if (!q || ARG(1) > 1) { RET(E_INVALIDARG); return; } M64(ARG(2)) = (uptr)q->ports[ARG(1)]; RET(S_OK); }
IMP(xgameruntime, XTaskQueueCreateComposite) {
  Queue *q = calloc(1, sizeof *q); q->magic = QMAGIC; q->refs = 1;
  q->ports[0] = (Port *)GP(ARG(0)); q->ports[1] = (Port *)GP(ARG(1));
  M64(ARG(2)) = (uptr)q; RET(S_OK);
}
IMP(xgameruntime, XTaskQueueSubmitCallback) { Queue *q = queue_of(ARG(0) ? ARG(0) : default_queue()); if (!q || q->terminated) { RET(E_ABORT); return; } submit(c, q, q->ports[ARG(1) & 1], 0, ARG(3), ARG(2)); RET(S_OK); }
IMP(xgameruntime, XTaskQueueSubmitDelayedCallback) { Queue *q = queue_of(ARG(0) ? ARG(0) : default_queue()); if (!q || q->terminated) { RET(E_ABORT); return; } submit(c, q, q->ports[ARG(1) & 1], (u32)ARG(2), M64(R[RSP] + 0x28), ARG(3)); RET(S_OK); }
IMP(xgameruntime, XTaskQueueDispatch) { // (queue, port, timeout ms) -> true when a callback ran
  Queue *q = queue_of(ARG(0) ? ARG(0) : default_queue()); if (!q) { RET(0); return; }
  Port *p = q->ports[ARG(1) & 1]; u64 until = now_us() + (u32)ARG(2) * 1000ull;
  pthread_mutex_lock(&p->m);
  for (;;) {
    u64 now = now_us(), wait = 0; Item **best = 0;
    for (Item **pi = &p->items; *pi; pi = &(*pi)->next) { if ((*pi)->due <= now) { best = pi; break; } u64 w = (*pi)->due - now; if (!wait || w < wait) wait = w; }
    if (best) { Item *it = *best; *best = it->next; pthread_mutex_unlock(&p->m); run_item(c, it, 0); RET(1); return; }
    if (now >= until) break;
    u64 w = until - now; if (wait && wait < w) w = wait;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)(w % 1000000) * 1000; ts.tv_sec += (time_t)(w / 1000000 + ts.tv_nsec / 1000000000); ts.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&p->cv, &p->m, &ts);
  }
  pthread_mutex_unlock(&p->m);
  RET(0);
}
IMP(xgameruntime, XTaskQueueTerminate) { // (queue, wait, context, callback): its pending callbacks run canceled
  Queue *q = queue_of(ARG(0)); if (!q) { RET(E_INVALIDARG); return; }
  q->terminated = 1;
  for (int k = 0; k < 2; k++) {
    Port *p = q->ports[k]; Item *mine = 0, **tail = &mine;
    pthread_mutex_lock(&p->m);
    for (Item **pi = &p->items; *pi;) { if ((*pi)->owner == (uptr)q) { Item *it = *pi; *pi = it->next; it->next = 0; *tail = it; tail = &it->next; } else pi = &(*pi)->next; }
    pthread_mutex_unlock(&p->m);
    while (mine) { Item *n = mine->next; run_item(c, mine, 1); mine = n; }
  }
  if (ARG(3)) rt_guest_call(c, ARG(3), ARG(2), 0, 0, 0);
  RET(S_OK);
}
IMP(xgameruntime, XTaskQueueCloseHandle) { RET(S_OK); }
IMP(xgameruntime, XTaskQueueDuplicateHandle) { M64(ARG(1)) = ARG(0); RET(S_OK); }
IMP(xgameruntime, XTaskQueueGetCurrentProcessTaskQueue) { M64(ARG(0)) = default_queue(); RET(1); }
IMP(xgameruntime, XTaskQueueSetCurrentProcessTaskQueue) { process_queue = ARG(0); }
IMP(xgameruntime, XTaskQueueRegisterMonitor) { if (ARG(3)) M64(ARG(3)) = 1; RET(S_OK); }
IMP(xgameruntime, XTaskQueueUnregisterMonitor) {}
IMP(xgameruntime, XTaskQueueRegisterWaiter) { if (ARG(5)) M64(ARG(5)) = 1; RET(S_OK); }
IMP(xgameruntime, XTaskQueueUnregisterWaiter) {}

// ---- XAsync: the operations here finish at once; the block's internal words hold our state ----
// XAsyncBlock: queue, context, callback, internal[4]
typedef struct { u32 magic; u32 status; u64 result; u64 size; } Async;
#define AMAGIC 0x41535943u
static Async *async_of(u64 block) { u64 p = M64(block + 24); Async *a = (Async *)GP(p); return p && a->magic == AMAGIC ? a : 0; }
static u32 async_finish(Cpu *c, u64 block, u32 status, u64 result, u64 size) {
  Async *a = calloc(1, sizeof *a); a->magic = AMAGIC; a->status = status; a->result = result; a->size = size;
  M64(block + 24) = (uptr)a;
  u64 cb = M64(block + 16);
  if (cb) { Queue *q = queue_of(M64(block) ? M64(block) : default_queue()); if (q) submit(c, q, q->ports[1], 0, cb, block); }
  return S_OK;
}
IMP(xgameruntime, XAsyncGetStatus) { Async *a = async_of(ARG(0)); RET(a ? a->status : E_PENDING); }
IMP(xgameruntime, XAsyncGetResultSize) { Async *a = async_of(ARG(0)); if (!a) { RET(E_PENDING); return; } if (ARG(1)) M64(ARG(1)) = a->size; RET(a->status); }
IMP(xgameruntime, XAsyncCancel) {}
IMP(xgameruntime, XAsyncRun) { // (block, work): run the work callback with the block, then complete
  u32 hr = (u32)rt_guest_call(c, ARG(1), ARG(0), 0, 0, 0);
  RET(async_finish(c, ARG(0), hr, 0, 0));
}

// ---- XGameRuntime, XSystem, XPackage ----
static char package_id[256];
static void read_manifest(void) {
  if (package_id[0]) return;
  snprintf(package_id, sizeof package_id, "Title");
  u32 e; VFile *f = vfs_open("G:\\MicrosoftGame.config", 0x80000000u, 3, &e);
  if (!f) return;
  u64 n = vfs_size(f); char *s = calloc(1, n + 1); vfs_read(f, s, n); vfs_close(f);
  char *id = strstr(s, "<Identity");
  char *name = id ? strstr(id, "Name=\"") : 0;
  if (name) { name += 6; char *end = strchr(name, '"'); if (end && end - name < 200) { memcpy(package_id, name, (size_t)(end - name)); package_id[end - name] = 0; } }
  free(s);
}
IMP(xgameruntime, XGameRuntimeInitialize) { RET(S_OK); }
IMP(xgameruntime, XGameRuntimeInitializeWithOptions) { RET(S_OK); }
IMP(xgameruntime, XGameRuntimeUninitialize) {}
IMP(xgameruntime, XSystemGetDeviceType) { RET(7); } // XboxScarlettAnaconda
IMP(xgameruntime, XSystemGetConsoleId) { const char *id = "0000000000000000-0000000000000000000"; if (ARG(0) >= 38) snprintf(GSTR(ARG(1)), 38, "%s", id); if (ARG(2)) M64(ARG(2)) = 38; RET(S_OK); }
IMP(xgameruntime, XPackageGetCurrentProcessPackageIdentifier) { read_manifest(); if (ARG(0) <= strlen(package_id)) { RET(E_INVALIDARG); return; } snprintf(GSTR(ARG(1)), ARG(0), "%s", package_id); RET(S_OK); }
IMP(xgameruntime, XGameGetXboxTitleId) { M32(ARG(0)) = 0x7952987Au; RET(S_OK); }
IMP(xgameruntime, XPersistentLocalStorageGetPath) {
  const char *p = "S:\\"; if (ARG(0) < 4) { RET(E_INVALIDARG); return; }
  snprintf(GSTR(ARG(1)), ARG(0), "%s", p); if (ARG(2)) M64(ARG(2)) = 4; RET(S_OK);
}
IMP(xgameruntime, XPersistentLocalStorageGetPathSize) { M64(ARG(0)) = 4; RET(S_OK); }
IMP(xgameruntime, XPersistentLocalStorageGetSpaceInfo) { M64(ARG(0)) = 2048ull << 20; M64(ARG(0) + 8) = 2048ull << 20; RET(S_OK); }

// ---- XUser: one local offline user ----
static u64 the_user;
static u64 user(void) { if (!the_user) { the_user = (uptr)calloc(1, 64); M32(the_user) = 0x55534552u; } return the_user; }
IMP(xgameruntime, XUserAddAsync) { RET(async_finish(c, ARG(1), S_OK, user(), 8)); }
IMP(xgameruntime, XUserAddResult) { Async *a = async_of(ARG(0)); if (!a) { RET(E_PENDING); return; } if (a->status) { RET(a->status); return; } M64(ARG(1)) = a->result; RET(S_OK); }
IMP(xgameruntime, XUserFindForDevice) { M64(ARG(1)) = user(); RET(S_OK); }
IMP(xgameruntime, XUserDuplicateHandle) { M64(ARG(1)) = ARG(0); RET(S_OK); }
IMP(xgameruntime, XUserCloseHandle) {}
IMP(xgameruntime, XUserGetId) { M64(ARG(1)) = 0x0009000000000001ull; RET(S_OK); }
IMP(xgameruntime, XUserGetLocalId) { M64(ARG(1)) = 1; RET(S_OK); }
IMP(xgameruntime, XUserGetState) { M32(ARG(1)) = 0; RET(S_OK); } // SignedIn
IMP(xgameruntime, XUserGetIsGuest) { M8(ARG(1)) = 0; RET(S_OK); }
IMP(xgameruntime, XUserGetGamertag) { // (user, component, size, buffer, &used)
  const char *tag = "Player1"; u64 l = strlen(tag) + 1;
  if (ARG(2) < l) { RET(E_INVALIDARG); return; }
  memcpy(GSTR(ARG(3)), tag, l); if (M64(R[RSP] + 0x28)) M64(M64(R[RSP] + 0x28)) = l; RET(S_OK);
}
IMP(xgameruntime, XUserGetTokenAndSignatureAsync) { RET(async_finish(c, M64(R[RSP] + 0x48), E_GAMEUSER_NO_NETWORK, 0, 0)); } // no Xbox Live
IMP(xgameruntime, XUserGetTokenAndSignatureResultSize) { Async *a = async_of(ARG(0)); RET(a ? a->status : E_PENDING); }
IMP(xgameruntime, XUserGetTokenAndSignatureResult) { Async *a = async_of(ARG(0)); RET(a ? a->status : E_PENDING); }
IMP(xgameruntime, XUserRegisterForChangeEvent) { if (ARG(3)) M64(ARG(3)) = 1; RET(S_OK); }
IMP(xgameruntime, XUserUnregisterForChangeEvent) {}
IMP(xgameruntime, XUserRegisterForDeviceAssociationChanged) { if (ARG(3)) M64(ARG(3)) = 1; RET(S_OK); }
IMP(xgameruntime, XUserUnregisterForDeviceAssociationChanged) {}

// ---- saves: a folder in persistent local storage ----
IMP(xgameruntime, XGameSaveFilesGetFolderWithUiAsync) { vfs_mkdir("S:\\GameSaves"); RET(async_finish(c, ARG(2), S_OK, 0, 16)); }
IMP(xgameruntime, XGameSaveFilesGetFolderWithUiResult) {
  Async *a = async_of(ARG(0)); if (!a) { RET(E_PENDING); return; }
  if (ARG(1) < 15) { RET(E_INVALIDARG); return; }
  snprintf(GSTR(ARG(2)), ARG(1), "S:\\GameSaves"); RET(S_OK);
}

// ---- networking: offline ----
IMP(xgameruntime, XNetworkingQueryPreferredLocalUdpMultiplayerPort) { M16(ARG(0)) = 3074; RET(S_OK); }
IMP(xgameruntime, XNetworkingGetConnectivityHint) { u64 h = ARG(0); memset((void *)GP(h), 0, 16); M32(h) = 1; M32(h + 4) = 1; M8(h + 12) = 1; RET(S_OK); } // local access, network initialised

// ---- the title's window: the console's screen, a hidden 1920x1080 frameless tool window ----
typedef struct { u32 magic; u32 style, exstyle; u64 proc; } Window;
static u64 make_window(Cpu *c) { (void)c; Window *w = calloc(1, sizeof *w); w->magic = 0x57494E44u; w->style = 0x90000000u; w->exstyle = 0x80; return (uptr)w; }
static Window *window(u64 h) { Window *w = (Window *)GP(h); return h && w->magic == 0x57494E44u ? w : 0; }
IMP(user32, RegisterClassW) { RET(0xC001); }
IMP(user32, RegisterClassExW) { RET(0xC001); }
IMP(user32, RegisterClassA) { RET(0xC001); }
IMP(user32, CreateWindowExW) { RET(make_window(c)); }
IMP(user32, CreateWindowExA) { RET(make_window(c)); }
IMP(user32, DestroyWindow) { RET(1); }
IMP(user32, ShowWindow) { RET(1); }
IMP(user32, UpdateWindow) { RET(1); }
IMP(user32, DefWindowProcW) { RET(0); }
IMP(user32, DefWindowProcA) { RET(0); }
IMP(user32, GetWindowLongW) { Window *w = window(ARG(0)); s32 i = (s32)ARG(1); RET(!w ? 0 : i == -16 ? w->style : i == -20 ? w->exstyle : 0); }
IMP(user32, SetWindowLongW) { RET(0); }
IMP(user32, GetClientRect) { u64 r = ARG(1); M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = 1920; M32(r + 12) = 1080; RET(1); }
IMP(user32, GetWindowRect) { u64 r = ARG(1); M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = 1920; M32(r + 12) = 1080; RET(1); }
IMP(user32, PeekMessageW) { RET(0); }
IMP(user32, GetMessageW) { RET(0); }
IMP(user32, TranslateMessage) { RET(0); }
IMP(user32, DispatchMessageW) { RET(0); }
IMP(user32, PostQuitMessage) {}
IMP(dwmapi, DwmGetWindowAttribute) { if ((u32)ARG(1) == 14 && ARG(3) >= 4) { M32(ARG(2)) = 1; RET(S_OK); return; } RET(E_INVALIDARG); } // DWMWA_CLOAKED

// ---- GameInput: an IGameInput whose methods are runtime functions. The host writes the keyboard's and the
// gamepad's state into an input block in shared memory (see input_open in the hosts); a reading is a snapshot
// of it. Slots follow GameInput's v0 interface order. ----
HOST("input_open") void host_input_open(u32 block);
// input block words: 0 sequence, 1 key count, 2..17 keys (virtual key | scan code << 8), 18 gamepad connected,
// 19 gamepad buttons, 20..25 floats (left trigger, right trigger, left x, left y, right x, right y)
static u32 *input_block;
static u64 gameinput, vt_reading;
#define KIND_KEYBOARD 0x10u
#define KIND_GAMEPAD 0x40000u
typedef struct { u64 vtbl; u32 magic, kind, seq, refs; u32 snap[32]; u64 time; } Reading;
static Reading *reading(u64 p) { Reading *r = (Reading *)GP(p); return p && r->magic == 0x52454144u ? r : 0; }
IMP(gameinput, IGameInput_QueryInterface) { M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(gameinput, IGameInput_AddRef) { RET(2); }
IMP(gameinput, IGameInput_Release) { RET(1); }
IMP(gameinput, IGameInput_GetCurrentTimestamp) { RET(now_us()); }
IMP(gameinput, IGameInput_GetCurrentReading) { // (kind, device, &reading)
  u32 kind = (u32)ARG(1); u64 out = ARG(3);
  if (out) M64(out) = 0;
  if (!input_block || !out) { RET(0x838A0003u); return; } // GAMEINPUT_E_READING_NOT_FOUND
  u32 snap[32]; memcpy(snap, input_block, sizeof snap);
  u32 k = kind & KIND_GAMEPAD ? KIND_GAMEPAD : kind & KIND_KEYBOARD ? KIND_KEYBOARD : 0;
  if (!k || (k == KIND_GAMEPAD && !snap[18])) { RET(0x838A0003u); return; }
  Reading *r = aligned_alloc(16, sizeof(Reading)); memset(r, 0, sizeof *r);
  r->vtbl = vt_reading; r->magic = 0x52454144u; r->kind = k; r->seq = snap[0]; r->refs = 1; r->time = now_us();
  memcpy(r->snap, snap, sizeof snap);
  M64(out) = (uptr)r; RET(S_OK);
}
IMP(gameinput, IGameInput_RegisterDeviceCallback) { // (device, kind, status filter, enumeration, context, callback, &token): no devices are announced
  u64 tok = ARG(7); if (tok) M64(tok) = 1; RET(S_OK);
}
IMP(gameinput, IGameInput_RegisterReadingCallback) { u64 tok = ARG(6); if (tok) M64(tok) = 2; RET(S_OK); }
IMP(gameinput, IGameInput_RegisterSystemButtonCallback) { u64 tok = ARG(5); if (tok) M64(tok) = 3; RET(S_OK); }
IMP(gameinput, IGameInput_RegisterKeyboardLayoutCallback) { u64 tok = ARG(4); if (tok) M64(tok) = 4; RET(S_OK); }
IMP(gameinput, IGameInput_StopCallback) {}
IMP(gameinput, IGameInput_UnregisterCallback) { RET(1); }
IMP(gameinput, IGameInput_SetFocusPolicy) {}
IMP(gameinput, IGameInput_Stub) { RET(E_NOTIMPL); }
IMP(gameinput, Reading_QueryInterface) { M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(gameinput, Reading_AddRef) { Reading *r = reading(ARG(0)); RET(r ? ++r->refs : 1); }
IMP(gameinput, Reading_Release) { Reading *r = reading(ARG(0)); if (!r) { RET(0); return; } u32 n = --r->refs; if (!n) { r->magic = 0; free(r); } RET(n); }
IMP(gameinput, Reading_GetInputKind) { Reading *r = reading(ARG(0)); RET(r ? r->kind : 0); }
IMP(gameinput, Reading_GetSequenceNumber) { Reading *r = reading(ARG(0)); RET(r ? r->seq : 0); }
IMP(gameinput, Reading_GetTimestamp) { Reading *r = reading(ARG(0)); RET(r ? r->time : 0); }
IMP(gameinput, Reading_GetDevice) { if (ARG(1)) M64(ARG(1)) = 0; }
IMP(gameinput, Reading_Zero) { RET(0); }
IMP(gameinput, Reading_GetKeyCount) { Reading *r = reading(ARG(0)); RET(r && r->kind == KIND_KEYBOARD ? (r->snap[1] > 16 ? 16 : r->snap[1]) : 0); }
IMP(gameinput, Reading_GetKeyState) { // (max count, GameInputKeyState[]) -> count; a key state: scan code, code point, virtual key, dead key
  Reading *r = reading(ARG(0)); u32 max = (u32)ARG(1), n = 0; u64 out = ARG(2);
  if (r && r->kind == KIND_KEYBOARD) for (u32 i = 0; i < r->snap[1] && i < 16 && n < max; i++, n++) {
    u32 k = r->snap[2 + i]; u64 e = out + 12ull * n;
    M32(e) = k >> 8 & 0xFFFF; M32(e + 4) = (k & 0xFF) >= 0x41 && (k & 0xFF) <= 0x5A ? (k & 0xFF) + 32 : 0; M8(e + 8) = (u8)k; M8(e + 9) = 0;
  }
  RET(n);
}
IMP(gameinput, Reading_GetGamepadState) { // (GameInputGamepadState*) -> bool
  Reading *r = reading(ARG(0)); u64 g = ARG(1);
  if (!r || r->kind != KIND_GAMEPAD || !g) { RET(0); return; }
  M32(g) = r->snap[19]; for (int i = 0; i < 6; i++) M32(g + 4 + 4 * i) = r->snap[20 + i];
  RET(1);
}
static u64 vt_of(const char *const *names, unsigned n, unsigned total, const char *rest) {
  u64 vt = (uptr)calloc(total, 8);
  for (unsigned i = 0; i < total; i++) M64(vt + 8 * i) = rt_dynamic_import("gameinput", i < n && names[i] ? names[i] : rest);
  return vt;
}
IMP(gameinput, GameInputCreate) {
  if (!gameinput) {
    static const char *const slots[] = {"IGameInput_QueryInterface", "IGameInput_AddRef", "IGameInput_Release", "IGameInput_GetCurrentTimestamp", "IGameInput_GetCurrentReading",
      0, 0, 0, "IGameInput_RegisterReadingCallback", "IGameInput_RegisterDeviceCallback", "IGameInput_RegisterSystemButtonCallback", "IGameInput_RegisterKeyboardLayoutCallback",
      "IGameInput_StopCallback", "IGameInput_UnregisterCallback", 0, 0, 0, 0, 0, 0, 0, "IGameInput_SetFocusPolicy"};
    static const char *const rslots[] = {"Reading_QueryInterface", "Reading_AddRef", "Reading_Release", "Reading_GetInputKind", "Reading_GetSequenceNumber", "Reading_GetTimestamp", "Reading_GetDevice",
      0, 0, 0, 0, 0, 0, 0, "Reading_GetKeyCount", "Reading_GetKeyState", 0, 0, 0, 0, 0, 0, "Reading_GetGamepadState"};
    u64 vt = vt_of(slots, sizeof slots / sizeof *slots, 64, "IGameInput_Stub");
    vt_reading = vt_of(rslots, sizeof rslots / sizeof *rslots, 32, "Reading_Zero");
    gameinput = (uptr)calloc(1, 16); M64(gameinput) = vt;
    input_block = aligned_alloc(64, 128); memset(input_block, 0, 128);
    host_input_open((u32)(uptr)input_block);
  }
  M64(ARG(0)) = gameinput; RET(S_OK);
}

// ---- small things Windows titles ask for ----
IMP(advapi32, LsaOpenPolicy) { RET(0xC0000002u); } // STATUS_NOT_IMPLEMENTED: the caller's fallback
IMP(oleaut32, SysFreeString) { if (ARG(0)) free((void *)GP(ARG(0) - 4)); }
IMP(oleaut32, SysStringLen) { RET(ARG(0) ? M32(ARG(0) - 4) / 2 : 0); }
IMP(oleaut32, GetErrorInfo) { M64(ARG(1)) = 0; RET(1); } // S_FALSE
IMP(kernel32, SetThreadStackGuarantee) { if (ARG(0)) M32(ARG(0)) = 0; RET(1); }
IMP(kernel32, IsThreadAFiber) { RET(0); }
IMP(kernel32, GetEnvironmentVariableW) { SET_LAST_ERROR(203); RET(0); }
IMP(kernel32, GetEnvironmentVariableA) { SET_LAST_ERROR(203); RET(0); }
IMP(kernel32, CreateWaitableTimerExW) { SET_LAST_ERROR(50); RET(0); }
IMP(kernel32, CreateToolhelp32Snapshot) { SET_LAST_ERROR(50); RET(0xFFFFFFFFFFFFFFFFull); }
IMP(kernel32, Module32FirstW) { RET(0); }
IMP(kernel32, Module32NextW) { RET(0); }
