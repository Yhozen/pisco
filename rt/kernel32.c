// kernel32.dll (and the ntdll functions it forwards) for lifted titles.
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"

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
IMP(kernel32, GetProcessAffinityMask) { u64 m = (1ull << sysconf(_SC_NPROCESSORS_ONLN)) - 1; if (ARG(1)) M64(ARG(1)) = m; if (ARG(2)) M64(ARG(2)) = m; RET(1); }
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
  if (!ms) { sched_yield(); return; }
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
  nanosleep(&ts, 0);
}
IMP(kernel32, SleepEx) { imp_kernel32_Sleep(c); RET(0); }
IMP(kernel32, GetSystemInfo) {
  u64 p = ARG(0); memset((void *)GP(p), 0, 48);
  M16(p) = 9; M32(p + 4) = 4096; M64(p + 8) = 0x10000; M64(p + 16) = 0x7FFFFFFEFFFF;
  M64(p + 24) = (1ull << sysconf(_SC_NPROCESSORS_ONLN)) - 1; M32(p + 32) = (u32)sysconf(_SC_NPROCESSORS_ONLN);
  M32(p + 36) = 8664; M32(p + 40) = 65536; M16(p + 44) = 6;
}
// PF_* numbers: 3 MMX, 6 XMMI, 10 XMMI64, 13 SSE3, 36 SSSE3, 37 SSE4_1, 38 SSE4_2, 39 AVX
IMP(kernel32, IsProcessorFeaturePresent) { u32 f = (u32)ARG(0); RET(f == 3 || f == 6 || f == 10 || f == 13 || f == 36 || f == 37 || f == 38 || f == 8 || f == 12); }

// ---- errors ----
IMP(kernel32, GetLastError) { RET(M32(THREAD(c)->teb + TEB_LAST_ERROR)); }
IMP(kernel32, SetLastError) { SET_LAST_ERROR((u32)ARG(0)); }
IMP(kernel32, FormatMessageA) { SET_LAST_ERROR(317); RET(0); }
IMP(kernel32, FormatMessageW) { SET_LAST_ERROR(317); RET(0); }
IMP(kernel32, SetUnhandledExceptionFilter) { RET(0); }
IMP(kernel32, OutputDebugStringA) { rt_log("debug: %s", GSTR(ARG(0))); }

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
  if (ARG(3)) M64(ARG(3)) = rt_handle_new(o);
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
  for (u32 i = 0; i < n && i < 64; i++) hs[i] = M64(ARG(1) + 8 * i);
  RET(rt_wait(c, n, hs, ARG(2) != 0, (u32)ARG(3)));
}

// ---- critical sections: a recursive pthread mutex behind the guest's 40-byte structure ----
static pthread_mutex_t cs_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_mutex(u64 cs) {
  pthread_mutex_t *m = (pthread_mutex_t *)GP(__atomic_load_n((u64 *)GP(cs), __ATOMIC_ACQUIRE));
  if (m) return m;
  pthread_mutex_lock(&cs_init_lock);
  m = (pthread_mutex_t *)GP(M64(cs));
  if (!m) {
    m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    __atomic_store_n((u64 *)GP(cs), (u64)(uptr)m, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&cs_init_lock);
  return m;
}
IMP(kernel32, InitializeCriticalSection) { memset((void *)GP(ARG(0)), 0, 40); cs_mutex(ARG(0)); }
IMP(kernel32, InitializeCriticalSectionAndSpinCount) { memset((void *)GP(ARG(0)), 0, 40); cs_mutex(ARG(0)); RET(1); }
IMP(kernel32, InitializeCriticalSectionEx) { memset((void *)GP(ARG(0)), 0, 40); cs_mutex(ARG(0)); RET(1); }
IMP(kernel32, DeleteCriticalSection) {
  pthread_mutex_t *m = (pthread_mutex_t *)GP(M64(ARG(0)));
  if (m) { pthread_mutex_destroy(m); free(m); M64(ARG(0)) = 0; }
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
IMP(kernel32, VirtualQuery) {
  u64 a = ARG(0), out = ARG(1);
  memset((void *)GP(out), 0, 48);
  pthread_mutex_lock(&region_lock);
  for (Region *r = regions; r; r = r->next) if (a >= r->base && a < r->base + r->size) {
    M64(out) = a & ~4095ull; M64(out + 8) = r->base; M32(out + 16) = r->protect; M64(out + 24) = r->base + r->size - (a & ~4095ull);
    M32(out + 32) = 0x1000; M32(out + 36) = r->protect; M32(out + 40) = 0x20000;
    pthread_mutex_unlock(&region_lock); RET(48); return;
  }
  pthread_mutex_unlock(&region_lock);
  M64(out) = a & ~4095ull; M64(out + 8) = a & ~4095ull; M64(out + 24) = 4096; M32(out + 32) = 0x1000; M32(out + 36) = 4; M32(out + 40) = 0x20000;
  RET(48);
}
IMP(kernel32, GetProcessHeap) { RET(0x7000); }
IMP(kernel32, HeapAlloc) { u64 n = ARG(2); void *p = (ARG(1) & 8) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); RET((uptr)p); }
IMP(kernel32, HeapFree) { free((void *)GP(ARG(2))); RET(1); }
IMP(kernel32, HeapReAlloc) { RET((uptr)realloc((void *)GP(ARG(2)), ARG(3))); }
IMP(kernel32, HeapSize) { RET(malloc_usable_size((void *)GP(ARG(2)))); }
IMP(kernel32, LocalAlloc) { RET((uptr)((ARG(0) & 0x40) ? calloc(1, ARG(1) ? ARG(1) : 1) : malloc(ARG(1) ? ARG(1) : 1))); }
IMP(kernel32, LocalFree) { free((void *)GP(ARG(0))); RET(0); }

// file mappings without files: shared memory inside the process
IMP(kernel32, CreateFileMappingA) {
  if (ARG(0) != 0xFFFFFFFFFFFFFFFFull) { SET_LAST_ERROR(50); RET(0); return; }
  int ex; Object *o = named(OBJ_MAPPING, ARG(5) ? GSTR(ARG(5)) : 0, &ex);
  if (!ex) { o->size = (ARG(3) << 32) | (u32)ARG(4); o->view = rt_valloc(o->size, 65536); }
  SET_LAST_ERROR(ex ? 183 : 0);
  RET(o->view ? rt_handle_new(o) : 0);
}
IMP(kernel32, OpenFileMappingA) {
  int ex; Object *o = named(OBJ_MAPPING, GSTR(ARG(2)), &ex);
  if (!ex) { SET_LAST_ERROR(2); RET(0); return; }
  RET(rt_handle_new(o));
}
IMP(kernel32, MapViewOfFile) { Object *o = rt_handle_get(ARG(0)); RET(o && o->type == OBJ_MAPPING ? o->view + ((ARG(2) << 32) | (u32)ARG(3)) : 0); }
IMP(kernel32, UnmapViewOfFile) { RET(1); }

// ---- time ----
static u64 filetime_now(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (u64)t.tv_sec * 10000000 + (u64)t.tv_nsec / 100 + 116444736000000000ull; }
IMP(kernel32, QueryPerformanceCounter) { M64(ARG(0)) = (u64)host_now_us(); RET(1); }
IMP(kernel32, QueryPerformanceFrequency) { M64(ARG(0)) = 1000000; RET(1); }
IMP(kernel32, GetTickCount) { RET((u32)(host_now_us() / 1000)); }
IMP(kernel32, GetTickCount64) { RET((u64)(host_now_us() / 1000)); }
IMP(kernel32, GetSystemTimeAsFileTime) { M64(ARG(0)) = filetime_now(); }
IMP(kernel32, GetSystemTimePreciseAsFileTime) { M64(ARG(0)) = filetime_now(); }
IMP(kernel32, GetSystemTimeAdjustment) { M32(ARG(0)) = 156250; M32(ARG(1)) = 156250; M32(ARG(2)) = 1; RET(1); }
IMP(kernel32, SetSystemTime) { SET_LAST_ERROR(1314); RET(0); }
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
  // API sets and console variants: the DLL behind them
  {"ntuser", 0x7FF70000ull}, {"minuser", 0x7FF70000ull}, {"advapi32legacy", 0x7FF80000ull}, {"api-ms-win-core", 0x7FF00000ull},
};
static u64 module_handle(const char *n) {
  const ModuleDef *m = rt_module_named(n);
  if (m) return m->base;
  for (unsigned i = 0; i < sizeof system_modules / sizeof system_modules[0]; i++) if (strcasestr(n, system_modules[i].name)) return system_modules[i].h;
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
  M64(ARG(2)) = h; if (!h) SET_LAST_ERROR(126); RET(h != 0);
}
static const char *module_path_of(u64 h) {
  static char buf[16][300]; static int k;
  const ModuleDef *m = h ? rt_module_of(h) : &rt_modules[0];
  if (!m) return 0;
  char *b = buf[k++ & 15]; snprintf(b, 300, "C:\\title\\%s", m->name);
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
  u64 h = module_handle(n);
  if (!h) SET_LAST_ERROR(126);
  if (rt_trace) rt_log("LoadLibrary %s -> 0x%llx", n, h);
  RET(h);
}
IMP(kernel32, LoadLibraryA) { loaded(c, GSTR(ARG(0))); }
IMP(kernel32, LoadLibraryExA) { loaded(c, GSTR(ARG(0))); }
IMP(kernel32, LoadLibraryW) { char *n = rt_narrow(GWSTR(ARG(0))); loaded(c, n); free(n); }
IMP(kernel32, LoadLibraryExW) { char *n = rt_narrow(GWSTR(ARG(0))); loaded(c, n); free(n); }
IMP(kernel32, FreeLibrary) { RET(1); }
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
IMP(kernel32, GetACP) { RET(65001); }
IMP(kernel32, GetOEMCP) { RET(437); }
IMP(kernel32, GetCPInfo) { u64 p = ARG(1); memset((void *)GP(p), 0, 20); M32(p) = 1; M8(p + 4) = '?'; RET(1); }
IMP(kernel32, SetConsoleCP) { RET(1); }
IMP(kernel32, SetConsoleOutputCP) { RET(1); }
IMP(kernel32, GetConsoleMode) { RET(0); }
// UTF-8 or ASCII <-> UTF-16
IMP(kernel32, MultiByteToWideChar) {
  const u8 *s = (const u8 *)GP(ARG(2)); s32 n = (s32)ARG(3); wchar16 *out = GWSTR(ARG(4)); s32 cap = (s32)ARG(5);
  if (n < 0) n = (s32)strlen((const char *)s) + 1;
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

// ---- slim reader/writer locks and condition variables: runtime objects behind the guest's 8 bytes ----
typedef struct { pthread_mutex_t m; pthread_cond_t cv; int readers, writer; } Srw;
typedef struct { pthread_mutex_t m; pthread_cond_t cv; u64 seq; } Cond;
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;
static void *lazy(u64 at, size_t size) {
  void *p = (void *)GP(__atomic_load_n((u64 *)GP(at), __ATOMIC_ACQUIRE));
  if (p) return p;
  pthread_mutex_lock(&lazy_lock);
  p = (void *)GP(M64(at));
  if (!p) { p = calloc(1, size); pthread_mutex_init((pthread_mutex_t *)p, 0); pthread_cond_init((pthread_cond_t *)((char *)p + sizeof(pthread_mutex_t)), 0); __atomic_store_n((u64 *)GP(at), (u64)(uptr)p, __ATOMIC_RELEASE); }
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
IMP(kernel32, InitializeSRWLock) { M64(ARG(0)) = 0; }
IMP(kernel32, AcquireSRWLockExclusive) { srw_lock(ARG(0), 1); }
IMP(kernel32, AcquireSRWLockShared) { srw_lock(ARG(0), 0); }
IMP(kernel32, TryAcquireSRWLockExclusive) { RET(srw_try(ARG(0), 1)); }
IMP(kernel32, TryAcquireSRWLockShared) { RET(srw_try(ARG(0), 0)); }
IMP(kernel32, ReleaseSRWLockExclusive) { srw_unlock(ARG(0), 1); }
IMP(kernel32, ReleaseSRWLockShared) { srw_unlock(ARG(0), 0); }
IMP(kernel32, InitializeConditionVariable) { M64(ARG(0)) = 0; }
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
