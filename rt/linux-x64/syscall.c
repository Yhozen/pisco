// SPDX-License-Identifier: GPL-2.0-or-later
// The `syscall` instruction: the number in RAX, the arguments in RDI, RSI, RDX, R10, R8 and R9, the result (or a
// negated errno) back in RAX. A process has one thread: clone for a thread says no. fork, execve, wait4 and pipes
// are the host's (process.c, fd.c). A call the runtime does not know says ENOSYS and is
// logged once; with SW_STRICT=1 it stops the title. SW_TRACE=1 logs every call.
#include <unistd.h>
#include "linux.h"

static const struct { u16 nr; const char *name; } names[] = {
  {0,"read"},{1,"write"},{2,"open"},{3,"close"},{4,"stat"},{5,"fstat"},{6,"lstat"},{7,"poll"},{8,"lseek"},{9,"mmap"},{10,"mprotect"},{11,"munmap"},{12,"brk"},
  {13,"rt_sigaction"},{14,"rt_sigprocmask"},{15,"rt_sigreturn"},{16,"ioctl"},{17,"pread64"},{18,"pwrite64"},{19,"readv"},{20,"writev"},{21,"access"},{22,"pipe"},
  {23,"select"},{24,"sched_yield"},{25,"mremap"},{26,"msync"},{27,"mincore"},{28,"madvise"},{32,"dup"},{33,"dup2"},{34,"pause"},{35,"nanosleep"},{39,"getpid"},
  {41,"socket"},{42,"connect"},{43,"accept"},{44,"sendto"},{45,"recvfrom"},{46,"sendmsg"},{47,"recvmsg"},{48,"shutdown"},{49,"bind"},{50,"listen"},{51,"getsockname"},{52,"getpeername"},{53,"socketpair"},{54,"setsockopt"},{55,"getsockopt"},{56,"clone"},{57,"fork"},{58,"vfork"},{59,"execve"},{60,"exit"},{61,"wait4"},{62,"kill"},{63,"uname"},{72,"fcntl"},{73,"flock"},
  {74,"fsync"},{76,"truncate"},{77,"ftruncate"},{78,"getdents"},{79,"getcwd"},{80,"chdir"},{82,"rename"},{83,"mkdir"},{84,"rmdir"},{86,"link"},{87,"unlink"},
  {88,"symlink"},{89,"readlink"},{90,"chmod"},{92,"chown"},{95,"umask"},{96,"gettimeofday"},{97,"getrlimit"},{98,"getrusage"},{99,"sysinfo"},{100,"times"},
  {102,"getuid"},{104,"getgid"},{107,"geteuid"},{108,"getegid"},{109,"setpgid"},{110,"getppid"},{111,"getpgrp"},{112,"setsid"},{105,"setuid"},{106,"setgid"},{113,"setreuid"},{114,"setregid"},{115,"getgroups"},{116,"setgroups"},{117,"setresuid"},{119,"setresgid"},{122,"setfsuid"},{123,"setfsgid"},{118,"getresuid"},
  {120,"getresgid"},{121,"getpgid"},{124,"getsid"},{131,"sigaltstack"},{137,"statfs"},{138,"fstatfs"},{157,"prctl"},{158,"arch_prctl"},{186,"gettid"},{200,"tkill"},
  {201,"time"},{202,"futex"},{203,"sched_setaffinity"},{204,"sched_getaffinity"},{217,"getdents64"},{218,"set_tid_address"},{228,"clock_gettime"},
  {229,"clock_getres"},{230,"clock_nanosleep"},{231,"exit_group"},{234,"tgkill"},{257,"openat"},{258,"mkdirat"},{262,"newfstatat"},{263,"unlinkat"},
  {267,"readlinkat"},{269,"faccessat"},{270,"pselect6"},{271,"ppoll"},{273,"set_robust_list"},{281,"epoll_pwait"},{290,"eventfd2"},{291,"epoll_create1"},
  {292,"dup3"},{293,"pipe2"},{302,"prlimit64"},{309,"getcpu"},{318,"getrandom"},{319,"memfd_create"},{332,"statx"},{334,"rseq"},{435,"clone3"},{439,"faccessat2"},
};
static const char *name_of(u64 nr) { for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (names[i].nr == nr) return names[i].name; return "?"; }

static void sleep_us(u64 us) { if (rt_clock_virtual()) rt_clock_advance(us); else usleep((useconds_t)us); }
static void put_time(u64 ts, u64 us) { M64(ts) = us / 1000000; M64(ts + 8) = us % 1000000 * 1000; }
static s64 clock_get(u64 id, u64 ts) {
  if (id > 11) return -L_EINVAL;
  put_time(ts, id == 0 || id == 5 || id == 8 || id == 11 ? rt_now_us() : rt_uptime_us());   // the wall clocks; the rest count from the start
  return 0;
}
static s64 sys_uname(u64 u) {
  static const char *const f[6] = { "Linux", LINUX_HOSTNAME, LINUX_RELEASE, LINUX_VERSION, "x86_64", "(none)" };
  memset((void *)GP(u), 0, 6 * 65);
  for (int i = 0; i < 6; i++) strcpy((char *)GP(u + 65 * (u64)i), f[i]);
  return 0;
}
static s64 sys_sysinfo(u64 s) {
  memset((void *)GP(s), 0, 112);
  M64(s) = rt_uptime_us() / 1000000; M64(s + 32) = LINUX_MEM_TOTAL; M64(s + 40) = LINUX_MEM_TOTAL - linux_mem_used(); M16(s + 80) = 1; M32(s + 104) = 1;
  return 0;
}
static s64 sys_getcwd(u64 buf, u64 n) { size_t l = strlen(linux_cwd) + 1; if (n < l) return -L_ERANGE; memcpy((void *)GP(buf), linux_cwd, l); return (s64)l; }
static s64 sys_chdir(const char *path) {
  char abs[1024]; Node n; s64 r = fs_path(-100, path, abs, sizeof abs); if (r) return r;
  r = fs_lookup(abs, &n, 1); if (r) return r; free(n.data);
  if (n.type != N_DIR) return -L_ENOTDIR;
  strcpy(linux_cwd, abs); return 0;
}
static s64 sys_prlimit(u64 pid, u64 what, u64 set, u64 old) {
  (void)pid; if (set) return -L_EPERM;
  if (old) { u64 v = what == 3 ? rt_modules[0].stack_reserve : what == 7 ? 256 : ~0ull;   // RLIMIT_STACK, RLIMIT_NOFILE; the rest unlimited
    M64(old) = v; M64(old + 8) = v; }
  return 0;
}
static s64 sys_futex(u64 addr, u64 op, u64 val, u64 timeout) {
  switch (op & 0x7F) {
  case 0: case 9:   // FUTEX_WAIT, FUTEX_WAIT_BITSET: nobody else can change the word or wake us
    if (M32(addr) != (u32)val) return -L_EAGAIN;
    if (!timeout) rt_fatal("futex: the title waits for a wake-up nobody can send (it has one thread)");
    sleep_us(M64(timeout) * 1000000 + M64(timeout + 8) / 1000);
    return -L_ETIMEDOUT;
  case 1: case 10: return 0;   // FUTEX_WAKE: no one waits
  }
  return -L_ENOSYS;
}

static int strict = -1;
static s64 unknown(Cpu *c, u64 nr) {
  if (strict < 0) { const char *e = getenv("SW_STRICT"); strict = e && *e == '1'; }
  if (strict) rt_fatal("system call %llu (%s) is not implemented", nr, name_of(nr));
  static u8 told[512];
  if (nr < 512 && !told[nr]) { told[nr] = 1; rt_log("system call %llu (%s) is not implemented: ENOSYS", nr, name_of(nr)); }
  (void)c; return -L_ENOSYS;
}

void rt_syscall(Cpu *c, u64 next) {
  u64 nr = R[RAX], a = R[RDI], b = R[RSI], d = R[RDX], e = R[10], f = R[8], g = R[9];
  #define S(p) ((const char *)GP(p))
  #define P(p) ((void *)GP(p))
  enum { AT_FDCWD = -100 };
  s64 r;
  switch (nr) {
  // files
  case 0: r = sys_read((s64)a, P(b), d); break;
  case 1: r = sys_write((s64)a, P(b), d); break;
  case 2: r = sys_openat(AT_FDCWD, S(a), b); break;
  case 3: r = sys_close((s64)a); break;
  case 4: r = sys_fstatat(AT_FDCWD, S(a), b, 0); break;
  case 5: r = sys_fstat((s64)a, b); break;
  case 6: r = sys_fstatat(AT_FDCWD, S(a), b, 0x100); break;
  case 7: r = sys_poll(a, b, (s32)d); break;
  case 271: r = sys_poll(a, b, d ? (s64)(M64(d) * 1000 + M64(d + 8) / 1000000) : -1); break;
  case 8: r = sys_lseek((s64)a, (s64)b, d); break;
  case 16: r = sys_ioctl((s64)a, b, d); break;
  case 17: r = sys_pread((s64)a, P(b), d, e); break;
  case 19: r = sys_readv((s64)a, b, d); break;
  case 20: r = sys_writev((s64)a, b, d); break;
  case 21: r = sys_faccessat(AT_FDCWD, S(a), b); break;
  case 32: r = sys_dup((s64)a, -1); break;
  case 33: r = sys_dup((s64)a, (s64)b); break;
  case 292: r = a == b ? -L_EINVAL : sys_dup((s64)a, (s64)b); if (r >= 0 && (d & 02000000)) sys_fcntl(r, 2, 1); break;
  case 72: r = sys_fcntl((s64)a, b, d); break;
  case 79: r = sys_getcwd(a, b); break;
  case 80: r = sys_chdir(S(a)); break;
  case 89: r = sys_readlinkat(AT_FDCWD, S(a), P(b), d); break;
  case 137: r = sys_statfs(S(a), b); break;
  case 138: r = fd_is_file((s64)a) ? sys_statfs("/", b) : -L_EBADF; break;
  case 217: r = sys_getdents64((s64)a, b, d); break;
  case 257: r = sys_openat((s64)(s32)a, S(b), d); break;
  case 262: r = sys_fstatat((s64)(s32)a, S(b), d, e); break;
  case 267: r = sys_readlinkat((s64)(s32)a, S(b), P(d), e); break;
  case 269: case 439: r = sys_faccessat((s64)(s32)a, S(b), d); break;
  case 332: r = sys_statx((s64)(s32)a, S(b), d, e, f); break;
  case 18: case 74: case 76: case 77: case 82: case 83: case 84: case 86: case 87: case 88: case 90: case 92: case 258: case 263: r = -L_EROFS; break;   // nothing can be written
  case 73: case 95: r = 0; break;   // flock, umask
  // memory
  case 9: r = sys_mmap(a, b, d, e, (s64)(s32)f, g); break;
  case 10: case 28: case 26: r = 0; break;   // mprotect, madvise, msync
  case 11: r = sys_munmap(a, b); break;
  case 12: r = sys_brk(a); break;
  case 25: r = sys_mremap(a, b, d, e, f); break;
  // the process: one, with one thread
  case 39: case 186: r = host_proc_pid(); break;   // getpid, gettid
  case 111: case 121: case 124: r = 1; break;   // getpgrp, getpgid, getsid: one group, the first process's
  case 110: r = host_proc_ppid(); break;
  case 109: case 112: r = 0; break;   // setpgid, setsid
  case 102: case 104: case 107: case 108: r = LINUX_UID; break;
  case 118: case 120: M32(a) = LINUX_UID; M32(b) = LINUX_UID; M32(d) = LINUX_UID; r = 0; break;
  case 115: r = 0; break;   // getgroups: none besides its own
  case 105: case 106: case 113: case 114: case 116: case 117: case 119: r = (a == LINUX_UID || a == (u64)-1 || nr == 116) ? 0 : -L_EPERM; break;   // setuid and the rest: to what it is already
  case 122: case 123: r = LINUX_UID; break;   // setfsuid, setfsgid: the old value
  case 218: r = 1; break;   // set_tid_address
  case 158: if (a == 0x1002) { c->fs_base = b; r = 0; } else if (a == 0x1003) { M64(b) = c->fs_base; r = 0; } else if (a == 0x1001) { c->gs_base = b; r = 0; } else r = -L_EINVAL; break;
  case 13: if (d) memset(P(d), 0, 32); r = 0; break;   // rt_sigaction: no signal is ever delivered
  case 14: if (d) memset(P(d), 0, e > 8 ? 8 : e); r = 0; break;
  case 131: if (b) { memset(P(b), 0, 24); M32(b + 8) = 2; } r = 0; break;   // sigaltstack: none (SS_DISABLE)
  case 273: r = 0; break;   // set_robust_list
  case 334: r = -L_ENOSYS; break;   // rseq is not there (as on an older kernel)
  case 157: r = a == 15 || a == 16 ? 0 : -L_EINVAL; break;   // prctl: the thread's name
  case 24: r = 0; break;
  case 203: r = 0; break;
  case 204: if (b < 8) r = -L_EINVAL; else { memset(P(d), 0, 8); M8(d) = 1; r = 8; } break;   // one CPU
  case 309: if (a) M32(a) = 0; if (b) M32(b) = 0; r = 0; break;
  case 57: case 58: r = sys_fork(c, next); break;   // fork, vfork (as a fork: the child has memory of its own)
  case 56: r = (a & 0x100) ? -L_ENOSYS : sys_fork(c, next); break;   // clone: a process, not a thread (CLONE_VM)
  case 435: r = -L_ENOSYS; break;
  case 59: r = sys_execve(S(a), b, d); break;
  case 61: r = sys_wait4((s64)(s32)a, b, d); break;
  case 62: if (b == 0 || ((s32)a != (s32)host_proc_pid() && (s32)a > 0)) { r = 0; break; }   // kill: no signal reaches another process yet
           rt_fatal("the title sent itself (or its group) signal %llu (abort is 6)", b);
  case 200: case 234: if ((nr == 234 ? d : b) == 0) { r = 0; break; } rt_fatal("the title sent itself signal %llu (abort is 6)", nr == 234 ? d : b);
  case 60: case 231: linux_exit((u32)a); r = 0; break;
  case 22: r = sys_pipe2(a, 0); break;
  case 293: r = sys_pipe2(a, b); break;
  case 290: case 291: case 319: r = -L_ENOSYS; break;   // eventfd, epoll, memfd
  case 41: r = -L_EAFNOSUPPORT; break;   // no network
  case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: r = -88; break;   // ENOTSOCK: no descriptor is a socket
  // the machine and the time
  case 63: r = sys_uname(a); break;
  case 99: r = sys_sysinfo(a); break;
  case 96: if (a) { u64 us = rt_now_us(); M64(a) = us / 1000000; M64(a + 8) = us % 1000000; } r = 0; break;
  case 201: r = (s64)(rt_now_us() / 1000000); if (a) M64(a) = (u64)r; break;
  case 228: r = clock_get(a, b); break;
  case 229: if (b) { M64(b) = 0; M64(b + 8) = 1000; } r = 0; break;
  case 35: sleep_us(M64(a) * 1000000 + M64(a + 8) / 1000); r = 0; break;
  case 230: if (b & 1) { u64 now = a == 0 ? rt_now_us() : rt_uptime_us(), to = M64(d) * 1000000 + M64(d + 8) / 1000; if (to > now) sleep_us(to - now); } else sleep_us(M64(d) * 1000000 + M64(d + 8) / 1000); r = 0; break;
  case 97: r = sys_prlimit(0, a, 0, b); break;
  case 302: r = sys_prlimit(a, b, d, e); break;
  case 98: memset(P(b), 0, 144); r = 0; break;   // getrusage
  case 100: if (a) memset(P(a), 0, 32); r = (s64)(rt_uptime_us() / 10000); break;
  case 318: if (rt_clock_virtual()) memset(P(a), 0x50, b); else arc4random_buf(P(a), b); r = (s64)b; break;   // (a run on the virtual clock repeats)
  case 202: r = sys_futex(a, b, d, e); break;
  default: r = unknown(c, nr);
  }
  if (rt_trace) rt_log("%s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = %lld", name_of(nr), a, b, d, e, (long long)r);
  R[RAX] = (u64)r;
}
