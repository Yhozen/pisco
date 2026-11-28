// SPDX-License-Identifier: GPL-2.0-or-later
// The process: image loading, the import table, guest calls, memory, threads and waits.
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <unistd.h>
#include "rt.h"

int rt_trace;
u64 rt_peb;
Thread *rt_main_thread;

// ---- guest calls ----
u64 rt_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
#if GUEST_BITS == 32
  u32 sp = (u32)R[RSP];
  R[RSP] = (sp - 16) & ~3u;
  M32(R[RSP]) = (u32)a0; M32(R[RSP] + 4) = (u32)a1; M32(R[RSP] + 8) = (u32)a2; M32(R[RSP] + 12) = (u32)a3;
  PUSH32(0); // return address: nothing returns to it, the lifted callee returns to us
  rt_call(c, (u32)fn);
  R[RSP] = sp; // whoever was to remove the arguments
  return (u32)R[RAX];
#else
  u64 sp = R[RSP];
  R[RSP] = (R[RSP] - 32) & ~15ull; // shadow space, 16-byte aligned before the call
  R[RCX] = a0; R[RDX] = a1; R[8] = a2; R[9] = a3;
  PUSH64(0); // return address: nothing returns to it, the lifted callee returns to us
  rt_call(c, fn);
  R[RSP] = sp;
  return R[RAX];
#endif
}

// ---- imports ----
typedef struct { const char *name; ImportFn fn; } ImportDef;
extern const ImportDef rt_import_table[]; extern const unsigned rt_import_table_count; // imports.gen.c
static ImportFn import_fn[8192];
static const char *import_name_of[8192];
static unsigned import_total; // the image's imports, then GetProcAddress results
// 32-bit guests: the bytes of arguments each import removes from the stack when it returns (stdcall), 0 for
// cdecl, -1 when nothing says (calling it stops the run: a wrong guess would corrupt the caller's stack).
// From imports.gen.c (tools/stdcall-sizes.sh), or the name's own decoration (`_Name@12`).
typedef struct { const char *name; int bytes; } StdcallDef;
extern const StdcallDef rt_stdcall_table[]; extern const unsigned rt_stdcall_table_count;
static short import_pop[8192];
// ALIAS(...) in the runtime's sources (imports.gen.c): an import by ordinal or by a name that is no C identifier
typedef struct { const char *name, *target; int bytes; } AliasDef;
extern const AliasDef rt_alias_table[]; extern const unsigned rt_alias_table_count;
static const AliasDef *alias_of(const char *full) {
  for (unsigned k = 0; k < rt_alias_table_count; k++) if (!strcmp(rt_alias_table[k].name, full)) return &rt_alias_table[k];
  return 0;
}
// a DLL that forwards its functions to another one's: wsock32's are ws2_32's, by name and by ordinal; the ordinals
// are the same but for three that the two number differently (wsock32's 10, 11, 12: inet_addr, inet_ntoa,
// ioctlsocket; ws2_32's 11, 12, 10)
static const char *forwarded(const char *full, char *buf, size_t cap) {
  if (strncmp(full, "wsock32.dll!", 12)) return full;
  const char *n = full + 12;
  snprintf(buf, cap, "ws2_32.dll!%s", !strcmp(n, "#10") ? "#11" : !strcmp(n, "#11") ? "#12" : !strcmp(n, "#12") ? "#10" : n);
  return buf;
}
static int stdcall_bytes(const char *full) {
  char fw[200]; full = forwarded(full, fw, sizeof fw);
  const AliasDef *al = alias_of(full);
  if (al) { if (al->bytes >= 0) return al->bytes; full = al->target; }
  for (unsigned k = 0; k < rt_stdcall_table_count; k++) if (!strcmp(rt_stdcall_table[k].name, full)) return rt_stdcall_table[k].bytes;
  const char *at = strrchr(full, '@');
  if (at && at[1] >= '0' && at[1] <= '9') return atoi(at + 1);
  return -1;
}

static ImportFn find_exact(const char *full) {
  for (unsigned k = 0; k < rt_import_table_count; k++) if (!strcmp(rt_import_table[k].name, full)) return rt_import_table[k].fn;
  return 0;
}
// "dll!name". API sets resolve to the DLL behind them; ucrtbase falls back to msvcrt's functions, and
// kernelbase / ntdll to kernel32's.
static ImportFn find_import(const char *full) {
  char fw[200]; full = forwarded(full, fw, sizeof fw);
  ImportFn f = find_exact(full);
  if (f) return f;
  const AliasDef *al = alias_of(full);   // by ordinal, or by a C++ name (ALIAS in the runtime's sources)
  if (al) return find_exact(al->target);
  const char *bang = strchr(full, '!');
  if (!bang) return 0;
  char alt[200];
  const char *name = bang + 1;
  int crt = !strncmp(full, "api-ms-win-crt-", 15) || !strncmp(full, "ucrtbase.dll!", 13) || !strncmp(full, "vcruntime140.dll!", 17) || !strncmp(full, "msvcrt.dll!", 11);
  int core = !strncmp(full, "api-ms-win-core-", 16) || !strncmp(full, "kernelbase.dll!", 15) || !strncmp(full, "ntdll.dll!", 10) || !strncmp(full, "ext-ms-win-", 11);
  if (crt) {
    snprintf(alt, sizeof alt, "ucrtbase.dll!%s", name); if ((f = find_exact(alt))) return f;
    snprintf(alt, sizeof alt, "msvcrt.dll!%s", name); if ((f = find_exact(alt))) return f;
  }
  if (core) { snprintf(alt, sizeof alt, "kernel32.dll!%s", name); if ((f = find_exact(alt))) return f; }
  // psapi.dll's functions are kernel32's, named K32...
  if (!strncmp(full, "psapi.dll!", 10)) { snprintf(alt, sizeof alt, "kernel32.dll!K32%s", name); if ((f = find_exact(alt))) return f; }
  // the console's DLLs: kernelx is kernel32 with additions, xmem's functions are kernelx's
  if (!strncmp(full, "xmem.dll!", 9)) { snprintf(alt, sizeof alt, "kernelx.dll!%s", name); if ((f = find_exact(alt))) return f; }
  if (!strncmp(full, "kernelx.dll!", 12) || !strncmp(full, "xmem.dll!", 9) || strstr(full, "legacy.dll!")) {
    snprintf(alt, sizeof alt, "kernel32.dll!%s", name); if ((f = find_exact(alt))) return f;
    snprintf(alt, sizeof alt, "advapi32.dll!%s", name); if ((f = find_exact(alt))) return f;
  }
  if (!strncmp(full, "ext-ms-win-rtcore-ntuser", 24) || !strncmp(full, "ext-ms-win-ntuser", 17) || !strncmp(full, "minuser", 7)) { snprintf(alt, sizeof alt, "user32.dll!%s", name); if ((f = find_exact(alt))) return f; }
  if (!strncmp(full, "advapi32legacy.dll!", 19) || !strncmp(full, "api-ms-win-security", 19)) { snprintf(alt, sizeof alt, "advapi32.dll!%s", name); if ((f = find_exact(alt))) return f; }
#if GUEST_BITS == 32
  // a DLL that exports its stdcall functions under their decorated names (fmod.dll: `_FSOUND_Init@12`)
  const char *at = strrchr(name, '@');
  if (name[0] == '_' && at && at[1] >= '0' && at[1] <= '9') { snprintf(alt, sizeof alt, "%.*s!%.*s", (int)(bang - full), full, (int)(at - name - 1), name + 1); if ((f = find_exact(alt))) return f; }
#endif
  return 0;
}
// An import nothing implements: a stub that logs its first call and returns what the DLL's functions
// usually return (E_NOTIMPL for the console's HRESULT APIs, 0 otherwise), as XCL does. SW_STRICT=1 stops instead.
static int strict = -1;
static u32 stub_value(const char *full) {
  if (strstr(full, "X::#")) return 0; // a D3D12.X extension slot no one models: 0, as XCL answers them
  static const char *hresult[] = {"xgameruntime.dll!", "d3d12_xs.dll!", "d3d12.dll!", "dxgi.dll!", "xaudio2_9.dll!", "d3d12_x.dll!", "xg_xs.dll!", "xg_x.dll!", "acphal.dll!", "xapu.dll!", "chad.dll!", "xcrdapi.dll!", "gameinput.dll!", "mfplat.dll!", "d3dcompiler_47.dll!", "xaudio2_9.dll!", "libhttpclient.dll!", "xcurl.dll!"};
  for (unsigned i = 0; i < sizeof hresult / sizeof hresult[0]; i++) if (!strncmp(full, hresult[i], strlen(hresult[i]))) return 0x80004001u;
  return 0;
}
static void canon(char *out, const char *dll, const char *name) {
  size_t n = 0;
  for (const char *p = dll; *p && n < 60; p++) out[n++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
  out[n] = 0;
  if (n < 4 || strcmp(out + n - 4, ".dll")) strcat(out, ".dll");
  strcat(out, "!"); strncat(out, name, 120);
}
static int dynamic_quiet;
static pthread_mutex_t import_lock = PTHREAD_MUTEX_INITIALIZER;   // the table grows when an app asks for a function by name
u64 rt_dynamic_import(const char *dll, const char *name) {
  char full[200]; canon(full, dll, name);
  ImportFn fn = find_import(full);
  if (!fn) { if (rt_trace && !dynamic_quiet) rt_log("GetProcAddress: %s not implemented", full); return 0; }
  pthread_mutex_lock(&import_lock);
  for (unsigned i = 0; i < import_total; i++) if (import_fn[i] == fn) { pthread_mutex_unlock(&import_lock); return import_base + 8ull * i; }
  unsigned i = import_total++;
  import_fn[i] = fn; import_name_of[i] = strdup(full); import_pop[i] = (short)stdcall_bytes(full);
  pthread_mutex_unlock(&import_lock);
  return import_base + 8ull * i;
}
// An address that calls nothing: a call through it is reported under `full` (a vtable slot no one
// implements, "d3d12.dll!ID3D12Device16::CreateStateObject") and returns stub_value's answer.
u64 rt_stub_import(const char *full) {
  pthread_mutex_lock(&import_lock);
  for (unsigned i = 0; i < import_total; i++) if (!import_fn[i] && import_name_of[i] && !strcmp(import_name_of[i], full)) { pthread_mutex_unlock(&import_lock); return import_base + 8ull * i; }
  unsigned i = import_total++;
  import_fn[i] = 0; import_name_of[i] = strdup(full); import_pop[i] = (short)stdcall_bytes(full);
  pthread_mutex_unlock(&import_lock);
  return import_base + 8ull * i;
}
// The same for whoever builds a vtable and gives a missing method a named stub: nothing to report (rt/windows-x86/com.c)
u64 rt_method_import(const char *dll, const char *name) { dynamic_quiet = 1; u64 a = rt_dynamic_import(dll, name); dynamic_quiet = 0; return a; }
// A COM method's argument bytes are known to whoever builds its vtable, not to an import library (rt/windows-x86/com.c)
void rt_import_set_pop(u64 addr, int bytes) {
  if (addr >= import_base && addr < import_base + 8ull * import_total) import_pop[(addr - import_base) / 8] = (short)bytes;
}
u64 rt_com_vtable(const char *dll, const char *prefix, const char *const *names, unsigned n) {
  void *vt = calloc(n, PTR_SIZE);
  for (unsigned i = 0; i < n; i++) {
    char m[160]; snprintf(m, sizeof m, "%s%s", prefix, names[i] ? names[i] : "");
    u64 a = names[i] ? rt_dynamic_import(dll, m) : 0;
    if (!a) { char full[200]; snprintf(full, sizeof full, "%s.dll!%s#%u", dll, prefix, i); if (names[i]) snprintf(full, sizeof full, "%s.dll!%s", dll, m); a = rt_stub_import(full); }
    MP((uptr)vt + PTR_SIZE * i) = a;
  }
  return (uptr)vt;
}
const char *rt_import_name(u64 a) {
  if (a < import_base || a >= import_base + 8ull * import_total) return 0;
  return import_name_of[(a - import_base) / 8];
}

// SW_TRACE_DLL=a,b: the imports from those DLLs (by file name without ".dll") are traced as Wine's +snoop does
static int snooped(u32 i) {
  static signed char *mark; static u32 marked; static int ready;
  if (!ready) {
    ready = 1; const char *e = getenv("SW_TRACE_DLL");
    if (e && *e) {
      marked = import_total; mark = calloc(marked ? marked : 1, 1);
      for (u32 k = 0; k < import_total; k++) {
        const char *n = import_name_of[k], *bang = strchr(n, '!'); if (!bang) continue;
        size_t len = (size_t)(bang - n); if (len > 4 && !strncasecmp(bang - 4, ".dll", 4)) len -= 4;
        for (const char *p = e; *p;) { const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p);
          if (l == len && !strncasecmp(p, n, l)) mark[k] = 1; p += l + (q != 0); }
      }
    }
  }
  return mark && i < marked && mark[i];
}
// SW_CALL_STATS=1: every import's calls counted; rt_call_stats_print writes those of some DLLs since it last did
static u32 *call_counts; // as many as import_fn has room for: dynamic imports (COM methods) are added as they are asked for
void rt_call_stats_print(const char *title, const char *dlls) { // dlls: "d3d12.dll,dxgi.dll"
  static u32 *last; if (!call_counts) return;
  if (!last) last = calloc(8192, 4);
  size_t cap = 4096, n = 0; char *out = malloc(cap); out[0] = 0;
  for (u32 i = 0; i < import_total; i++) {
    u32 k = call_counts[i] - last[i]; last[i] = call_counts[i];
    const char *name = import_name_of[i], *bang = strchr(name, '!');
    if (!k || !bang) continue;
    char dll[64]; snprintf(dll, sizeof dll, "%.*s", (int)(bang - name), name);
    if (!strstr(dlls, dll)) continue;
    size_t need = n + strlen(bang) + 24; if (need > cap) { cap = need * 2; out = realloc(out, cap); }
    n += (size_t)sprintf(out + n, "%s%s x%u", n ? ", " : "", bang + 1, k);
  }
  rt_log("%s: %s", app, n ? out : "(no calls)"); free(out);
}
void rt_call(Cpu *c, u64 t) {
  if (t >= import_base && t < import_base + 8ull * import_total) {
    u32 i = (u32)((t - import_base) / 8);
#if GUEST_BITS == 32
    // the return address, and what a stdcall function removes
    if (import_pop[i] < 0) rt_fatal("no argument size is known for %s (called from 0x%llx): the stack cannot be cleaned", import_name_of[i], (u64)MP(R[RSP]));
    u32 pop = 4 + (u32)import_pop[i];
#else
    u32 pop = 8; // the return address the caller pushed
#endif
    if (!import_fn[i]) {
      if (strict < 0) { const char *e = getenv("SW_STRICT"); strict = e && *e == '1'; }
      if (strict) rt_fatal("import not implemented: %s (called from 0x%llx)", import_name_of[i], (u64)MP(R[RSP]));
      static u8 told[8192];
      if (!told[i]) { told[i] = 1; rt_log("stub: %s (called from 0x%llx) returns 0x%x", import_name_of[i], (u64)MP(R[RSP]), stub_value(import_name_of[i])); }
      R[RAX] = stub_value(import_name_of[i]);
      R[RSP] += pop;
      return;
    }
    if (rt_trace > 1) rt_log("[%u] %s ret=%08llx", THREAD(c)->id, import_name_of[i], (u64)MP(R[RSP])); // as Wine's relay trace names the caller
    if (call_counts) __atomic_add_fetch(&call_counts[i], 1, __ATOMIC_RELAXED);
#if GUEST_BITS == 32
    if (snooped(i)) {   // SW_TRACE_DLL: the call as Wine's +snoop prints it once it returns (the stack's words, the result)
      u32 n = (u32)import_pop[i] / 4, w[16], at = (u32)R[RSP], caller = M32(at);
      for (u32 k = 0; k < n && k < 16; k++) w[k] = M32(at + 4 + 4 * k);
      import_fn[i](c);
      char line[400]; int len = snprintf(line, sizeof line, "RET  %s(", strchr(import_name_of[i], '!') ? strchr(import_name_of[i], '!') + 1 : import_name_of[i]);
      for (u32 k = 0; k < n && k < 16 && len < 300; k++) len += snprintf(line + len, sizeof line - len, "%s%08x", k ? "," : "", w[k]);
      rt_log("snoop: %s) retval=%08x ret=%08x", line, (u32)R[RAX], caller);
      R[RSP] += pop;
      return;
    }
#endif
    import_fn[i](c);
    R[RSP] += pop;
    return;
  }
  rt_dispatch(c, t);
}

// ---- handles and waits ----
pthread_mutex_t rt_wait_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t rt_wait_cond = PTHREAD_COND_INITIALIZER;
static Object *handles[65536];
Object *rt_object_new(int type) { Object *o = calloc(1, sizeof *o); o->type = type; o->refs = 0; o->fd = -1; return o; }
u64 rt_handle_new(Object *o) {
  pthread_mutex_lock(&rt_wait_lock);
  for (unsigned i = 1; i < 65536; i++) if (!handles[i]) { handles[i] = o; o->refs++; pthread_mutex_unlock(&rt_wait_lock); return 0x1000 + 4ull * i; }
  pthread_mutex_unlock(&rt_wait_lock);
  rt_fatal("out of handles");
}
Object *rt_handle_get(u64 h) {
  if (h < 0x1000 || ((h - 0x1000) & 3)) return 0;
  u64 i = (h - 0x1000) / 4;
  return i < 65536 ? handles[i] : 0;
}
void rt_handle_close(u64 h) {
  if (h < 0x1000) return;
  pthread_mutex_lock(&rt_wait_lock);
  u64 i = (h - 0x1000) / 4;
  if (i < 65536 && handles[i]) { Object *o = handles[i]; handles[i] = 0; if (--o->refs == 0 && o->type != OBJ_THREAD) { if (o->type == OBJ_FILE) { extern void rt_file_closed(Object *); rt_file_closed(o); } if (o->type == OBJ_MAPPING) rt_mapping_free(o); else free(o); } }
  pthread_mutex_unlock(&rt_wait_lock);
}
void rt_signal(void) { pthread_cond_broadcast(&rt_wait_cond); }

static int ready(Object *o, Thread *me) {
  switch (o->type) {
  case OBJ_EVENT: case OBJ_THREAD: case OBJ_PROCESS: return o->signaled;
  case OBJ_SEMAPHORE: return o->count > 0;
  case OBJ_MUTEX: return !o->owner || o->owner == me;
  default: return 1;
  }
}
static void take(Object *o, Thread *me) {
  if (o->type == OBJ_EVENT && !o->manual) o->signaled = 0;
  else if (o->type == OBJ_SEMAPHORE) o->count--;
  else if (o->type == OBJ_MUTEX) { o->owner = me; o->recursion++; }
}
// WAIT_OBJECT_0 + i, WAIT_TIMEOUT (0x102) or WAIT_FAILED
u32 rt_wait(Cpu *c, u32 n, const u64 *hs, int all, u32 ms) {
  Thread *me = THREAD(c);
  Object *os[64];
  if (n == 0 || n > 64) return 0xFFFFFFFF;
  for (u32 i = 0; i < n; i++) {
    u64 h = hs[i];
    os[i] = h == PSEUDO_THREAD ? me->handle : rt_handle_get(h);
    if (!os[i]) { SET_LAST_ERROR(6); return 0xFFFFFFFF; }
  }
  struct timespec until; int timed = ms != 0xFFFFFFFF;
  if (timed) { clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; } }
  pthread_mutex_lock(&rt_wait_lock);
  for (;;) {
    if (all) {
      u32 ok = 1; for (u32 i = 0; i < n; i++) ok &= ready(os[i], me);
      if (ok) { for (u32 i = 0; i < n; i++) take(os[i], me); pthread_mutex_unlock(&rt_wait_lock); return 0; }
    } else {
      for (u32 i = 0; i < n; i++) if (ready(os[i], me)) { take(os[i], me); pthread_mutex_unlock(&rt_wait_lock); return i; }
    }
    if (timed) {
      if (ms == 0 || pthread_cond_timedwait(&rt_wait_cond, &rt_wait_lock, &until) == ETIMEDOUT) {
        // a last look after the timeout
        int any = 0; for (u32 i = 0; i < n && !any; i++) any = ready(os[i], me);
        if (!any || all) { pthread_mutex_unlock(&rt_wait_lock); return 0x102; }
      }
    } else pthread_cond_wait(&rt_wait_cond, &rt_wait_lock);
  }
}

// ---- threads ----
static u32 next_tid = 1000;
static __thread Thread *current;

// implicit TLS: each module with a TLS directory has an index; a thread's TLS array holds a copy of each template
static u64 new_teb(Thread *t) {
  u64 teb = rt_valloc(TEB_SIZE, 4096);
#ifdef TEB_SEH
  MP(teb + TEB_SEH) = 0xFFFFFFFFu;
#endif
  MP(teb + TEB_SELF) = teb;
  MP(teb + TEB_STACK_BASE) = t->stack_hi;
  MP(teb + TEB_STACK_LIMIT) = t->stack_lo;
  MP(teb + TEB_PID) = 4;
  MP(teb + TEB_TID) = t->id;
  MP(teb + TEB_PEB) = rt_peb;
  u64 array = rt_valloc(64 * 8, 64);
  for (unsigned k = 0; k < rt_module_count; k++) {
    u64 dir = rt_modules[k].tls_dir;
    if (!dir) continue;
    // IMAGE_TLS_DIRECTORY: start, end, the index's address, callbacks (pointers), then the zero fill's size
    u64 start = MP(dir), end = MP(dir + PTR_SIZE), zero = M32(dir + 4 * PTR_SIZE);
    u64 block = rt_valloc(end - start + zero + 64, 64);
    memcpy((void *)GP(block), (void *)GP(start), end - start);
    MP(array + PTR_SIZE * M32(MP(dir + 2 * PTR_SIZE))) = block;
  }
  MP(teb + TEB_TLS_POINTER) = array;
  return teb;
}

Thread *rt_thread_new(u64 stack_size) {
  Thread *t = aligned_alloc(64, sizeof(Thread));
  memset(t, 0, sizeof *t);
  if (stack_size < (1 << 20)) stack_size = 1 << 20;
  t->stack_lo = rt_valloc(stack_size, 65536);
  t->stack_hi = t->stack_lo + stack_size;
  pthread_mutex_lock(&rt_wait_lock); t->id = next_tid += 4; pthread_mutex_unlock(&rt_wait_lock);
  t->teb = new_teb(t);
  t->cpu.gs_base = t->teb;
#if GUEST_BITS == 32
  t->cpu.fs_base = t->teb; // 32-bit code reaches the TEB through fs
  t->cpu.fcw = 0x027F;     // what Windows gives a 32-bit thread: 53-bit precision, round to nearest
#endif
  t->cpu.r[RSP] = t->stack_hi - 64;
  t->cpu.mxcsr = 0x1F80;
  t->handle = rt_object_new(OBJ_THREAD);
  t->handle->thread = t;
  return t;
}

// TLS callbacks and DllMain of every module (DLLs first, the exe last; in reverse on detach)
// An on-demand module (MODULE_ON_DEMAND) is attached by LoadLibrary, not at start, and detached when FreeLibrary
// takes its last reference; loaded again, its sections are read again from its file, as Windows maps a DLL afresh.
static unsigned module_refs[256];
static unsigned char module_was_loaded[256];
int rt_module_loaded(const ModuleDef *m) { unsigned k = (unsigned)(m - rt_modules); return !(m->flags & MODULE_ON_DEMAND) || (k < 256 && module_refs[k]); }
static void attach_module(Thread *t, const ModuleDef *m, int reason) {
  if (!rt_module_loaded(m)) return;
  if (m->tls_dir) for (u64 cbs = MP(m->tls_dir + 3 * PTR_SIZE); cbs && MP(cbs); cbs += PTR_SIZE) rt_guest_call(&t->cpu, MP(cbs), m->base, (u64)reason, 0, 0);
  if (m->is_dll && m->entry && m->entry != m->base) rt_guest_call(&t->cpu, m->entry, m->base, (u64)reason, 0, 0);
}
void rt_thread_attach(Thread *t, int reason) {
  if (reason == 0 || reason == 3) { for (unsigned k = 0; k < rt_module_count; k++) attach_module(t, &rt_modules[k], reason); return; }
  for (unsigned k = rt_module_count; k-- > 1;) attach_module(t, &rt_modules[k], reason);
  attach_module(t, &rt_modules[0], reason);
}

void rt_thread_exit(Cpu *c, u32 code) {
  Thread *t = THREAD(c);
  t->exit_code = code;
  if (!t->exit_jmp) rt_fatal("thread %u exits with no frame to unwind to", t->id);
  longjmp(*(jmp_buf *)t->exit_jmp, 1);
}

static void run_guest(Thread *t, u64 fn, u64 arg) {
  jmp_buf jb;
  t->exit_jmp = &jb;
  if (!setjmp(jb)) {
    rt_thread_attach(t, 2); // DLL_THREAD_ATTACH
    t->exit_code = (u32)rt_guest_call(&t->cpu, fn, arg, 0, 0, 0);
  }
  t->exit_jmp = 0;
}

typedef struct { Thread *t; Object *resume; } Start;
static void *thread_main(void *p) {
  Start *s = p; Thread *t = s->t;
  current = t;
  if (s->resume) { u64 h = rt_handle_new(s->resume); rt_wait(&t->cpu, 1, &h, 0, 0xFFFFFFFF); rt_handle_close(h); }
  free(s);
  if (t->native) { jmp_buf jb; t->exit_jmp = &jb; if (!setjmp(jb)) t->native(&t->cpu, t->native_arg); t->exit_jmp = 0; }
  else run_guest(t, t->start, t->param);
  pthread_mutex_lock(&rt_wait_lock);
  t->handle->signaled = 1;
  rt_signal();
  pthread_mutex_unlock(&rt_wait_lock);
  return 0;
}

// CreateThread / _beginthreadex. `suspended`: the caller resumes it with ResumeThread.
static Object *resume_events[65536];
u64 rt_create_thread(Cpu *c, u64 stack, u64 start, u64 param, int suspended, u32 *tid) {
  Thread *t = rt_thread_new(stack ? stack : image_stack_reserve);
  t->start = start; t->param = param;
  Start *s = calloc(1, sizeof *s); s->t = t;
  if (suspended) { s->resume = rt_object_new(OBJ_EVENT); s->resume->manual = 1; resume_events[t->id & 0xFFFF] = s->resume; }
  if (tid) *tid = t->id;
  u64 h = rt_handle_new(t->handle);
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
  pthread_t pt;
  if (pthread_create(&pt, &a, thread_main, s)) { rt_handle_close(h); return 0; }
  pthread_detach(pt);
  return h;
}
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg) {
  Thread *t = rt_thread_new(1 << 20);
  t->native = fn; t->native_arg = arg;
  Start *s = calloc(1, sizeof *s); s->t = t;
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
  pthread_t pt;
  if (pthread_create(&pt, &a, thread_main, s)) return 0;
  pthread_detach(pt);
  return t->id;
}
int rt_resume_thread(u64 h) {
  Object *o = rt_handle_get(h);
  if (!o || o->type != OBJ_THREAD) return -1;
  Object *e = resume_events[o->thread->id & 0xFFFF];
  if (!e) return 0;
  pthread_mutex_lock(&rt_wait_lock); e->signaled = 1; rt_signal(); pthread_mutex_unlock(&rt_wait_lock);
  resume_events[o->thread->id & 0xFFFF] = 0;
  return 1;
}

// ---- the process ----
const ModuleDef *rt_module_of(u64 a) {
  for (unsigned k = 0; k < rt_module_count; k++) if (a >= rt_modules[k].base && a < rt_modules[k].base + rt_modules[k].size) return &rt_modules[k];
  return 0;
}
const ModuleDef *rt_module_named(const char *name) {
  const char *slash = strrchr(name, '\\'); if (slash) name = slash + 1;
  slash = strrchr(name, '/'); if (slash) name = slash + 1;
  size_t n = strlen(name);
  for (unsigned k = 0; k < rt_module_count; k++) {
    const char *m = rt_modules[k].name, *ms = strrchr(m, '/'); if (ms) m = ms + 1;   // a DLL from C:\WINDOWS\system32: "c:/WINDOWS/system32/x.dll"
    size_t mn = strlen(m);
    if (!strcasecmp(m, name)) return &rt_modules[k];
    if (mn == n + 4 && !strncasecmp(m, name, n) && !strcasecmp(m + n, ".dll")) return &rt_modules[k];
  }
  return 0;
}
u64 rt_module_export(const ModuleDef *m, const char *name) {
  if (!m->exports) return 0;
  u64 d = m->exports, b = m->base;
  u32 nnames = M32(d + 24), funcs = M32(d + 28), names = M32(d + 32), ords = M32(d + 36), ord_base = M32(d + 16);
  u32 idx = 0xFFFFFFFF;
  if ((uptr)name < 0x10000) idx = (u32)(uptr)name - ord_base;
  else for (u32 i = 0; i < nnames; i++) if (!strcmp(GSTR(b + M32(b + names + 4ull * i)), name)) { idx = M16(b + ords + 2ull * i); break; }
  if (idx >= M32(d + 20)) return 0;
  return b + M32(b + funcs + 4ull * idx);
}

// Windows' loader fills in the stack-protection cookie an image names in its load configuration directory, so
// the image's own start-up code finds it set and does not make one from the clock, the thread id and the
// performance counter. We do the same, with a fixed value: runs stay the same from one to the next.
static void set_security_cookie(const ModuleDef *m) {
  u8 h[1024];
  if (host_read_file(m->name, (u32)strlen(m->name), 0, sizeof h, (u64)(uptr)h) != sizeof h) return;
  u32 pe; memcpy(&pe, h + 0x3C, 4);
  if (pe > sizeof h - 0x100 || memcmp(h + pe, "PE\0\0", 4)) return;
  u16 magic; memcpy(&magic, h + pe + 24, 2);
  if (magic != 0x10B) return; // PE32 only
  u32 rva, size; memcpy(&rva, h + pe + 24 + 96 + 8 * 10, 4); memcpy(&size, h + pe + 24 + 96 + 8 * 10 + 4, 4);
  if (!rva || size < 64) return;
  u32 cookie = M32(m->base + rva + 60); // IMAGE_LOAD_CONFIG_DIRECTORY32.SecurityCookie: the variable's address
  if (cookie >= m->base && cookie < m->base + m->size) M32(cookie) = 0x5EC0C1E5u;
}

// The image's headers at its base, as Windows maps them (code reads them: MSVC 8's C runtime looks at its own exe's
// sections before it starts floating point, _IsNonwritableInCurrentImage), with the base the image has here
static void map_headers(const ModuleDef *m) {
  u64 first = m->base + m->size;
  for (unsigned i = 0; i < m->section_count; i++) if (m->sections[i].va < first) first = m->sections[i].va;
  u64 n = first > m->base ? first - m->base : 0; if (n > 0x1000) n = 0x1000;
  if (n < 0x200 || host_read_file(m->name, (u32)strlen(m->name), 0, (u32)n, m->base) != n) return;
  u32 pe = M32(m->base + 0x3C);
  if (pe > n - 0x80 || M32(m->base + pe) != 0x4550) return;
  if (M16(m->base + pe + 24) == 0x10B) M32(m->base + pe + 24 + 28) = (u32)m->base; else M64(m->base + pe + 24 + 24) = m->base;   // ImageBase
}
static void map_module(unsigned k) {
  const ModuleDef *m = &rt_modules[k];
  map_headers(m);
  for (unsigned i = 0; i < m->section_count; i++) {
    u64 va = m->sections[i].va, n = m->sections[i].len;
    if (n && host_read_file(m->name, (u32)strlen(m->name), m->sections[i].off, n, va) != n) rt_fatal("cannot read section %u of %s", i, m->name);
    if (m->sections[i].vsize > n) memset((void *)GP(va + n), 0, m->sections[i].vsize - n);   // the zero fill (again, on a reload)
  }
  for (unsigned i = 0; i < m->reloc_count; i++) MP(m->relocs[i]) += m->base - m->file_base;
#if GUEST_BITS == 32
  set_security_cookie(m);
#endif
  if (m->tls_dir) M32(MP(m->tls_dir + 2 * PTR_SIZE)) = k; // the module's TLS index
}
u64 rt_module_load(Cpu *c, const ModuleDef *m) {
  unsigned k = (unsigned)(m - rt_modules);
  if (!(m->flags & MODULE_ON_DEMAND) || k >= 256) return m->base;
  if (module_refs[k]++) return m->base;
  if (module_was_loaded[k]) map_module(k);   // loaded before and freed since: its data as the file has it
  module_was_loaded[k] = 1;
  // the on-demand DLLs it imports first, as Windows' loader does
  for (unsigned i = m->first_import; i < m->first_import + m->import_count; i++) {
    char dll[64]; const char *bang = strchr(import_names[i], '!');
    size_t n = bang ? (size_t)(bang - import_names[i]) : 0; if (!n || n >= sizeof dll) continue;
    memcpy(dll, import_names[i], n); dll[n] = 0;
    const ModuleDef *d = rt_module_named(dll);
    if (d && d != m && (d->flags & MODULE_ON_DEMAND) && !module_refs[d - rt_modules]) rt_module_load(c, d);
  }
  if (rt_trace) rt_log("load %s at 0x%llx", m->name, m->base);
  attach_module(THREAD(c), m, 1);   // DLL_PROCESS_ATTACH
  return m->base;
}
int rt_module_free(Cpu *c, const ModuleDef *m) {
  unsigned k = (unsigned)(m - rt_modules);
  if (!(m->flags & MODULE_ON_DEMAND) || k >= 256 || !module_refs[k]) return 1;
  if (module_refs[k] == 1) { attach_module(THREAD(c), m, 0); if (rt_trace) rt_log("free %s", m->name); }   // DLL_PROCESS_DETACH
  module_refs[k]--;
  return 1;
}
static void load_modules(void) {
  for (unsigned k = 0; k < rt_module_count; k++) map_module(k);
  unsigned missing = 0;
  for (unsigned i = 0; i < import_count; i++) {
    import_name_of[i] = import_names[i];
    if (import_targets[i]) { MP(import_slots[i]) = import_targets[i]; continue; } // a DLL we lifted
    u64 data = rt_data_import(import_names[i]);
    if (data) { MP(import_slots[i]) = data; continue; }
    import_fn[i] = find_import(import_names[i]);
    import_pop[i] = (short)stdcall_bytes(import_names[i]);
    if (!import_fn[i]) missing++;
    MP(import_slots[i]) = import_base + 8ull * i;
  }
  import_total = import_count;
  { const char *e = getenv("SW_CALL_STATS"); if (e && *e == '1') call_counts = calloc(8192, 4); }
  if (missing && rt_trace) {
    rt_log("%u of %u imports are not implemented:", missing, import_count);
    for (unsigned i = 0; i < import_count; i++) if (!import_targets[i] && !import_fn[i] && !rt_data_import(import_names[i])) rt_log("  %s", import_names[i]);
  }
}

extern void rt_crt_init(void);
extern const char *rt_command_line;
int main(int argc, char **argv) {
  // the app's command line: the program name, then the arguments the host was given
  { size_t n = 16; for (int i = 1; i < argc; i++) n += strlen(argv[i]) + 3;
    char *cl = malloc(n + 300); snprintf(cl, 300, "\"C:\\title\\%s\"", rt_modules[0].name);
    for (int i = 1; i < argc; i++) { strcat(cl, strchr(argv[i], ' ') ? " \"" : " "); strcat(cl, argv[i]); if (strchr(argv[i], ' ')) strcat(cl, "\""); }
    rt_command_line = cl; }
  const char *tr = getenv("SW_TRACE");
  rt_trace = tr ? atoi(tr) : 0;
  // the image sits at a fixed guest address: make linear memory reach past it first
  u64 top = 0;
  for (unsigned k = 0; k < rt_module_count; k++) if (rt_modules[k].base + rt_modules[k].size > top) top = rt_modules[k].base + rt_modules[k].size;
  top = (top + 0xFFFF) & ~0xFFFFull;
  u64 have = (u64)__builtin_wasm_memory_size(0) * 65536;
  // Two layouts (tools/build-app.sh picks). An image that must load low (32-bit apps: 0x400000, often with
  // no relocations) sits in a gap the link left for it, between the runtime's stack and its data; nothing of
  // the runtime reaches it. Otherwise the images are above the runtime and its heap has to jump over them.
  extern char __global_base, __stack_high;
  u64 low = top;
  for (unsigned k = 0; k < rt_module_count; k++) if (rt_modules[k].base < low) low = rt_modules[k].base;
  if (top <= (u64)(uptr)&__global_base) {
    if (low < (u64)(uptr)&__stack_high) rt_fatal("the image at 0x%llx overlaps the runtime's stack (it ends at 0x%llx)", low, (u64)(uptr)&__stack_high);
  } else {
    if ((u64)(uptr)sbrk(0) > image_base) rt_fatal("the runtime's heap (0x%llx) already reaches the image base 0x%llx", (u64)(uptr)sbrk(0), image_base);
    if (top > have && __builtin_wasm_memory_grow(0, (top - have) / 65536) == (uptr)-1) rt_fatal("cannot grow memory to 0x%llx", top);
    // the image's range is taken out of malloc's reach: sbrk moves past it
    sbrk((intptr_t)(top - (u64)(uptr)sbrk(0)));
  }
  rt_crt_init(); // before the imports are bound: data imports point into it
  load_modules();
  rt_peb = rt_valloc(0x1000, 4096);
  MP(rt_peb + PEB_IMAGE_BASE) = image_base;
  Thread *t = rt_main_thread = rt_thread_new(image_stack_reserve);
  current = t;
  rt_thread_attach(t, 1); // DLL_PROCESS_ATTACH
  jmp_buf jb; t->exit_jmp = &jb;
  if (!setjmp(jb)) {
    Cpu *c = &t->cpu;
#if GUEST_BITS == 32
    PUSH32(0);
#else
    PUSH64(0);
#endif
    rt_call(c, rt_modules[0].entry);
    t->exit_code = (u32)R[RAX];
  }
  extern void rt_exit_process(u32 code);
  rt_exit_process(t->exit_code);
  return 0;
}
