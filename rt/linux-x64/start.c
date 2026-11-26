// SPDX-License-Identifier: GPL-2.0-or-later
// The start of a Linux title: its segments are put in memory, its first stack is built (arguments, environment,
// the auxiliary vector musl reads to set up thread-local storage) and its entry point is called.
#include <unistd.h>
#include "linux.h"

int rt_trace;
int linux_tty; u16 linux_cols = 80, linux_rows = 24;
char linux_exe[300];
int linux_argc; char **linux_argv;
int linux_drive;
u64 linux_image_end;
// What a title finds in its environment: the same on every host (a title's output does not depend on who runs it).
// No SHELL: no shell started it. TERM only when its output is a terminal (SW_TTY): one that knows colours.
const char *linux_env[] = { "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/home/" LINUX_USER, "USER=" LINUX_USER, "LOGNAME=" LINUX_USER,
  "LANG=C.UTF-8", "PWD=/", 0, 0, 0 };

u64 linux_mem_used(void) { return (u64)__builtin_wasm_memory_size(0) * 65536; }

// A Linux title has no imports: an indirect call or jump goes to lifted code.
void rt_call(Cpu *c, u64 t) { rt_dispatch(c, t); }

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13,
       AT_EGID = 14, AT_PLATFORM = 15, AT_HWCAP = 16, AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25, AT_EXECFN = 31 };

static void load(const ModuleDef *m) {
  // room for the image: linear memory reaches past it, and malloc starts above it (as rt/process.c does for a
  // 64-bit Windows title), or the link left a gap for it below the runtime's data (tools/build-title.sh)
  extern char __global_base, __stack_high;
  u64 top = (m->base + m->size + 0xFFFF) & ~0xFFFFull, have = (u64)__builtin_wasm_memory_size(0) * 65536;
  if (top <= (u64)(uptr)&__global_base) {
    if (m->base < (u64)(uptr)&__stack_high) rt_fatal("the image at 0x%llx overlaps the runtime's stack (it ends at 0x%llx)", m->base, (u64)(uptr)&__stack_high);
  } else {
    if ((u64)(uptr)sbrk(0) > m->base) rt_fatal("the runtime's heap (0x%llx) already reaches the image base 0x%llx", (u64)(uptr)sbrk(0), m->base);
    if (top > have && __builtin_wasm_memory_grow(0, (top - have) / 65536) == (uptr)-1) rt_fatal("cannot grow memory to 0x%llx", top);
    sbrk((intptr_t)(top - (u64)(uptr)sbrk(0)));
  }
  for (unsigned i = 0; i < m->section_count; i++) {
    u64 va = m->sections[i].va, n = m->sections[i].len;
    if (n && host_read_file(m->name, (u32)strlen(m->name), m->sections[i].off, n, va) != n) rt_fatal("cannot read segment %u of %s", i, m->name);
  }
  if (M32(m->base) != 0x464C457F) rt_fatal("%s: the ELF header is not in the first segment", m->name);
  linux_image_end = top;
}

int main(int argc, char **argv) {
  const char *e = getenv("SW_TRACE"); rt_trace = e ? atoi(e) : 0;
  if ((e = getenv("SW_TTY")) && *e && *e != '0') { linux_tty = 1; unsigned w, h; if (sscanf(e, "%ux%u", &w, &h) == 2 && w && h) { linux_cols = (u16)w; linux_rows = (u16)h; } }
  if (linux_tty) { unsigned n = 0; while (linux_env[n]) n++; linux_env[n] = "TERM=xterm-256color"; linux_env[n + 1] = "COLORTERM=truecolor"; }
  const ModuleDef *m = &rt_modules[0];
  if (rt_module_count != 1) rt_fatal("a Linux title is one statically linked program");
  load(m);
  // where the title is: at the root, which is then its folder, or as a title image says (SW_EXE: the root is the image's drive)
  const char *base = strrchr(m->name, '/'); snprintf(linux_exe, sizeof linux_exe, "/%s", base ? base + 1 : m->name);
  if ((e = getenv("SW_EXE")) && e[0] == '/' && strlen(e) < sizeof linux_exe) { strcpy(linux_exe, e); linux_drive = 1; }
  linux_argc = argc; linux_argv = calloc((size_t)argc + 1, sizeof *linux_argv);
  linux_argv[0] = linux_exe; for (int i = 1; i < argc; i++) linux_argv[i] = argv[i];

  // the first stack, from the top: the strings, then (16-byte aligned) argc, argv, 0, envp, 0, the auxiliary vector
  static Cpu cpu; Cpu *c = &cpu;
  u64 size = m->stack_reserve, lo = rt_valloc(size, 65536); if (!lo) rt_fatal("no memory for the stack");
  u64 sp = lo + size;
  #define STR(s) (sp -= strlen(s) + 1, memcpy((void *)GP(sp), (s), strlen(s) + 1), sp)
  unsigned envc = 0; while (linux_env[envc]) envc++;
  u64 *args = calloc((size_t)argc + envc, 8), *envs = args + argc;
  for (int i = 0; i < argc; i++) args[i] = STR(linux_argv[i]);
  for (unsigned i = 0; i < envc; i++) envs[i] = STR(linux_env[i]);
  u64 platform = STR("x86_64"), execfn = STR(linux_exe);
  sp -= 16; u64 random = sp; { u64 r[2] = { rt_random64(), rt_random64() }; if (rt_clock_virtual()) r[0] = r[1] = 0x5049534350495343ull; memcpy((void *)GP(random), r, 16); }
  { Cpu id = {0}; id.r[RAX] = 1; rt_cpuid(&id); cpu.r[RDX] = 0; (void)id; }
  u64 aux[][2] = { { AT_PHDR, m->base + M64(m->base + 32) }, { AT_PHENT, M16(m->base + 54) }, { AT_PHNUM, M16(m->base + 56) }, { AT_PAGESZ, 4096 }, { AT_BASE, 0 },
    { AT_FLAGS, 0 }, { AT_ENTRY, m->entry }, { AT_UID, LINUX_UID }, { AT_EUID, LINUX_UID }, { AT_GID, LINUX_UID }, { AT_EGID, LINUX_UID }, { AT_PLATFORM, platform },
    { AT_HWCAP, 0x178BFBFF }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 }, { AT_RANDOM, random }, { AT_EXECFN, execfn }, { AT_NULL, 0 } };
  unsigned words = 1 + (unsigned)argc + 1 + envc + 1 + 2 * (sizeof aux / sizeof aux[0]);
  sp = (sp - 8ull * words) & ~15ull;
  u64 at = sp;
  #define WORD(v) (M64(at) = (v), at += 8)
  WORD((u64)argc); for (int i = 0; i < argc; i++) WORD(args[i]); WORD(0);
  for (unsigned i = 0; i < envc; i++) WORD(envs[i]); WORD(0);
  for (unsigned i = 0; i < sizeof aux / sizeof aux[0]; i++) { WORD(aux[i][0]); WORD(aux[i][1]); }
  free(args);
  R[RSP] = sp; cpu.mxcsr = 0x1F80; cpu.fcw = 0x037F;
  rt_call(c, m->entry);   // (it ends the process itself: exit_group)
  rt_exit_process((u32)R[RAX]);
  return 0;
}
