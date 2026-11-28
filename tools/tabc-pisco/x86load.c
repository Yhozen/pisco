// SPDX-License-Identifier: GPL-2.0-or-later
// x86load: the binfmt_misc interpreter of x86-64 ELF files on unkernel's (aarch64) Linux, with PISCO as the x86-64
// CPU. An aarch64 program, freestanding, that does what Linux's ELF loader does for an x86-64 program: its segments
// mapped from the file (MAP_PRIVATE: the page cache's pages, faulted in as they are touched), the interpreter's too
// when it has one (PT_INTERP: musl's loader), a stack with argc, argv, envp and the auxiliary vector. Then it asks
// the patched kw_run to go on as x86-64: `svc` with x8 = 0x7ff0, the entry in x0, the stack in x1.
// Registered with the P flag: argv is [x86load, the file's path, the program's argv[0], its arguments...].
// Addresses as x86-64 Linux gives them without randomisation: a position-independent program at 0x555555554000,
// its interpreter at 0x7ffff7fc0000 (so a program's image, and its lifted module, is the same on every run).
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef long s64;

static s64 sc6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
  register s64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}
#define sc(n, a, b, c) sc6(n, (s64)(a), (s64)(b), (s64)(c), 0, 0, 0)
static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void die(const char *m, const char *p) {
  sc(64, 2, "x86load: ", 9); if (p) { sc(64, 2, p, slen(p)); sc(64, 2, ": ", 2); } sc(64, 2, m, slen(m)); sc(64, 2, "\n", 1);
  sc(94, 127, 0, 0); for (;;) ;
}
static void zero(void *d, u64 n) { u8 *a = d; while (n--) *a++ = 0; }
static void cpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }

enum { PIE_BASE = 0x555555554000ul, INTERP_BASE = 0x7ffff7fc0000ul };
typedef struct { u64 entry, phdr, phnum, base, brk; u8 ph[56 * 32]; } Image;

// maps the file's PT_LOAD segments at bias (0 for a program that is not position-independent)
static void load(const char *path, Image *im, u64 pie_base, char *interp) {
  s64 fd = sc6(56, -100, (s64)path, 0, 0, 0, 0);   // openat
  if (fd < 0) die("cannot open it", path);
  u8 eh[64]; if (sc6(67, fd, (s64)eh, 64, 0, 0, 0) != 64) die("cannot read its ELF header", path);
  if (eh[0] != 0x7f || eh[1] != 'E' || eh[4] != 2 || *(u16 *)(eh + 18) != 62) die("not an x86-64 ELF file", path);
  u16 type = *(u16 *)(eh + 16); u64 bias = type == 3 ? pie_base : 0;
  if (type != 2 && type != 3) die("not a program", path);
  u64 phoff = *(u64 *)(eh + 32); u16 phnum = *(u16 *)(eh + 56);
  if (phnum > 32 || sc6(67, fd, (s64)im->ph, 56 * phnum, (s64)phoff, 0, 0) != 56 * phnum) die("cannot read its program headers", path);
  im->entry = *(u64 *)(eh + 24) + bias; im->phnum = phnum; im->phdr = 0; im->base = bias; im->brk = 0;
  u64 first = ~0ul;
  for (u32 i = 0; i < phnum; i++) {
    u8 *p = im->ph + 56 * i; u32 t = *(u32 *)p; u64 off = *(u64 *)(p + 8), va = *(u64 *)(p + 16) + bias, fs = *(u64 *)(p + 32), ms = *(u64 *)(p + 40);
    if (t == 6) im->phdr = va;
    if (t == 3 && interp) { if (fs > 255) die("its interpreter's name is too long", path); if (sc6(67, fd, (s64)interp, (s64)fs, (s64)off, 0, 0) != (s64)fs) die("cannot read its interpreter's name", path); interp[fs] = 0; }
    if (t != 1 || !ms) continue;
    if (first == ~0ul) first = va - off;
    u64 lo = va & ~4095ul, flo = off & ~4095ul, fend = (va + fs + 4095) & ~4095ul, hi = (va + ms + 4095) & ~4095ul;
    // the file's part (read and written as the program wants: lifted code does not care for PROT_EXEC), then zeros
    if (fs) { s64 m = sc6(222, (s64)lo, (s64)(fend - lo), 3, 0x12, fd, (s64)flo); if (m != (s64)lo) die("cannot map a segment", path); }   // MAP_PRIVATE|MAP_FIXED
    if (ms > fs) {
      u64 z = va + fs; if (fs && (z & 4095)) zero((void *)z, fend - z);   // the rest of the file's last page
      u64 zlo = fs ? fend : lo;
      if (hi > zlo) { s64 m = sc6(222, (s64)zlo, (s64)(hi - zlo), 3, 0x32, -1, 0); if (m != (s64)zlo) die("cannot map its zeroed memory", path); }   // anonymous
    }
    if (hi > im->brk) im->brk = hi;
  }
  if (!im->phdr && first != ~0ul) im->phdr = first + phoff;
  sc(57, fd, 0, 0);
}

void cmain(u64 *sp) {
  u64 argc = sp[0]; char **argv = (char **)(sp + 1); char **envp = argv + argc + 1;
  if (argc < 3) die("usage (binfmt_misc, flag P): x86load <program> <argv[0]> [args...]", 0);
  const char *path = argv[1];
  static Image prog, in; static char interp[256];
  interp[0] = 0;
  load(path, &prog, PIE_BASE, interp);
  u64 entry = prog.entry, ibase = 0;
  if (interp[0]) { load(interp, &in, INTERP_BASE, 0); entry = in.entry; ibase = in.base; }
  // the x86-64 stack, below this one (its strings stay where the kernel put them)
  u64 at_random = 0, uid = 0, euid = 0, gid = 0, egid = 0;
  { u64 *a = (u64 *)envp; while (*a) a++; for (a++; a[0]; a += 2) { if (a[0] == 25) at_random = a[1]; if (a[0] == 11) uid = a[1]; if (a[0] == 12) euid = a[1]; if (a[0] == 13) gid = a[1]; if (a[0] == 14) egid = a[1]; } }
  u64 nenv = 0; while (envp[nenv]) nenv++;
  u64 nargs = argc - 2;
  u64 aux[] = { 3, prog.phdr, 4, 56, 5, prog.phnum, 6, 4096, 7, ibase, 8, 0, 9, prog.entry, 11, uid, 12, euid, 13, gid, 14, egid, 17, 100, 23, 0, 25, at_random, 31, (u64)path, 0, 0 };
  u64 words = 1 + nargs + 1 + nenv + 1 + sizeof aux / 8;
  u64 *nsp = (u64 *)(((u64)sp - 4096 - words * 8) & ~15ul);
  u64 *w = nsp; *w++ = nargs;
  for (u64 i = 0; i < nargs; i++) *w++ = (u64)argv[2 + i];
  *w++ = 0;
  for (u64 i = 0; i < nenv; i++) *w++ = (u64)envp[i];
  *w++ = 0;
  cpy(w, aux, sizeof aux);
  register u64 x8 __asm__("x8") = 0x7ff0, x0 __asm__("x0") = entry, x1 __asm__("x1") = (u64)nsp;
  __asm__ volatile("svc #0" : : "r"(x8), "r"(x0), "r"(x1) : "memory");
  die("the kernel did not take the x86-64 program (is the kernel patched and PISCO loaded?)", path);
}
__asm__(".globl _start\n_start:\n mov x0, sp\n bl cmain\n");
