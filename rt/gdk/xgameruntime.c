// SPDX-License-Identifier: GPL-2.0-or-later
// The GDK's XGameRuntime, offline: task queues, XAsync, XUser, saves, persistent local storage, package,
// networking hints. Behaviour follows XCL's findings on retail titles (github.com/ivall/xcl, GPL-2.0).
#include "gdk.h"

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
  u64 ctx = ARG(2), cb = ARG(3); // (read now: the canceled callbacks reuse the registers)
  q->terminated = 1;
  for (int k = 0; k < 2; k++) {
    Port *p = q->ports[k]; Item *mine = 0, **tail = &mine;
    pthread_mutex_lock(&p->m);
    for (Item **pi = &p->items; *pi;) { if ((*pi)->owner == (uptr)q) { Item *it = *pi; *pi = it->next; it->next = 0; *tail = it; tail = &it->next; } else pi = &(*pi)->next; }
    pthread_mutex_unlock(&p->m);
    while (mine) { Item *n = mine->next; run_item(c, mine, 1); mine = n; }
  }
  if (cb) rt_guest_call(c, cb, ctx, 0, 0, 0);
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
  u64 block = ARG(0), work = ARG(1); // (the call below reuses the argument registers)
  u32 hr = (u32)rt_guest_call(c, work, block, 0, 0, 0);
  RET(async_finish(c, block, hr, 0, 0));
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
IMP(xgameruntime, XThreadAssertNotTimeSensitive) { RET(S_OK); }
IMP(xgameruntime, XSystemGetXboxLiveSandboxId) { // (size, buffer, &used): offline, the retail sandbox
  const char *id = "RETAIL"; u64 n = strlen(id) + 1;
  if (ARG(2)) M64(ARG(2)) = n; if (ARG(0) < n || !ARG(1)) { RET(0x8007007Au); return; } // ERROR_INSUFFICIENT_BUFFER
  memcpy(GSTR(ARG(1)), id, n); RET(S_OK);
}
IMP(xgameruntime, XSystemGetAnalyticsInfo) { // XSystemAnalyticsInfo by value (hidden pointer): OS version, hosting OS version, family, form
  u64 r = ARG(0); memset((void *)GP(r), 0, 144);
  M16(r) = 10; M16(r + 2) = 0; M16(r + 4) = 25398; M16(r + 8) = 10; M16(r + 12) = 25398; // XVersion: major, minor, build, revision
  strcpy(GSTR(r + 16), "Windows.Xbox"); strcpy(GSTR(r + 80), "Xbox Series X");
  RET(r);
}
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
