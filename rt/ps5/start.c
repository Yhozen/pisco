// SPDX-License-Identifier: GPL-2.0-or-later
// The start of a PS5 process: the executable's segments are put in memory at the base the lifter gave it, its
// relocations are written (the values the lifter kept, rt_reloc_values) and its imports bound to the runtime's
// functions; then its entry point is called as the PS5's loader calls it: _start(process parameters, teardown).
// A guest thread (scePthreadCreate) is a thread of the runtime with a Cpu and a stack of its own.
#include "ps5.h"

int rt_trace;
extern const u64 rt_reloc_values[];

// ---- imports: every IMP of the base (library, name) (tools/gen-imports.sh), by "lib!name" ----
typedef struct { const char *name; ImportFn fn; } ImportDef;
extern const ImportDef rt_import_table[]; extern const unsigned rt_import_table_count;
static ImportFn *import_fn;
static ImportFn find_import(const char *full) {
  const char *bang = strchr(full, '!'); if (!bang) return 0;
  size_t lib = (size_t)(bang - full);
  for (unsigned k = 0; k < rt_import_table_count; k++) {
    const char *n = rt_import_table[k].name, *b = strchr(n, '!');   // "libkernel.dll!sceKernelUsleep"
    size_t l = (size_t)(b - n); if (l > 4 && !strncmp(b - 4, ".dll", 4)) l -= 4;
    if (l == lib && !strncasecmp(n, full, lib) && !strcmp(b + 1, bang + 1)) return rt_import_table[k].fn;
  }
  return 0;
}
static int strict = -1;
void rt_call(Cpu *c, u64 t) {
  if (t >= import_base && t < import_base + 8ull * import_count) {
    u32 i = (u32)((t - import_base) / 8);
    if (rt_trace > 1) rt_log("[%llx] %s ret=%llx", (u64)(uptr)c, import_names[i], (u64)M64(R[RSP]));
    if (import_fn[i]) import_fn[i](c);
    else {
      if (strict < 0) { const char *e = getenv("SW_STRICT"); strict = e && *e == '1'; }
      if (strict) rt_fatal("import not implemented: %s (called from 0x%llx)", import_names[i], (u64)M64(R[RSP]));
      static u8 told[4096];
      if (i < sizeof told && !told[i]) { told[i] = 1; rt_log("stub: %s (called from 0x%llx) returns 0", import_names[i], (u64)M64(R[RSP])); }
      R[RAX] = 0;
    }
    R[RSP] += 8;   // the return address the caller pushed
    return;
  }
  rt_dispatch(c, t);
}
u64 rt_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
  u64 save[6] = {R[RDI], R[RSI], R[RDX], R[RCX], R[RSP], 0};
  R[RDI] = a0; R[RSI] = a1; R[RDX] = a2; R[RCX] = a3;
  R[RSP] = (R[RSP] - 64) & ~15ull;   // (aligned as at a call: the return address makes it 8 mod 16)
  PUSH64(0);
  rt_call(c, fn);
  u64 r = R[RAX];
  R[RDI] = save[0]; R[RSI] = save[1]; R[RDX] = save[2]; R[RCX] = save[3]; R[RSP] = save[4];
  return r;
}
void ps5_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1) { rt_guest_call(c, fn, a0, a1, 0, 0); }

// ---- threads ----
#define THREAD_STACK (2u << 20)
static void cpu_reset(Cpu *c) { c->mxcsr = 0x1F80; c->fcw = 0x37F; }
static void *thread_main(void *p) {
  Ps5Thread *t = p; Cpu *c = &t->cpu;
  cpu_reset(c);
  R[RSP] = t->stack + THREAD_STACK - 8;   // as after a call: the entry finds rsp 8 mod 16
  R[RDI] = t->arg;
  M64(R[RSP]) = 0;
  rt_call(c, t->entry);
  t->result = R[RAX];
  return 0;
}
u64 ps5_thread_create(u64 entry, u64 arg, const char *name) {
  Ps5Thread *t = aligned_alloc(64, sizeof *t); memset(t, 0, sizeof *t);
  t->entry = entry; t->arg = arg; t->stack = rt_valloc(THREAD_STACK, 65536);
  if (name) snprintf(t->name, sizeof t->name, "%s", name);
  if (!t->stack) rt_fatal("no memory for a thread's stack");
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&t->pt, &a, thread_main, t)) { free(t); return 0; }
  if (rt_trace) rt_log("thread %s started (entry 0x%llx)", t->name, entry);
  return (u64)(uptr)t;
}
int ps5_thread_join(u64 handle, u64 *result) {
  Ps5Thread *t = (Ps5Thread *)GP(handle);
  if (!t) return -1;
  pthread_join(t->pt, 0);
  if (result) *result = t->result;
  return 0;
}
// a runtime thread that calls guest code (the mixer, mixer.c)
typedef struct { Cpu cpu; void (*fn)(Cpu *, void *); void *arg; } Native;
static void *native_main(void *p) { Native *n = p; cpu_reset(&n->cpu); n->cpu.r[RSP] = rt_valloc(1 << 20, 65536) + (1 << 20) - 64; n->fn(&n->cpu, n->arg); return 0; }
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg) {
  Native *n = aligned_alloc(64, sizeof *n); memset(n, 0, sizeof *n); n->fn = fn; n->arg = arg;
  pthread_t pt; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&pt, &a, native_main, n)) return 0;
  pthread_detach(pt);
  return 1;
}

// ---- the process ----
__attribute__((noreturn)) void ps5_exit(Cpu *c, int status) {
  (void)c;
  ps5_video_flush();
  rt_exit_process((u32)status);
  for (;;) {}
}

static void load(const ModuleDef *m) {
  extern char __global_base, __stack_high;
  u64 top = (m->base + m->size + 0xFFFF) & ~0xFFFFull, have = (u64)__builtin_wasm_memory_size(0) * 65536;
  if (top <= (u64)(uptr)&__global_base) {
    if (m->base < (u64)(uptr)&__stack_high) rt_fatal("the image at 0x%llx overlaps the runtime's stack", m->base);
  } else {
    if ((u64)(uptr)sbrk(0) > m->base) rt_fatal("the runtime's heap (0x%llx) already reaches the image base 0x%llx", (u64)(uptr)sbrk(0), m->base);
    if (top > have && __builtin_wasm_memory_grow(0, (top - have) / 65536) == (uptr)-1) rt_fatal("cannot grow memory to 0x%llx", top);
    sbrk((intptr_t)(top - (u64)(uptr)sbrk(0)));
  }
  for (unsigned i = 0; i < m->section_count; i++) {
    u64 va = m->sections[i].va, n = m->sections[i].len;
    if (n && host_read_file(m->name, (u32)strlen(m->name), m->sections[i].off, n, va) != n) rt_fatal("cannot read segment %u of %s", i, m->name);
  }
  for (unsigned i = 0; i < m->reloc_count; i++) M64(m->relocs[i]) = rt_reloc_values[i];
}

static Ps5Thread main_thread;
int main(int argc, char **argv) {
  const char *e = getenv("SW_TRACE"); rt_trace = e ? atoi(e) : 0;
  if (rt_module_count != 1) rt_fatal("a PS5 app is one executable");
  const ModuleDef *m = &rt_modules[0];
  load(m);
  import_fn = calloc(import_count ? import_count : 1, sizeof *import_fn);
  unsigned missing = 0;
  for (unsigned i = 0; i < import_count; i++) {
    import_fn[i] = find_import(import_names[i]);
    if (!import_fn[i]) missing++;
    M64(import_slots[i]) = import_base + 8ull * i;
  }
  if (missing && rt_trace) { rt_log("%u of %u imports are not implemented:", missing, import_count); for (unsigned i = 0; i < import_count; i++) if (!import_fn[i]) rt_log("  %s", import_names[i]); }
  // the process parameters _start reads: argc (8 bytes), then argv; argv[0] is the executable as the PS5 names it
  u64 params = rt_valloc(4096, 4096);
  u64 strs = params + 1024, at = strs;
  const char *argv0 = "/app0/eboot.bin";
  M32(params) = (u32)argc;
  for (int i = 0; i < argc; i++) {
    const char *s = i == 0 ? argv0 : argv[i]; size_t n = strlen(s) + 1;
    if (at + n > params + 4096) break;
    memcpy((void *)GP(at), s, n); M64(params + 8 + 8ull * i) = at; at += n;
  }
  M64(params + 8 + 8ull * argc) = 0;
  Cpu *c = &main_thread.cpu;
  cpu_reset(c);
  u64 stack = rt_valloc(m->stack_reserve * 8, 65536); if (!stack) rt_fatal("no memory for the stack");
  R[RSP] = stack + m->stack_reserve * 8 - 8;
  M64(R[RSP]) = 0;
  R[RDI] = params; R[RSI] = 0;
  rt_call(c, m->entry);
  ps5_exit(c, (int)R[RAX]);
}
