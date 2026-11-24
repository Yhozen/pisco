// xcrdapi.dll, the console's DirectStorage: DStorageGetFactory -> factory, queues, files, status arrays.
// Vtable orders and the request layout are XCL's findings on a retail title (github.com/ivall/xcl, GPL-2.0):
//   factory: IUnknown, CreateQueue(desc, riid, out), OpenFile(path, riid, out), CreateStatusArray(capacity, name, riid, out), ...
//   queue: IUnknown, EnqueueRequest, EnqueueStatus(array, index), EnqueueSignal(fence, value), Submit,
//          CancelRequestsWithTag(mask, value), Close, ...
//   request (0x48): options u64 @0 (bit 21: the destination is a physical page array; bit 22: the source is
//          memory), destination @8, destination page offset u16 @0x10, destination size u32 @0x18, source
//          @0x20 (a file, or memory), file offset u64 @0x28, source size u32 @0x30, cancellation tag @0x38.
// Requests run in order when the queue is submitted, on the calling thread: file reads through the title's
// file system, memory copies. A status entry gets the first failure since the previous one; a signal sets a
// D3D12 fence from the CPU. Compressed (zlib, BCPack) requests are refused.
#include "rt.h"
#include "vfs.h"

#define S_OK 0
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_PENDING 0x8000000Au
#define E_ABORT 0x80004004u
#define MAGIC 0x44535447u

typedef struct { u64 vtbl; u32 magic, kind; s32 refs; } Head;                   // kind: 0 factory, 1 queue, 2 file, 3 status
typedef struct { Head h; VFile *vf; } File;
typedef struct { Head h; u32 n; s32 *slots; } Status;
typedef struct Item { int what; u8 req[0x48]; u64 a, b; struct Item *next; } Item; // what: 0 request, 1 status (a, b index), 2 signal (fence a, value b)
typedef struct { Head h; Item *items, **tail; s32 failure; pthread_mutex_t m; } Queue;
static Head *head_of(u64 p, u32 kind) { Head *h = (Head *)GP(p); return p && h->magic == MAGIC && h->kind == kind ? h : 0; }
static void *obj(const char *prefix, const char *const *names, unsigned n, u32 kind, size_t size) {
  static u64 vts[4];
  if (!vts[kind]) vts[kind] = rt_com_vtable("xcrdapi", prefix, names, n);
  Head *h = calloc(1, size); h->vtbl = vts[kind]; h->magic = MAGIC; h->kind = kind; h->refs = 1;
  return h;
}
static void release(Cpu *c) { Head *h = (Head *)GP(ARG(0)); s32 r = --h->refs; RET(r < 0 ? 0 : r); } // objects are kept: requests may name them
static void add_ref(Cpu *c) { RET(++((Head *)GP(ARG(0)))->refs); }
static void qi(Cpu *c) { if (ARG(2)) { M64(ARG(2)) = ARG(0); ((Head *)GP(ARG(0)))->refs++; } RET(S_OK); }

IMP(xcrdapi, DStorageGetFactory) { // (riid, &factory)
  static const char *const names[] = {"Factory_QueryInterface", "Factory_AddRef", "Factory_Release", "Factory_CreateQueue", "Factory_OpenFile", "Factory_CreateStatusArray",
    "Factory_SetCpuAffinity", "Factory_SetDebugFlags", "Factory_SetStagingBufferSize", 0, 0, 0, 0, 0, 0, 0};
  static Head *f;
  if (!ARG(1)) { RET(E_POINTER); return; }
  if (!f) f = obj("", names, 16, 0, sizeof(Head));
  f->refs++; M64(ARG(1)) = (uptr)f; RET(S_OK);
}
IMP(xcrdapi, Factory_QueryInterface) { qi(c); }
IMP(xcrdapi, Factory_AddRef) { add_ref(c); }
IMP(xcrdapi, Factory_Release) { release(c); }
IMP(xcrdapi, Factory_SetCpuAffinity) { RET(S_OK); }
IMP(xcrdapi, Factory_SetDebugFlags) { RET(S_OK); }
IMP(xcrdapi, Factory_SetStagingBufferSize) { RET(S_OK); }
IMP(xcrdapi, Factory_CreateQueue) { // (desc, riid, &queue)
  static const char *const names[] = {"Queue_QueryInterface", "Queue_AddRef", "Queue_Release", "Queue_EnqueueRequest", "Queue_EnqueueStatus", "Queue_EnqueueSignal",
    "Queue_Submit", "Queue_CancelRequestsWithTag", "Queue_Close", 0, 0, 0, 0, 0, 0, 0, 0, 0};
  if (!ARG(3)) { RET(E_POINTER); return; }
  Queue *q = obj("", names, 18, 1, sizeof(Queue)); q->tail = &q->items; pthread_mutex_init(&q->m, 0);
  M64(ARG(3)) = (uptr)q; RET(S_OK);
}
IMP(xcrdapi, Factory_OpenFile) { // (path, riid, &file)
  static const char *const names[] = {"File_QueryInterface", "File_AddRef", "File_Release", "File_Close", 0, 0, 0, 0, 0, 0, 0, 0};
  if (!ARG(3)) { RET(E_POINTER); return; }
  char *path = rt_narrow(GWSTR(ARG(1))); u32 err = 0;
  VFile *vf = vfs_open(path, 0x80000000u, 3, &err); // GENERIC_READ, OPEN_EXISTING
  if (!vf) { rt_log("DirectStorage: OpenFile %s: not found", path); free(path); M64(ARG(3)) = 0; RET(0x80070002u); return; }
  free(path);
  File *f = obj("", names, 12, 2, sizeof(File)); f->vf = vf;
  M64(ARG(3)) = (uptr)f; RET(S_OK);
}
IMP(xcrdapi, Factory_CreateStatusArray) { // (capacity, name, riid, &array)
  static const char *const names[] = {"Status_QueryInterface", "Status_AddRef", "Status_Release", "Status_IsComplete", "Status_GetHResult", 0, 0, 0, 0, 0};
  u32 n = (u32)ARG(1); if (!ARG(4)) { RET(E_POINTER); return; } if (!n || n > 1u << 20) { RET(E_INVALIDARG); return; }
  Status *s = obj("", names, 10, 3, sizeof(Status)); s->n = n; s->slots = malloc(4ull * n);
  for (u32 i = 0; i < n; i++) s->slots[i] = (s32)E_PENDING;
  M64(ARG(4)) = (uptr)s; RET(S_OK);
}
IMP(xcrdapi, File_QueryInterface) { qi(c); }
IMP(xcrdapi, File_AddRef) { add_ref(c); }
IMP(xcrdapi, File_Release) { release(c); }
IMP(xcrdapi, File_Close) { File *f = (File *)head_of(ARG(0), 2); if (f && f->vf) { vfs_close(f->vf); f->vf = 0; } }
IMP(xcrdapi, Status_QueryInterface) { qi(c); }
IMP(xcrdapi, Status_AddRef) { add_ref(c); }
IMP(xcrdapi, Status_Release) { release(c); }
IMP(xcrdapi, Status_IsComplete) { Status *s = (Status *)head_of(ARG(0), 3); u32 i = (u32)ARG(1); RET(s && i < s->n && __atomic_load_n(&s->slots[i], __ATOMIC_ACQUIRE) != (s32)E_PENDING); }
IMP(xcrdapi, Status_GetHResult) { Status *s = (Status *)head_of(ARG(0), 3); u32 i = (u32)ARG(1); RET(s && i < s->n ? (u32)__atomic_load_n(&s->slots[i], __ATOMIC_ACQUIRE) : E_INVALIDARG); }

IMP(xcrdapi, Queue_QueryInterface) { qi(c); }
IMP(xcrdapi, Queue_AddRef) { add_ref(c); }
IMP(xcrdapi, Queue_Release) { release(c); }
static void enqueue(Queue *q, Item *it) { pthread_mutex_lock(&q->m); *q->tail = it; q->tail = &it->next; pthread_mutex_unlock(&q->m); }
IMP(xcrdapi, Queue_EnqueueRequest) { Queue *q = (Queue *)head_of(ARG(0), 1); if (!q) return; Item *it = calloc(1, sizeof *it); memcpy(it->req, (void *)GP(ARG(1)), 0x48); enqueue(q, it); }
IMP(xcrdapi, Queue_EnqueueStatus) {
  Queue *q = (Queue *)head_of(ARG(0), 1); Status *s = (Status *)head_of(ARG(1), 3); if (!q || !s) return;
  u32 i = (u32)ARG(2); if (i < s->n) __atomic_store_n(&s->slots[i], (s32)E_PENDING, __ATOMIC_RELEASE);
  Item *it = calloc(1, sizeof *it); it->what = 1; it->a = ARG(1); it->b = i; enqueue(q, it);
}
IMP(xcrdapi, Queue_EnqueueSignal) { Queue *q = (Queue *)head_of(ARG(0), 1); if (!q) return; Item *it = calloc(1, sizeof *it); it->what = 2; it->a = ARG(1); it->b = ARG(2); enqueue(q, it); }
IMP(xcrdapi, Queue_CancelRequestsWithTag) { // (mask, value): queued requests whose tag & mask == value are dropped
  Queue *q = (Queue *)head_of(ARG(0), 1); if (!q) return; u64 mask = ARG(1), value = ARG(2);
  pthread_mutex_lock(&q->m);
  for (Item **pi = &q->items; *pi;) {
    Item *it = *pi; u64 tag; memcpy(&tag, it->req + 0x38, 8);
    if (it->what == 0 && (tag & mask) == value) { *pi = it->next; free(it); } else pi = &it->next;
  }
  q->tail = &q->items; while (*q->tail) q->tail = &(*q->tail)->next;
  pthread_mutex_unlock(&q->m);
}
IMP(xcrdapi, Queue_Close) {}
// one request: its bytes, then where they go
static u32 run(const u8 *r) {
  u64 options, dst, src, off; u32 dst_size, src_size; u16 page_off;
  memcpy(&options, r, 8); memcpy(&dst, r + 8, 8); memcpy(&page_off, r + 0x10, 2); memcpy(&dst_size, r + 0x18, 4);
  memcpy(&src, r + 0x20, 8); memcpy(&off, r + 0x28, 8); memcpy(&src_size, r + 0x30, 4);
  if (options & 0x1F) { rt_log("DirectStorage: compressed request (options 0x%llx) refused", (unsigned long long)options); return E_NOTIMPL; }
  if (options & (1ull << 23)) { rt_log("DirectStorage: a physical-page source is not supported"); return E_NOTIMPL; }
  u32 n = src_size < dst_size ? src_size : dst_size;
  u8 *bytes = malloc(n ? n : 1);
  if (options & (1ull << 22)) memcpy(bytes, (void *)GP(src), n);
  else {
    File *f = (File *)head_of(src, 2);
    if (!f || !f->vf) { free(bytes); return E_INVALIDARG; }
    u64 save = *vfs_pos(f->vf); *vfs_pos(f->vf) = off;
    s64 got = vfs_read(f->vf, bytes, n); *vfs_pos(f->vf) = save;
    if (got != (s64)n) { free(bytes); return 0x80070026u; } // ERROR_HANDLE_EOF
  }
  if (options & (1ull << 21)) { // a page array: page numbers, the first `page_off` bytes in
    for (u32 done = 0; done < n;) {
      u64 at = page_off + done, page = M64(dst + 8 * (at >> 16)); u32 in = (u32)(at & 0xFFFF), k = 0x10000 - in;
      if (k > n - done) k = n - done;
      u8 *p = rt_phys_page(page); if (!p) { free(bytes); return E_INVALIDARG; }
      memcpy(p + in, bytes + done, k); done += k;
    }
  } else memcpy((void *)GP(dst), bytes, n);
  free(bytes);
  return S_OK;
}
void rt_fence_signal_cpu(u64 fence, u64 value); // d3d12.c
IMP(xcrdapi, Queue_Submit) {
  Queue *q = (Queue *)head_of(ARG(0), 1); if (!q) return;
  pthread_mutex_lock(&q->m); Item *list = q->items; q->items = 0; q->tail = &q->items; pthread_mutex_unlock(&q->m);
  while (list) {
    Item *it = list; list = it->next;
    if (it->what == 0) { u32 hr = run(it->req); if (hr && !q->failure) q->failure = (s32)hr; }
    else if (it->what == 1) { Status *s = (Status *)head_of(it->a, 3); if (s && it->b < s->n) __atomic_store_n(&s->slots[it->b], q->failure, __ATOMIC_RELEASE); q->failure = 0; }
    else rt_fence_signal_cpu(it->a, it->b);
    free(it);
  }
}
