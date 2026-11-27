// SPDX-License-Identifier: GPL-2.0-or-later
// kernel32.dll (and the ntdll functions it forwards) for lifted titles.
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "vfs.h"

u64 rt_create_thread(Cpu *c, u64 stack, u64 start, u64 param, int suspended, u32 *tid);
int rt_resume_thread(u64 h);
void rt_exit_process(u32 code);
int rt_file_write(int fd, const void *p, u32 n);
int rt_file_read(int fd, void *p, u32 n);
extern const char *rt_command_line;

// ---- process and threads ----
IMP(kernel32, ExitProcess) { rt_exit_process((u32)ARG(0)); }
IMP(kernel32, TerminateProcess) { rt_exit_process((u32)ARG(1)); }
IMP(kernel32, GetCurrentProcess) { RET(PSEUDO_PROCESS); }
IMP(kernel32, GetCurrentProcessId) { RET(4); }
IMP(kernel32, GetCurrentThread) { RET(PSEUDO_THREAD); }
IMP(kernel32, GetCurrentThreadId) { RET(THREAD(c)->id); }
IMP(kernel32, OpenProcess) { SET_LAST_ERROR(5); RET(0); }
IMP(kernel32, CreateThread) {
  u32 tid = 0;
  u64 h = rt_create_thread(c, ARG(1), ARG(2), ARG(3), (ARG(4) & 4) != 0, &tid);
  if (ARG(5)) M32(ARG(5)) = tid;
  RET(h);
}
IMP(kernel32, ExitThread) { rt_thread_exit(c, (u32)ARG(0)); }
IMP(kernel32, ResumeThread) { RET(rt_resume_thread(ARG(0)) < 0 ? 0xFFFFFFFF : 1); }
IMP(kernel32, SuspendThread) { SET_LAST_ERROR(50); RET(0xFFFFFFFF); }
IMP(kernel32, GetThreadContext) { SET_LAST_ERROR(50); RET(0); }
IMP(kernel32, SetThreadContext) { SET_LAST_ERROR(50); RET(0); }
IMP(kernel32, GetExitCodeThread) {
  Object *o = rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_THREAD) { RET(0); return; }
  M32(ARG(1)) = o->signaled ? o->thread->exit_code : 259; // STILL_ACTIVE
  RET(1);
}
IMP(kernel32, GetThreadPriority) { RET(0); }
IMP(kernel32, SetThreadPriority) { RET(1); }
IMP(kernel32, SetThreadGroupAffinity) { if (ARG(2)) memset((void *)GP(ARG(2)), 0, 16); RET(1); }
IMP(kernel32, GetProcessAffinityMask) { u64 m = (1ull << sysconf(_SC_NPROCESSORS_ONLN)) - 1; if (ARG(1)) MP(ARG(1)) = m; if (ARG(2)) MP(ARG(2)) = m; RET(1); }
IMP(kernel32, SetProcessAffinityMask) { RET(1); }
IMP(kernel32, GetProcessGroupAffinity) { u64 n = ARG(1); if (n && M16(n) >= 1) { M16(n) = 1; if (ARG(2)) M16(ARG(2)) = 0; RET(1); } else { if (n) M16(n) = 1; SET_LAST_ERROR(122); RET(0); } }
IMP(kernel32, GetActiveProcessorCount) { RET(sysconf(_SC_NPROCESSORS_ONLN)); }
IMP(kernel32, GetActiveProcessorGroupCount) { RET(1); }
IMP(kernel32, GetLogicalProcessorInformationEx) { SET_LAST_ERROR(50); RET(0); }
IMP(kernel32, GetNumaProcessorNodeEx) { if (ARG(1)) M16(ARG(1)) = 0; RET(1); }
IMP(kernel32, GetLargePageMinimum) { RET(0); }
IMP(kernel32, SwitchToThread) { sched_yield(); RET(1); }
IMP(kernel32, Sleep) {
  u32 ms = (u32)ARG(0);
  if (rt_clock_virtual()) { rt_clock_advance(1000ull * ms); return; } // a virtual clock (SW_CLOCK): the time passes at once
  if (!ms) { sched_yield(); return; }
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
  nanosleep(&ts, 0);
}
IMP(kernel32, SleepEx) { imp_kernel32_Sleep(c); RET(0); }
// SYSTEM_INFO: 48 bytes for a 64-bit guest, 36 for a 32-bit one (whose pointers and mask are 4 bytes: writing the
// 64-bit layout into a 32-bit caller's frame overwrote its saved EBP)
IMP(kernel32, GetSystemInfo) {
  u64 p = ARG(0); u32 cpus = (u32)sysconf(_SC_NPROCESSORS_ONLN);
#if GUEST_BITS == 32
  memset((void *)GP(p), 0, 36);
  M16(p) = 0; M32(p + 4) = 4096; M32(p + 8) = 0x10000; M32(p + 12) = 0x7FFEFFFF;   // PROCESSOR_ARCHITECTURE_INTEL
  M32(p + 16) = cpus >= 32 ? 0xFFFFFFFFu : (1u << cpus) - 1; M32(p + 20) = cpus;
  M32(p + 24) = 586; M32(p + 28) = 65536; M16(p + 32) = 6;                         // PROCESSOR_INTEL_PENTIUM, level 6
#else
  memset((void *)GP(p), 0, 48);
  M16(p) = 9; M32(p + 4) = 4096; M64(p + 8) = 0x10000; M64(p + 16) = 0x7FFFFFFEFFFF;
  M64(p + 24) = (1ull << cpus) - 1; M32(p + 32) = cpus;
  M32(p + 36) = 8664; M32(p + 40) = 65536; M16(p + 44) = 6;
#endif
}
// PF_* numbers: 3 MMX, 6 XMMI, 10 XMMI64, 13 SSE3, 36 SSSE3, 37 SSE4_1, 38 SSE4_2, 39 AVX
IMP(kernel32, IsProcessorFeaturePresent) { // PF_*: 2 CMPXCHG8B, 3 MMX, 6 SSE, 8 RDTSC, 10 SSE2, 12 NX, 13 SSE3, 14 CMPXCHG16B, 36 SSSE3, 37 SSE4.1, 38 SSE4.2
  u32 f = (u32)ARG(0); unsigned have = rt_cpu_features;
  RET(f == 2 ? !!(have & CPU_CX8) : f == 3 ? !!(have & CPU_MMX) : f == 6 ? !!(have & CPU_SSE) : f == 8 ? !!(have & CPU_TSC) : f == 10 ? !!(have & CPU_SSE2) : f == 12 ? 1
    : f == 13 ? !!(have & CPU_SSE3) : f == 14 ? !!(have & CPU_CX16) : f == 36 ? !!(have & CPU_SSSE3) : f == 37 ? !!(have & CPU_SSE41) : f == 38 ? !!(have & CPU_SSE42) : 0);
}

// ---- errors ----
IMP(kernel32, GetLastError) { RET(M32(THREAD(c)->teb + TEB_LAST_ERROR)); }
IMP(kernel32, SetLastError) { SET_LAST_ERROR((u32)ARG(0)); }
IMP(kernel32, FormatMessageA) { SET_LAST_ERROR(317); RET(0); }
IMP(kernel32, FormatMessageW) { SET_LAST_ERROR(317); RET(0); }
IMP(kernel32, SetUnhandledExceptionFilter) { RET(0); }
IMP(kernel32, OutputDebugStringA) { rt_log("debug: %s", GSTR(ARG(0))); }
IMP(kernel32, OutputDebugStringW) { char *s = rt_narrow(GWSTR(ARG(0))); rt_log("debug: %s", s); free(s); }
IMP(kernel32, IsDebuggerPresent) { RET(0); }
// The last of an exception's handlers: on Windows the process ends (or a debugger is asked); here the run stops
// with what was raised, where.
IMP(kernel32, UnhandledExceptionFilter) { // (EXCEPTION_POINTERS*)
  u64 ep = ARG(0), rec = ep ? MP(ep) : 0;
  rt_fatal("unhandled exception 0x%08x at 0x%llx", rec ? M32(rec) : 0, rec ? (u64)MP(rec + (GUEST_BITS == 32 ? 12 : 16)) : 0);
}
IMP(kernel32, AreFileApisANSI) { RET(1); }
IMP(kernel32, GetNativeSystemInfo) { imp_kernel32_GetSystemInfo(c); }
IMP(kernel32, GetCurrentProcessorNumber) { RET(0); }
IMP(kernel32, FlushProcessWriteBuffers) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ---- TLS ----
static u32 tls_used[64];
IMP(kernel32, TlsAlloc) {
  pthread_mutex_lock(&rt_wait_lock);
  for (u32 i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; pthread_mutex_unlock(&rt_wait_lock); RET(i); return; }
  pthread_mutex_unlock(&rt_wait_lock);
  RET(0xFFFFFFFF);
}
IMP(kernel32, TlsFree) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; RET(1); }
IMP(kernel32, TlsGetValue) { SET_LAST_ERROR(0); RET(ARG(0) < 64 ? M64(THREAD(c)->teb + TEB_TLS_SLOTS + 8 * ARG(0)) : 0); }
IMP(kernel32, TlsSetValue) { if (ARG(0) < 64) M64(THREAD(c)->teb + TEB_TLS_SLOTS + 8 * ARG(0)) = ARG(1); RET(ARG(0) < 64); }

// ---- handles and waits ----
IMP(kernel32, CloseHandle) { rt_handle_close(ARG(0)); RET(1); }
IMP(kernel32, DuplicateHandle) {
  u64 h = ARG(1);
  Object *o = h == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(h);
  if (!o) { SET_LAST_ERROR(6); RET(0); return; }
  if (ARG(3)) MP(ARG(3)) = rt_handle_new(o);
  if (ARG(6) & 1) rt_handle_close(h); // DUPLICATE_CLOSE_SOURCE
  RET(1);
}
static Object *named(int type, const char *name, int *existed) {
  static Object *list[256]; static int n;
  *existed = 0;
  if (!name || !*name) return rt_object_new(type);
  pthread_mutex_lock(&rt_wait_lock);
  for (int i = 0; i < n; i++) if (list[i]->type == type && !strcmp(list[i]->name, name)) { *existed = 1; pthread_mutex_unlock(&rt_wait_lock); return list[i]; }
  Object *o = rt_object_new(type); strncpy(o->name, name, 63);
  if (n < 256) { list[n++] = o; o->refs++; }
  pthread_mutex_unlock(&rt_wait_lock);
  return o;
}
static void create_event(Cpu *c, const char *name) {
  int ex; Object *o = named(OBJ_EVENT, name, &ex);
  if (!ex) { o->manual = ARG(1) != 0; o->signaled = ARG(2) != 0; }
  SET_LAST_ERROR(ex ? 183 : 0);
  RET(rt_handle_new(o));
}
IMP(kernel32, CreateEventA) { create_event(c, ARG(3) ? GSTR(ARG(3)) : 0); }
IMP(kernel32, CreateEventW) { char *n = ARG(3) ? rt_narrow(GWSTR(ARG(3))) : 0; create_event(c, n); free(n); }
IMP(kernel32, CreateEventExW) { // (attributes, name, flags: 1 manual reset, 2 initially set, access)
  char *n = ARG(1) ? rt_narrow(GWSTR(ARG(1))) : 0; u32 fl = (u32)ARG(2); int ex; Object *o = named(OBJ_EVENT, n, &ex);
  if (!ex) { o->manual = fl & 1; o->signaled = (fl & 2) != 0; }
  SET_LAST_ERROR(ex ? 183 : 0); free(n); RET(rt_handle_new(o));
}
IMP(kernel32, CreateEventExA) { u32 fl = (u32)ARG(2); int ex; Object *o = named(OBJ_EVENT, ARG(1) ? GSTR(ARG(1)) : 0, &ex); if (!ex) { o->manual = fl & 1; o->signaled = (fl & 2) != 0; } SET_LAST_ERROR(ex ? 183 : 0); RET(rt_handle_new(o)); }
IMP(kernel32, GetLocaleInfoEx) { // (locale name, type, buffer, size): the invariant "C" answers the C++ runtime asks for
  u32 type = (u32)ARG(1) & 0xFFFF, n = (u32)ARG(3); const char *v = type == 0x0E ? "." : type == 0x0F ? "," : type == 0x10 ? "3;0" : type == 0x1004 ? "1252" : type == 0x0B ? "1252" : "";
  size_t k = strlen(v) + 1; if (!n) { RET(k); return; } if (n < k) { SET_LAST_ERROR(122); RET(0); return; }
  for (size_t i = 0; i < k; i++) M16(ARG(2) + 2 * i) = (u8)v[i]; RET(k);
}
IMP(kernel32, SetEvent) {
  Object *o = rt_handle_get(ARG(0)); if (!o) { RET(0); return; }
  pthread_mutex_lock(&rt_wait_lock); o->signaled = 1; rt_signal(); pthread_mutex_unlock(&rt_wait_lock); RET(1);
}
IMP(kernel32, ResetEvent) {
  Object *o = rt_handle_get(ARG(0)); if (!o) { RET(0); return; }
  pthread_mutex_lock(&rt_wait_lock); o->signaled = 0; pthread_mutex_unlock(&rt_wait_lock); RET(1);
}
static void create_semaphore(Cpu *c, const char *name) {
  int ex; Object *o = named(OBJ_SEMAPHORE, name, &ex);
  if (!ex) { o->count = (s32)ARG(1); o->max = (s32)ARG(2); }
  SET_LAST_ERROR(ex ? 183 : 0);
  RET(rt_handle_new(o));
}
IMP(kernel32, CreateSemaphoreA) { create_semaphore(c, ARG(3) ? GSTR(ARG(3)) : 0); }
IMP(kernel32, CreateSemaphoreW) { char *n = ARG(3) ? rt_narrow(GWSTR(ARG(3))) : 0; create_semaphore(c, n); free(n); }
IMP(kernel32, ReleaseSemaphore) {
  Object *o = rt_handle_get(ARG(0)); s32 n = (s32)ARG(1);
  if (!o || o->type != OBJ_SEMAPHORE) { SET_LAST_ERROR(6); RET(0); return; }
  pthread_mutex_lock(&rt_wait_lock);
  if (o->count + n > o->max) { pthread_mutex_unlock(&rt_wait_lock); SET_LAST_ERROR(298); RET(0); return; }
  if (ARG(2)) M32(ARG(2)) = (u32)o->count;
  o->count += n; rt_signal(); pthread_mutex_unlock(&rt_wait_lock);
  RET(1);
}
static void create_mutex(Cpu *c, int owned, const char *name) {
  int ex; Object *o = named(OBJ_MUTEX, name, &ex);
  if (!ex && owned) { o->owner = THREAD(c); o->recursion = 1; }
  SET_LAST_ERROR(ex ? 183 : 0);
  RET(rt_handle_new(o));
}
IMP(kernel32, CreateMutexA) { create_mutex(c, ARG(1) != 0, ARG(2) ? GSTR(ARG(2)) : 0); }
IMP(kernel32, CreateMutexW) { char *n = ARG(2) ? rt_narrow(GWSTR(ARG(2))) : 0; create_mutex(c, ARG(1) != 0, n); free(n); }
IMP(kernel32, ReleaseMutex) {
  Object *o = rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_MUTEX || o->owner != THREAD(c)) { SET_LAST_ERROR(288); RET(0); return; }
  pthread_mutex_lock(&rt_wait_lock);
  if (--o->recursion == 0) { o->owner = 0; rt_signal(); }
  pthread_mutex_unlock(&rt_wait_lock);
  RET(1);
}
IMP(kernel32, WaitForSingleObject) { u64 h = ARG(0); RET(rt_wait(c, 1, &h, 0, (u32)ARG(1))); }
IMP(kernel32, WaitForSingleObjectEx) { u64 h = ARG(0); RET(rt_wait(c, 1, &h, 0, (u32)ARG(1))); }
IMP(kernel32, WaitForMultipleObjects) {
  u32 n = (u32)ARG(0); u64 hs[64];
  for (u32 i = 0; i < n && i < 64; i++) hs[i] = MP(ARG(1) + PTR_SIZE * i);
  RET(rt_wait(c, n, hs, ARG(2) != 0, (u32)ARG(3)));
}

// ---- critical sections: a recursive pthread mutex behind the guest's structure (40 bytes in 64-bit code, 24 in
// 32-bit), kept in its first pointer-sized field (DebugInfo), which needs no more than the pointer's alignment ----
#if GUEST_BITS == 32
typedef u32 CsCell;
#define CS_BYTES 24
#else
typedef u64 CsCell;
#define CS_BYTES 40
#endif
static pthread_mutex_t cs_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_mutex(u64 cs) {
  pthread_mutex_t *m = (pthread_mutex_t *)GP(__atomic_load_n((CsCell *)GP(cs), __ATOMIC_ACQUIRE));
  if (m) return m;
  pthread_mutex_lock(&cs_init_lock);
  m = (pthread_mutex_t *)GP(MP(cs));
  if (!m) {
    m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    __atomic_store_n((CsCell *)GP(cs), (CsCell)(uptr)m, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&cs_init_lock);
  return m;
}
IMP(kernel32, InitializeCriticalSection) { memset((void *)GP(ARG(0)), 0, CS_BYTES); cs_mutex(ARG(0)); }
IMP(kernel32, InitializeCriticalSectionAndSpinCount) { memset((void *)GP(ARG(0)), 0, CS_BYTES); cs_mutex(ARG(0)); RET(1); }
IMP(kernel32, InitializeCriticalSectionEx) { memset((void *)GP(ARG(0)), 0, CS_BYTES); cs_mutex(ARG(0)); RET(1); }
IMP(kernel32, DeleteCriticalSection) {
  pthread_mutex_t *m = (pthread_mutex_t *)GP(MP(ARG(0)));
  if (m) { pthread_mutex_destroy(m); free(m); MP(ARG(0)) = 0; }
}
IMP(kernel32, EnterCriticalSection) { pthread_mutex_lock(cs_mutex(ARG(0))); }
IMP(kernel32, LeaveCriticalSection) { pthread_mutex_unlock(cs_mutex(ARG(0))); }
IMP(kernel32, TryEnterCriticalSection) { RET(pthread_mutex_trylock(cs_mutex(ARG(0))) == 0); }

// ---- memory ----
typedef struct Region { u64 base, size; u32 protect; struct Region *next; } Region;
static Region *regions;
static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;
IMP(kernel32, VirtualAlloc) {
  u64 addr = ARG(0), size = ARG(1); u32 type = (u32)ARG(2), prot = (u32)ARG(3);
  if (type & 0x20000000) { SET_LAST_ERROR(1314); RET(0); return; } // MEM_LARGE_PAGES
  if (addr) {
    // commit inside a reservation of ours: already backed
    pthread_mutex_lock(&region_lock);
    for (Region *r = regions; r; r = r->next) if (addr >= r->base && addr + size <= r->base + r->size) { pthread_mutex_unlock(&region_lock); RET(addr); return; }
    pthread_mutex_unlock(&region_lock);
    if (rt_trace) rt_log("VirtualAlloc at a fixed address 0x%llx (+0x%llx): placed elsewhere", addr, size);
  }
  u64 p = rt_valloc(size, 65536);
  if (!p) { SET_LAST_ERROR(8); RET(0); return; }
  Region *r = malloc(sizeof *r); r->base = p; r->size = (size + 4095) & ~4095ull; r->protect = prot;
  pthread_mutex_lock(&region_lock); r->next = regions; regions = r; pthread_mutex_unlock(&region_lock);
  RET(p);
}
IMP(kernel32, VirtualFree) {
  u64 addr = ARG(0); u32 type = (u32)ARG(2);
  if ((type & 0x8000) && rt_gfx_free(addr)) { RET(1); return; } // graphics memory (gdk.c)
  if (type & 0x8000) { // MEM_RELEASE
    pthread_mutex_lock(&region_lock);
    for (Region **pr = &regions; *pr; pr = &(*pr)->next) if ((*pr)->base == addr) { Region *r = *pr; *pr = r->next; free(r); rt_vfree(addr); break; }
    pthread_mutex_unlock(&region_lock);
  }
  RET(1);
}
IMP(kernel32, VirtualProtect) { if (ARG(3)) M32(ARG(3)) = 4; RET(1); }
static void virtual_query(Cpu *c, u64 a, u64 out);
IMP(kernel32, VirtualQuery) { virtual_query(c, ARG(0), ARG(1)); }
IMP(kernel32, VirtualQueryEx) { virtual_query(c, ARG(1), ARG(2)); }   // (process, address, info, size): this process's
// MEMORY_BASIC_INFORMATION: base, allocation base, allocation protection, region size, state (MEM_COMMIT), protection,
// type (MEM_PRIVATE); pointer-sized fields, so 28 bytes for a 32-bit title and 48 for a 64-bit one
static void put_mbi(u64 out, u64 base, u64 alloc, u32 aprot, u64 size, u32 prot) {
#if GUEST_BITS == 32
  memset((void *)GP(out), 0, 28);
  M32(out) = (u32)base; M32(out + 4) = (u32)alloc; M32(out + 8) = aprot; M32(out + 12) = (u32)size; M32(out + 16) = 0x1000; M32(out + 20) = prot; M32(out + 24) = 0x20000;
#else
  memset((void *)GP(out), 0, 48);
  M64(out) = base; M64(out + 8) = alloc; M32(out + 16) = aprot; M64(out + 24) = size; M32(out + 32) = 0x1000; M32(out + 36) = prot; M32(out + 40) = 0x20000;
#endif
}
static void virtual_query(Cpu *c, u64 a, u64 out) {
  u32 n = GUEST_BITS == 32 ? 28 : 48;
  pthread_mutex_lock(&region_lock);
  for (Region *r = regions; r; r = r->next) if (a >= r->base && a < r->base + r->size) {
    put_mbi(out, a & ~4095ull, r->base, r->protect, r->base + r->size - (a & ~4095ull), r->protect);
    pthread_mutex_unlock(&region_lock); RET(n); return;
  }
  pthread_mutex_unlock(&region_lock);
  put_mbi(out, a & ~4095ull, a & ~4095ull, 4, 4096, 4);
  RET(n);
}
IMP(kernel32, GetProcessHeap) { RET(0x7000); }
// What a block the title did not ask to be zeroed holds. Windows hands back a small block with whatever was there
// before (only a big one, from fresh pages, is zero), and a title can depend on that by accident: Narbacular
// Drop's player, as it starts a level, attaches the camera to itself only if a pointer it sets a few lines later
// is already non-zero (0x41d9cb tests [this+0x1e4], 0x41da5e sets it), and without the camera the level's first
// frame reads through a stale index. Here the heap is the runtime's malloc, whose leftovers depend on the runtime
// and the host as much as on the title, so a recipe can ask for small blocks filled with one byte instead
// ([quirks] heap_fill = 0xCD: the same on every host, and non-zero as the game's runs under Wine show it found there).
static void heap_fresh(void *p, u64 from, u64 to) {
  if (p && to > from && rt_heap_fill >= 0) memset((u8 *)p + from, to >= 0x7F000 ? 0 : rt_heap_fill, to - from);   // 0x7F000: above it Windows' heap allocates fresh pages
}
// The size a block was asked for: what Windows' HeapSize answers (malloc_usable_size is rounded up). A title's
// CRT grows its atexit table when HeapSize says the table is full, so a larger answer changes its calls. Kept
// beside the heap, by address (open addressing; a freed entry becomes a tombstone), so blocks stay as malloc
// lays them out.
static struct { uptr *key; u64 *size; u32 cap, used; } asked;
static pthread_mutex_t asked_lock = PTHREAD_MUTEX_INITIALIZER;
#define ASKED_GONE ((uptr)1)
static u32 asked_slot(uptr p) { u64 h = (u64)p * 0x9E3779B97F4A7C15ull; return (u32)(h >> 32) & (asked.cap - 1); }
static void asked_put_locked(uptr p, u64 n) {
  if ((asked.used + 1) * 2 > asked.cap) {   // grow (tombstones dropped)
    uptr *ok = asked.key; u64 *os = asked.size; u32 oc = asked.cap;
    asked.cap = oc ? oc * 2 : 4096; asked.key = calloc(asked.cap, sizeof *asked.key); asked.size = malloc(asked.cap * sizeof *asked.size); asked.used = 0;
    for (u32 i = 0; i < oc; i++) if (ok[i] > ASKED_GONE) { u32 j = asked_slot(ok[i]); while (asked.key[j]) j = (j + 1) & (asked.cap - 1); asked.key[j] = ok[i]; asked.size[j] = os[i]; asked.used++; }
    free(ok); free(os);
  }
  u32 j = asked_slot(p), tomb = ~0u;
  for (; asked.key[j]; j = (j + 1) & (asked.cap - 1)) {
    if (asked.key[j] == p) { asked.size[j] = n; return; }
    if (asked.key[j] == ASKED_GONE && tomb == ~0u) tomb = j;
  }
  if (tomb != ~0u) j = tomb; else asked.used++;
  asked.key[j] = p; asked.size[j] = n;
}
static void asked_drop_locked(uptr p) {
  if (!asked.cap || !p) return;
  for (u32 j = asked_slot(p); asked.key[j]; j = (j + 1) & (asked.cap - 1)) if (asked.key[j] == p) { asked.key[j] = ASKED_GONE; return; }
}
static void asked_put(void *p, u64 n) { if (!p) return; pthread_mutex_lock(&asked_lock); asked_put_locked((uptr)p, n); pthread_mutex_unlock(&asked_lock); }
static void asked_drop(void *p) { pthread_mutex_lock(&asked_lock); asked_drop_locked((uptr)p); pthread_mutex_unlock(&asked_lock); }
static u64 asked_size(void *p) {   // a block the heap functions did not hand out: its usable size
  u64 n = p ? malloc_usable_size(p) : 0;
  pthread_mutex_lock(&asked_lock);
  if (asked.cap && p) for (u32 j = asked_slot((uptr)p); asked.key[j]; j = (j + 1) & (asked.cap - 1)) if (asked.key[j] == (uptr)p) { n = asked.size[j]; break; }
  pthread_mutex_unlock(&asked_lock);
  return n;
}
IMP(kernel32, HeapAlloc) { u64 n = ARG(2); void *p = (ARG(1) & 8) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); if (!(ARG(1) & 8)) heap_fresh(p, 0, n); asked_put(p, n); RET((uptr)p); }
// The global heap of 16-bit Windows: a handle is the block itself (GMEM_MOVEABLE blocks never move), locked or not
IMP(kernel32, GlobalAlloc) { u64 n = ARG(1); void *p = (ARG(0) & 0x40) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); if (!(ARG(0) & 0x40)) heap_fresh(p, 0, n); asked_put(p, n); RET((uptr)p); }
IMP(kernel32, GlobalFree) { void *p = (void *)GP(ARG(0)); asked_drop(p); free(p); RET(0); }
IMP(kernel32, GlobalLock) { RET(ARG(0)); }
IMP(kernel32, GlobalUnlock) { SET_LAST_ERROR(0); RET(0); }
IMP(kernel32, GlobalHandle) { RET(ARG(0)); }
IMP(kernel32, GlobalSize) { RET(ARG(0) ? asked_size((void *)GP(ARG(0))) : 0); }
IMP(kernel32, HeapFree) { void *p = (void *)GP(ARG(2)); asked_drop(p); free(p); RET(1); }
IMP(kernel32, HeapReAlloc) { // (heap, flags, block, size): a grown block's new bytes are zero with HEAP_ZERO_MEMORY (8)
  void *old = (void *)GP(ARG(2)); u64 n = ARG(3), had = asked_size(old), room = old ? malloc_usable_size(old) : 0;
  if (ARG(1) & 0x10) { if (n > room) { RET(0); return; } }   // HEAP_REALLOC_IN_PLACE_ONLY: it fits in the block or fails
  u8 *p = (ARG(1) & 0x10) ? old : realloc(old, n ? n : 1);
  if (p && n > had) { if (ARG(1) & 8) memset(p + had, 0, n - had); else heap_fresh(p, had, n); }
  if (p) { pthread_mutex_lock(&asked_lock); if (p != (u8 *)old) asked_drop_locked((uptr)old); asked_put_locked((uptr)p, n); pthread_mutex_unlock(&asked_lock); }
  RET((uptr)p);
}
IMP(kernel32, HeapSize) { RET(asked_size((void *)GP(ARG(2)))); }
IMP(kernel32, LocalAlloc) { RET((uptr)((ARG(0) & 0x40) ? calloc(1, ARG(1) ? ARG(1) : 1) : malloc(ARG(1) ? ARG(1) : 1))); }
IMP(kernel32, LocalFree) { free((void *)GP(ARG(0))); RET(0); }

// file mappings without files: shared memory inside the process
// (file or INVALID_HANDLE_VALUE, security, protection, size high, size low, name). A file's mapping is a copy of its
// bytes, made now: what readers of a mapped file see (writes through the view do not reach the file).
IMP(kernel32, CreateFileMappingA) {
  u64 fh = ARG(0), size = (ARG(3) << 32) | (u32)ARG(4);
  VFile *vf = 0;
  if (fh != PSEUDO_PROCESS) {   // INVALID_HANDLE_VALUE: the pagefile, at the guest's width
    Object *f = rt_handle_get(fh);
    if (!f || f->type != OBJ_FILE || !f->vf) { SET_LAST_ERROR(6); RET(0); return; }
    vf = f->vf; if (!size) size = vfs_size(vf);
    if (!size) { SET_LAST_ERROR(1006); RET(0); return; }   // ERROR_FILE_INVALID: an empty file cannot be mapped
  }
  int ex; Object *o = named(OBJ_MAPPING, ARG(5) ? GSTR(ARG(5)) : 0, &ex);
  if (!ex) {
    o->size = size; o->view = rt_valloc(o->size, 65536);
    if (vf && o->view) { u64 *pos = vfs_pos(vf), keep = *pos; *pos = 0; u64 n = vfs_size(vf); vfs_read(vf, (void *)GP(o->view), n < size ? n : size); *pos = keep; }
  }
  SET_LAST_ERROR(ex ? 183 : 0);
  RET(o->view ? rt_handle_new(o) : 0);
}
IMP(kernel32, OpenFileMappingA) {
  int ex; Object *o = named(OBJ_MAPPING, GSTR(ARG(2)), &ex);
  if (!ex) { SET_LAST_ERROR(2); RET(0); return; }
  RET(rt_handle_new(o));
}
// A mapping lives while a handle or a view holds it: a view stays valid after the mapping's handle is closed, until
// UnmapViewOfFile. Then its bytes go too. (Tag maps every texture file it loads, dozens of times a second; when the
// bytes were never freed, it grew by about 100 KB a frame and failed at 2 GiB after five minutes of play.)
typedef struct View { u64 at; Object *o; struct View *next; } View;
static View *views;   // under rt_wait_lock
void rt_mapping_free(Object *o) { if (o->view) rt_vfree(o->view); free(o); }
IMP(kernel32, MapViewOfFile) {
  Object *o = rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_MAPPING) { RET(0); return; }
  View *v = malloc(sizeof *v); v->at = o->view + ((ARG(2) << 32) | (u32)ARG(3)); v->o = o;
  pthread_mutex_lock(&rt_wait_lock); o->refs++; v->next = views; views = v; pthread_mutex_unlock(&rt_wait_lock);
  RET(v->at);
}
IMP(kernel32, UnmapViewOfFile) {
  pthread_mutex_lock(&rt_wait_lock);
  for (View **pv = &views; *pv; pv = &(*pv)->next) if ((*pv)->at == ARG(0)) {
    View *v = *pv; Object *o = v->o; *pv = v->next; free(v);
    if (--o->refs == 0) rt_mapping_free(o);
    break;
  }
  pthread_mutex_unlock(&rt_wait_lock);
  RET(1);
}

// ---- time ----
// (the clock itself is clock.c)
#if GUEST_BITS == 32
// 32-bit titles: every clock is the title's clock
IMP(kernel32, QueryPerformanceCounter) { rt_clock_polled(); M64(ARG(0)) = rt_uptime_us(); RET(1); }
IMP(kernel32, QueryPerformanceFrequency) { M64(ARG(0)) = 1000000; RET(1); }
IMP(kernel32, GetTickCount) { rt_clock_polled(); RET((u32)(rt_uptime_us() / 1000)); }
IMP(kernel32, GetTickCount64) { RET64(rt_uptime_us() / 1000); }
static u64 filetime_now(void) { return rt_now_us() * 10 + 116444736000000000ull; }
#else
// 64-bit titles: the host's clocks, or with SW_CLOCK the title's (a Windows x64 desktop title's sessions)
static u64 filetime_now(void) { if (rt_clock_virtual()) return rt_now_us() * 10 + 116444736000000000ull; struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (u64)t.tv_sec * 10000000 + (u64)t.tv_nsec / 100 + 116444736000000000ull; }
IMP(kernel32, QueryPerformanceCounter) { if (rt_clock_virtual()) { rt_clock_polled(); M64(ARG(0)) = rt_uptime_us(); } else M64(ARG(0)) = (u64)host_now_us(); RET(1); }
IMP(kernel32, QueryPerformanceFrequency) { M64(ARG(0)) = 1000000; RET(1); }
IMP(kernel32, GetTickCount) { if (rt_clock_virtual()) { rt_clock_polled(); RET((u32)(rt_uptime_us() / 1000)); } else RET((u32)(host_now_us() / 1000)); }
IMP(kernel32, GetTickCount64) { RET(rt_clock_virtual() ? rt_uptime_us() / 1000 : (u64)(host_now_us() / 1000)); }
#endif
IMP(kernel32, GetSystemTimeAsFileTime) { M64(ARG(0)) = filetime_now(); }
IMP(kernel32, GetSystemTimePreciseAsFileTime) { M64(ARG(0)) = filetime_now(); }
IMP(kernel32, GetSystemTimeAdjustment) { M32(ARG(0)) = 156250; M32(ARG(1)) = 156250; M32(ARG(2)) = 1; RET(1); }
IMP(kernel32, SetSystemTime) { SET_LAST_ERROR(1314); RET(0); }
// The time zone is UTC: local time is the system time
static void put_systemtime(u64 ft, u64 out) {
  time_t s = (time_t)((ft - 116444736000000000ull) / 10000000); struct tm tm; gmtime_r(&s, &tm);
  M16(out) = tm.tm_year + 1900; M16(out + 2) = tm.tm_mon + 1; M16(out + 4) = tm.tm_wday; M16(out + 6) = tm.tm_mday;
  M16(out + 8) = tm.tm_hour; M16(out + 10) = tm.tm_min; M16(out + 12) = tm.tm_sec; M16(out + 14) = (ft / 10000) % 1000;
}
IMP(kernel32, GetSystemTime) { put_systemtime(filetime_now(), ARG(0)); }
IMP(kernel32, GetLocalTime) { put_systemtime(filetime_now(), ARG(0)); }
IMP(kernel32, FileTimeToLocalFileTime) { M64(ARG(1)) = M64(ARG(0)); RET(1); }
IMP(kernel32, GetTimeZoneInformation) { memset((void *)GP(ARG(0)), 0, 172); RET(0); }   // TIME_ZONE_ID_UNKNOWN: no daylight saving
IMP(kernel32, FileTimeToSystemTime) {
  u64 ft = M64(ARG(0)), out = ARG(1);
  time_t s = (time_t)((ft - 116444736000000000ull) / 10000000); struct tm tm; gmtime_r(&s, &tm);
  M16(out) = tm.tm_year + 1900; M16(out + 2) = tm.tm_mon + 1; M16(out + 4) = tm.tm_wday; M16(out + 6) = tm.tm_mday;
  M16(out + 8) = tm.tm_hour; M16(out + 10) = tm.tm_min; M16(out + 12) = tm.tm_sec; M16(out + 14) = (ft / 10000) % 1000;
  RET(1);
}
static void cpu_times(u64 a, u64 b, u64 k, u64 u) {
  u64 now = filetime_now(), run = (u64)(host_now_us() * 10);
  if (a) M64(a) = now - run; if (b) M64(b) = 0; if (k) M64(k) = 0; if (u) M64(u) = run;
}
IMP(kernel32, GetProcessTimes) { cpu_times(ARG(1), ARG(2), ARG(3), ARG(4)); RET(1); }
IMP(kernel32, GetThreadTimes) { cpu_times(ARG(1), ARG(2), ARG(3), ARG(4)); RET(1); }

// ---- modules ----
// modules: lifted images are real modules (their base); system DLLs are pseudo handles whose functions
// GetProcAddress finds in the runtime's import table
static const struct { const char *name; u64 h; } system_modules[] = {
  {"kernel32", 0x7FF00000ull}, {"kernelbase", 0x7FF10000ull}, {"ntdll", 0x7FF20000ull}, {"kernelx", 0x7FF30000ull},
  {"xgameruntime", 0x7FF40000ull}, {"ucrtbase", 0x7FF50000ull}, {"msvcrt", 0x7FF60000ull},
  {"user32", 0x7FF70000ull}, {"advapi32", 0x7FF80000ull}, {"gameinput", 0x7FF90000ull}, {"xmem", 0x7FFA0000ull},
  {"d3d12_xs", 0x7FFB0000ull}, {"d3d12", 0x7FFC0000ull}, {"dxgi", 0x7FFD0000ull}, {"xg_xs", 0x7FFE0000ull}, {"xcrdapi", 0x7FFF0000ull},
  {"mfplat", 0x7FE00000ull}, {"d3dcompiler_47", 0x7FE10000ull}, {"xaudio2_9", 0x7FE20000ull},
#if GUEST_BITS == 32
  // the desktop's DLLs a title looks functions up in (rt/windows-x86/)
  {"gdi32", 0x7FD00000ull}, {"d3d9", 0x7FD10000ull}, {"dinput8", 0x7FD20000ull}, {"winmm", 0x7FD30000ull}, {"ddraw", 0x7FD40000ull}, {"opengl32", 0x7FD50000ull}, {"dsound", 0x7FD60000ull},
#endif
  // API sets and console variants: the DLL behind them
  {"ntuser", 0x7FF70000ull}, {"minuser", 0x7FF70000ull}, {"advapi32legacy", 0x7FF80000ull}, {"api-ms-win-core", 0x7FF00000ull},
};
static u64 module_handle(const char *n) {
  const ModuleDef *m = rt_module_named(n);
  if (m) return rt_module_loaded(m) ? m->base : 0;   // an on-demand DLL has no handle until LoadLibrary loads it
#if GUEST_BITS == 32
  // a desktop title names a DLL exactly: "d3d9d.dll" (Direct3D's debug build, which D3DX looks for) is not "d3d9.dll"
  const char *base = strrchr(n, '\\'); base = base ? base + 1 : n;
  size_t len = strlen(base); if (len > 4 && !strcasecmp(base + len - 4, ".dll")) len -= 4;
  for (unsigned i = 0; i < sizeof system_modules / sizeof system_modules[0]; i++) if (strlen(system_modules[i].name) == len && !strncasecmp(base, system_modules[i].name, len)) return system_modules[i].h;
#else
  for (unsigned i = 0; i < sizeof system_modules / sizeof system_modules[0]; i++) if (strcasestr(n, system_modules[i].name)) return system_modules[i].h;
#endif
  return 0;
}
static const char *system_name(u64 h) {
  for (unsigned i = 0; i < sizeof system_modules / sizeof system_modules[0]; i++) if (system_modules[i].h == h) return system_modules[i].name;
  return 0;
}
// (the first entry with a handle names the module GetProcAddress resolves against)
IMP(kernel32, GetModuleHandleA) { RET(ARG(0) ? module_handle(GSTR(ARG(0))) : image_base); }
IMP(kernel32, GetModuleHandleW) {
  if (!ARG(0)) { RET(image_base); return; }
  char *n = rt_narrow(GWSTR(ARG(0))); RET(module_handle(n)); free(n);
}
IMP(kernel32, GetModuleHandleExW) {
  u32 flags = (u32)ARG(0); u64 h;
  if (flags & 4) { const ModuleDef *m = rt_module_of(ARG(1)); h = m ? m->base : 0; } // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
  else if (!ARG(1)) h = image_base;
  else { char *n = rt_narrow(GWSTR(ARG(1))); h = module_handle(n); free(n); }
  MP(ARG(2)) = h; if (!h) SET_LAST_ERROR(126); RET(h != 0);
}
static const char *module_path_of(u64 h) {
  static char buf[16][300]; static int k;
  const ModuleDef *m = h ? rt_module_of(h) : &rt_modules[0];
  if (!m) return 0;
  char *b = buf[k++ & 15];
  if (!strncasecmp(m->name, "c:/", 3)) snprintf(b, 300, "C:\\%s", m->name + 3);   // a DLL lifted from the image's drive
  else snprintf(b, 300, "C:\\title\\%s", m->name);
  for (char *p = b; *p; p++) if (*p == '/') *p = '\\';
  return b;
}
IMP(kernel32, GetModuleFileNameA) {
  u32 n = (u32)ARG(2); const char *p = module_path_of(ARG(0));
  if (!n || !p) { RET(0); return; }
  snprintf(GSTR(ARG(1)), n, "%s", p); RET(strlen(GSTR(ARG(1))));
}
IMP(kernel32, GetModuleFileNameW) {
  u32 n = (u32)ARG(2), i = 0; const char *p = module_path_of(ARG(0));
  if (!n || !p) { RET(0); return; }
  for (; p[i] && i + 1 < n; i++) GWSTR(ARG(1))[i] = (u8)p[i];
  GWSTR(ARG(1))[i] = 0;
  RET(i);
}
static void loaded(Cpu *c, const char *n) {
  const ModuleDef *m = rt_module_named(n);
  u64 h = m ? rt_module_load(c, m) : module_handle(n);
  if (!h) SET_LAST_ERROR(126);
  if (rt_trace) rt_log("LoadLibrary %s -> 0x%llx", n, h);
  RET(h);
}
IMP(kernel32, LoadLibraryA) { loaded(c, GSTR(ARG(0))); }
IMP(kernel32, LoadLibraryExA) { loaded(c, GSTR(ARG(0))); }
IMP(kernel32, LoadLibraryW) { char *n = rt_narrow(GWSTR(ARG(0))); loaded(c, n); free(n); }
IMP(kernel32, LoadLibraryExW) { char *n = rt_narrow(GWSTR(ARG(0))); loaded(c, n); free(n); }
IMP(kernel32, FreeLibrary) { const ModuleDef *m = rt_module_of(ARG(0)); RET(m && m->base == ARG(0) ? rt_module_free(c, m) : 1); }
IMP(kernel32, GetProcAddress) {
  u64 h = ARG(0), name = ARG(1);
  const ModuleDef *m = rt_module_of(h);
  if (m && m->base == h) { u64 a = rt_module_export(m, name < 0x10000 ? (const char *)(uptr)name : GSTR(name)); if (!a) SET_LAST_ERROR(127); RET(a); return; }
  const char *sys = system_name(h);
  if (!sys || name < 0x10000) { SET_LAST_ERROR(127); RET(0); return; }
  u64 a = rt_dynamic_import(sys, GSTR(name));
  if (!a && !strcmp(sys, "kernel32")) a = rt_dynamic_import("kernelbase", GSTR(name));
  if (!a) SET_LAST_ERROR(127);
  RET(a);
}

// ---- strings and code pages ----
IMP(kernel32, GetCommandLineA) { static u64 p; if (!p) { p = (uptr)strdup(rt_command_line); } RET(p); }
IMP(kernel32, GetCommandLineW) { static u64 p; if (!p) p = (uptr)rt_widen_guest(rt_command_line); RET(p); }
IMP(kernel32, GetACP) { RET(GUEST_BITS == 32 ? 1252 : 65001); } // an old 32-bit title's C runtime builds its tables from a single-byte code page
IMP(kernel32, GetOEMCP) { RET(437); }
IMP(kernel32, GetCPInfo) { u64 p = ARG(1); memset((void *)GP(p), 0, 20); M32(p) = 1; M8(p + 4) = '?'; RET(1); }
IMP(kernel32, SetConsoleCP) { RET(1); }
IMP(kernel32, SetConsoleOutputCP) { RET(1); }
IMP(kernel32, GetConsoleMode) { RET(0); }
// UTF-8 or ASCII <-> UTF-16
IMP(kernel32, MultiByteToWideChar) {
  const u8 *s = (const u8 *)GP(ARG(2)); s32 n = (s32)ARG(3); wchar16 *out = GWSTR(ARG(4)); s32 cap = (s32)ARG(5);
  if (n < 0) n = (s32)strlen((const char *)s) + 1;
#if GUEST_BITS == 32
  if ((u32)ARG(0) != 65001) { // a single-byte code page (1252, read as Latin-1): one character a byte
    if (cap) { if (n > cap) { SET_LAST_ERROR(122); RET(0); return; } for (s32 i = 0; i < n; i++) out[i] = s[i]; }
    RET(n); return;
  }
#endif
  s32 o = 0;
  for (s32 i = 0; i < n;) {
    u32 cp = s[i++];
    if (cp >= 0xC0 && i < n) { int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1; cp &= 0x3F >> extra; while (extra-- && i < n) cp = (cp << 6) | (s[i++] & 0x3F); }
    if (cp >= 0x10000) { if (cap) { if (o + 2 > cap) { SET_LAST_ERROR(122); RET(0); return; } out[o] = 0xD800 | ((cp - 0x10000) >> 10); out[o + 1] = 0xDC00 | (cp & 0x3FF); } o += 2; }
    else { if (cap) { if (o + 1 > cap) { SET_LAST_ERROR(122); RET(0); return; } out[o] = cp; } o++; }
  }
  RET(o);
}
IMP(kernel32, WideCharToMultiByte) {
  const wchar16 *s = GWSTR(ARG(2)); s32 n = (s32)ARG(3); u8 *out = (u8 *)GP(ARG(4)); s32 cap = (s32)ARG(5);
  if (n < 0) n = (s32)rt_wcslen(s) + 1;
#if GUEST_BITS == 32
  if ((u32)ARG(0) != 65001) { // a single-byte code page: one byte a character, '?' for what it lacks
    if (cap) { if (n > cap) { SET_LAST_ERROR(122); RET(0); return; } for (s32 i = 0; i < n; i++) out[i] = s[i] < 0x100 ? (u8)s[i] : '?'; }
    RET(n); return;
  }
#endif
  s32 o = 0;
  for (s32 i = 0; i < n; i++) {
    u32 cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
    u8 b[4]; int k = 0;
    if (cp < 0x80) b[k++] = cp;
    else if (cp < 0x800) { b[k++] = 0xC0 | cp >> 6; b[k++] = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { b[k++] = 0xE0 | cp >> 12; b[k++] = 0x80 | ((cp >> 6) & 0x3F); b[k++] = 0x80 | (cp & 0x3F); }
    else { b[k++] = 0xF0 | cp >> 18; b[k++] = 0x80 | ((cp >> 12) & 0x3F); b[k++] = 0x80 | ((cp >> 6) & 0x3F); b[k++] = 0x80 | (cp & 0x3F); }
    if (cap) { if (o + k > cap) { SET_LAST_ERROR(122); RET(0); return; } memcpy(out + o, b, k); }
    o += k;
  }
  RET(o);
}

// ---- ntdll functions kernel32 forwards: exception handling is not implemented yet ----

// ---- slim reader/writer locks and condition variables: runtime objects behind the guest's pointer-sized cell ----
typedef struct { pthread_mutex_t m; pthread_cond_t cv; int readers, writer; } Srw;
typedef struct { pthread_mutex_t m; pthread_cond_t cv; u64 seq; } Cond;
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;
#if GUEST_BITS == 32
typedef u32 LazyCell;
#else
typedef u64 LazyCell;
#endif
static void *lazy(u64 at, size_t size) {
  void *p = (void *)GP(__atomic_load_n((LazyCell *)GP(at), __ATOMIC_ACQUIRE));
  if (p) return p;
  pthread_mutex_lock(&lazy_lock);
  p = (void *)GP(MP(at));
  if (!p) { p = calloc(1, size); pthread_mutex_init((pthread_mutex_t *)p, 0); pthread_cond_init((pthread_cond_t *)((char *)p + sizeof(pthread_mutex_t)), 0); __atomic_store_n((LazyCell *)GP(at), (LazyCell)(uptr)p, __ATOMIC_RELEASE); }
  pthread_mutex_unlock(&lazy_lock);
  return p;
}
static void srw_lock(u64 at, int excl) {
  Srw *l = lazy(at, sizeof(Srw)); pthread_mutex_lock(&l->m);
  if (excl) { while (l->writer || l->readers) pthread_cond_wait(&l->cv, &l->m); l->writer = 1; }
  else { while (l->writer) pthread_cond_wait(&l->cv, &l->m); l->readers++; }
  pthread_mutex_unlock(&l->m);
}
static int srw_try(u64 at, int excl) {
  Srw *l = lazy(at, sizeof(Srw)); int ok = 0; pthread_mutex_lock(&l->m);
  if (excl ? !(l->writer || l->readers) : !l->writer) { if (excl) l->writer = 1; else l->readers++; ok = 1; }
  pthread_mutex_unlock(&l->m); return ok;
}
static void srw_unlock(u64 at, int excl) {
  Srw *l = lazy(at, sizeof(Srw)); pthread_mutex_lock(&l->m);
  if (excl) l->writer = 0; else l->readers--;
  pthread_cond_broadcast(&l->cv); pthread_mutex_unlock(&l->m);
}
IMP(kernel32, InitializeSRWLock) { MP(ARG(0)) = 0; }
IMP(kernel32, AcquireSRWLockExclusive) { srw_lock(ARG(0), 1); }
IMP(kernel32, AcquireSRWLockShared) { srw_lock(ARG(0), 0); }
IMP(kernel32, TryAcquireSRWLockExclusive) { RET(srw_try(ARG(0), 1)); }
IMP(kernel32, TryAcquireSRWLockShared) { RET(srw_try(ARG(0), 0)); }
IMP(kernel32, ReleaseSRWLockExclusive) { srw_unlock(ARG(0), 1); }
IMP(kernel32, ReleaseSRWLockShared) { srw_unlock(ARG(0), 0); }
IMP(kernel32, InitializeConditionVariable) { MP(ARG(0)) = 0; }
static void cond_wake(u64 at) { Cond *v = lazy(at, sizeof(Cond)); pthread_mutex_lock(&v->m); v->seq++; pthread_cond_broadcast(&v->cv); pthread_mutex_unlock(&v->m); }
IMP(kernel32, WakeConditionVariable) { cond_wake(ARG(0)); }
IMP(kernel32, WakeAllConditionVariable) { cond_wake(ARG(0)); }
// the waiter takes the condition's mutex before it lets go of the lock, so no wake falls between the two
static int cond_wait(Cpu *c, u64 at, u32 ms, void (*release)(u64, int), void (*acquire)(u64, int), u64 lock, int mode) {
  Cond *v = lazy(at, sizeof(Cond)); int ok = 1;
  pthread_mutex_lock(&v->m); u64 seq = v->seq;
  release(lock, mode);
  struct timespec until; if (ms != 0xFFFFFFFF) { clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; } }
  while (v->seq == seq) {
    if (ms == 0xFFFFFFFF) pthread_cond_wait(&v->cv, &v->m);
    else if (pthread_cond_timedwait(&v->cv, &v->m, &until)) { ok = v->seq != seq; break; }
  }
  pthread_mutex_unlock(&v->m);
  acquire(lock, mode);
  if (!ok) SET_LAST_ERROR(1460); // ERROR_TIMEOUT
  return ok;
}
static void srw_release(u64 l, int shared) { srw_unlock(l, !shared); }
static void srw_acquire(u64 l, int shared) { srw_lock(l, !shared); }
static void cs_release(u64 l, int x) { (void)x; pthread_mutex_unlock(cs_mutex(l)); }
static void cs_acquire(u64 l, int x) { (void)x; pthread_mutex_lock(cs_mutex(l)); }
IMP(kernel32, SleepConditionVariableSRW) { RET(cond_wait(c, ARG(0), (u32)ARG(2), srw_release, srw_acquire, ARG(1), (int)(ARG(3) & 1))); }
IMP(kernel32, SleepConditionVariableCS) { RET(cond_wait(c, ARG(0), (u32)ARG(2), cs_release, cs_acquire, ARG(1), 0)); }

// INIT_ONCE: 0 new, 1 running, 2 done
IMP(kernel32, InitOnceExecuteOnce) {
  u64 once = ARG(0), fn = ARG(1), param = ARG(2), ctx = ARG(3);
  for (;;) {
    u64 expect = 0;
    if (__atomic_compare_exchange_n((u64 *)GP(once), &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      u32 ok = (u32)rt_guest_call(c, fn, once, param, ctx, 0);
      __atomic_store_n((u64 *)GP(once), ok ? 2 : 0, __ATOMIC_RELEASE);
      RET(ok); return;
    }
    if (expect == 2) { RET(1); return; }
    sched_yield();
  }
}
IMP(kernel32, InitOnceBeginInitialize) {
  u64 once = ARG(0); u32 flags = (u32)ARG(1);
  for (;;) {
    u64 expect = 0;
    if (__atomic_compare_exchange_n((u64 *)GP(once), &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { if (ARG(2)) M32(ARG(2)) = 1; RET(1); return; }
    if (expect == 2) { if (ARG(2)) M32(ARG(2)) = 0; RET(1); return; }
    if (flags & 2) { SET_LAST_ERROR(170); RET(0); return; } // INIT_ONCE_CHECK_ONLY
    sched_yield();
  }
}
IMP(kernel32, InitOnceComplete) { __atomic_store_n((u64 *)GP(ARG(0)), (ARG(1) & 4) ? 0 : 2, __ATOMIC_RELEASE); RET(1); } // INIT_ONCE_INIT_FAILED

// fiber local storage: per thread, as TLS (no fibers)
#define TEB_FLS_SLOTS 0x1A00
static u32 fls_used[64];
IMP(kernel32, FlsAlloc) {
  pthread_mutex_lock(&rt_wait_lock);
  for (u32 i = 0; i < 64; i++) if (!fls_used[i]) { fls_used[i] = 1; pthread_mutex_unlock(&rt_wait_lock); RET(i); return; }
  pthread_mutex_unlock(&rt_wait_lock); RET(0xFFFFFFFF);
}
IMP(kernel32, FlsFree) { if (ARG(0) < 64) fls_used[ARG(0)] = 0; RET(1); }
IMP(kernel32, FlsGetValue) { RET(ARG(0) < 64 ? M64(THREAD(c)->teb + TEB_FLS_SLOTS + 8 * ARG(0)) : 0); }
IMP(kernel32, FlsSetValue) { if (ARG(0) < 64) M64(THREAD(c)->teb + TEB_FLS_SLOTS + 8 * ARG(0)) = ARG(1); RET(ARG(0) < 64); }
IMP(kernel32, GetThreadId) { Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0)); RET(o && o->type == OBJ_THREAD ? o->thread->id : 0); }

// ---- WaitOnAddress / WakeByAddress (Rust's and MSVC's STL locks): one wait list for all addresses ----
static pthread_mutex_t addr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t addr_cond = PTHREAD_COND_INITIALIZER;
static u64 addr_seq;
static int same(u64 a, u64 b, u64 size) {
  switch (size) { case 1: return __atomic_load_n((u8 *)GP(a), __ATOMIC_SEQ_CST) == M8(b); case 2: return __atomic_load_n((u16 *)GP(a), __ATOMIC_SEQ_CST) == M16(b);
  case 4: return __atomic_load_n((u32 *)GP(a), __ATOMIC_SEQ_CST) == M32(b); default: return __atomic_load_n((u64 *)GP(a), __ATOMIC_SEQ_CST) == M64(b); }
}
IMP(kernel32, WaitOnAddress) { // (address, compare, size, ms): returns when *address differs from *compare, or woken
  u64 a = ARG(0), cmp = ARG(1), size = ARG(2); u32 ms = (u32)ARG(3);
  struct timespec until; if (ms != 0xFFFFFFFF) { clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; } }
  pthread_mutex_lock(&addr_lock);
  u64 seq = addr_seq; int ok = 1;
  while (same(a, cmp, size) && seq == addr_seq) {
    if (ms == 0xFFFFFFFF) pthread_cond_wait(&addr_cond, &addr_lock);
    else if (pthread_cond_timedwait(&addr_cond, &addr_lock, &until)) { ok = !same(a, cmp, size) || seq != addr_seq; break; }
  }
  pthread_mutex_unlock(&addr_lock);
  if (!ok) SET_LAST_ERROR(1460);
  RET(ok);
}
static void addr_wake(void) { pthread_mutex_lock(&addr_lock); addr_seq++; pthread_cond_broadcast(&addr_cond); pthread_mutex_unlock(&addr_lock); }
IMP(kernel32, WakeByAddressSingle) { addr_wake(); }
IMP(kernel32, WakeByAddressAll) { addr_wake(); }

// ---- interlocked singly linked lists; one lock for all ----
// SLIST_HEADER: the first entry, then the depth. 32-bit: Next (4 bytes), Depth (2), Sequence (2); 64-bit (the header
// we write): Next (8), Depth (2 at +8). An entry starts with its Next pointer (pointer-sized).
static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;
#define SL_DEPTH(h) M16((h) + PTR_SIZE)
IMP(kernel32, InitializeSListHead) { u64 h = ARG(0); M64(h) = 0; if (PTR_SIZE == 8) M64(h + 8) = 0; } // 8 bytes (32-bit) or 16
IMP(kernel32, InterlockedPushEntrySList) { pthread_mutex_lock(&slist_lock); u64 h = ARG(0), e = ARG(1), old = MP(h); MP(e) = old; MP(h) = e; SL_DEPTH(h) += 1; if (PTR_SIZE == 4) M16(h + 6) += 1; pthread_mutex_unlock(&slist_lock); RET(old); }
IMP(kernel32, InterlockedPopEntrySList) { pthread_mutex_lock(&slist_lock); u64 h = ARG(0), e = MP(h); if (e) { MP(h) = MP(e); SL_DEPTH(h) -= 1; if (PTR_SIZE == 4) M16(h + 6) += 1; } pthread_mutex_unlock(&slist_lock); RET(e); }
IMP(kernel32, InterlockedFlushSList) { pthread_mutex_lock(&slist_lock); u64 h = ARG(0), e = MP(h); MP(h) = 0; SL_DEPTH(h) = 0; pthread_mutex_unlock(&slist_lock); RET(e); }
IMP(kernel32, QueryDepthSList) { RET(SL_DEPTH(ARG(0))); }
IMP(kernel32, EncodePointer) { RET(ARG(0)); } // no secret to mix in: the identity, which DecodePointer undoes
IMP(kernel32, DecodePointer) { RET(ARG(0)); }
IMP(kernel32, DisableThreadLibraryCalls) { RET(1); }
IMP(kernel32, RtlPcToFileHeader) { // (pc, &base): the image holding pc
  const ModuleDef *m = rt_module_of(ARG(0)); u64 b = m ? m->base : 0;
  if (ARG(1)) MP(ARG(1)) = b; RET(b);
}
// STARTUPINFO: 68 bytes for a 32-bit guest, 104 for a 64-bit one; all zero but its size
#define STARTUPINFO_SIZE (GUEST_BITS == 32 ? 68 : 104)
IMP(kernel32, GetStartupInfoW) { u64 p = ARG(0); memset((void *)GP(p), 0, STARTUPINFO_SIZE); M32(p) = STARTUPINFO_SIZE; }
IMP(kernel32, GetStartupInfoA) { u64 p = ARG(0); memset((void *)GP(p), 0, STARTUPINFO_SIZE); M32(p) = STARTUPINFO_SIZE; }
IMP(kernel32, SetCurrentDirectoryA) { RET(1); } // the title's folder stays the current one (as SetCurrentDirectoryW)
IMP(kernel32, RoInitialize) { RET(0); }
IMP(kernel32, RoUninitialize) {}

// ---- the thread pool's work objects: each submit runs the callback on a thread of its own ----
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg); // process.c
typedef struct { u32 magic; u64 cb, ctx; int pending; pthread_mutex_t m; pthread_cond_t cv; } TpWork;
#define TPMAGIC 0x54505752u
static TpWork *tp_work(u64 p) { TpWork *w = (TpWork *)GP(p); return p && w->magic == TPMAGIC ? w : 0; }
typedef struct { TpWork *w; u64 cb, ctx, simple; } TpRun;
static void tp_main(Cpu *c, void *arg) {
  TpRun *r = arg; static u8 instance[64]; // PTP_CALLBACK_INSTANCE: an opaque pointer the callback may hand back to us
  if (r->simple) rt_guest_call(c, r->cb, (uptr)instance, r->ctx, 0, 0);      // (instance, context)
  else rt_guest_call(c, r->cb, (uptr)instance, r->ctx, (uptr)r->w, 0);      // (instance, context, work)
  if (r->w) { pthread_mutex_lock(&r->w->m); r->w->pending--; pthread_cond_broadcast(&r->w->cv); pthread_mutex_unlock(&r->w->m); }
  free(r);
}
IMP(kernel32, CreateThreadpoolWork) { // (callback, context, environment)
  TpWork *w = calloc(1, sizeof *w); w->magic = TPMAGIC; w->cb = ARG(0); w->ctx = ARG(1);
  pthread_mutex_init(&w->m, 0); pthread_cond_init(&w->cv, 0); RET((uptr)w);
}
IMP(kernel32, SubmitThreadpoolWork) {
  TpWork *w = tp_work(ARG(0)); if (!w) return;
  pthread_mutex_lock(&w->m); w->pending++; pthread_mutex_unlock(&w->m);
  TpRun *r = calloc(1, sizeof *r); r->w = w; r->cb = w->cb; r->ctx = w->ctx; rt_spawn_native(tp_main, r);
}
IMP(kernel32, WaitForThreadpoolWorkCallbacks) { // (work, cancel pending): ours have all started
  TpWork *w = tp_work(ARG(0)); if (!w) return;
  pthread_mutex_lock(&w->m); while (w->pending) pthread_cond_wait(&w->cv, &w->m); pthread_mutex_unlock(&w->m);
}
IMP(kernel32, CloseThreadpoolWork) {} // kept: a callback still running may name it
IMP(kernel32, TrySubmitThreadpoolCallback) { TpRun *r = calloc(1, sizeof *r); r->cb = ARG(0); r->ctx = ARG(1); r->simple = 1; rt_spawn_native(tp_main, r); RET(1); }
IMP(kernel32, CallbackMayRunLong) { RET(1); }
IMP(kernel32, FreeLibraryWhenCallbackReturns) {} // modules are never unloaded here

// ---- psapi (kernel32's K32 names): the lifted images, for code that looks for its modules' unwind tables ----
IMP(kernel32, K32EnumProcessModules) { // (process, modules[], bytes, &needed)
  u64 out = ARG(1); u32 cap = (u32)ARG(2) / PTR_SIZE;
  for (unsigned i = 0; i < rt_module_count && i < cap; i++) MP(out + PTR_SIZE * i) = rt_modules[i].base;
  if (ARG(3)) M32(ARG(3)) = rt_module_count * PTR_SIZE;
  RET(1);
}
IMP(kernel32, K32GetModuleInformation) { // (process, module, MODULEINFO* {base, size, entry}, bytes)
  const ModuleDef *m = rt_module_of(ARG(1));
  if (!m || ARG(3) < 3u * PTR_SIZE) { SET_LAST_ERROR(87); RET(0); return; }
  u64 mi = ARG(2); MP(mi) = m->base; M32(mi + PTR_SIZE) = (u32)m->size; MP(mi + 2 * PTR_SIZE) = m->entry;
  RET(1);
}
