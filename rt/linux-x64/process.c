// SPDX-License-Identifier: GPL-2.0-or-later
// Processes. Each is an instance of its program's module with memory of its own, the host's to start and to end.
// fork: the host copies this process's memory into a new instance, which goes on where the parent called fork
// (rt_fork_resume: the CPU's registers are in the copied memory, the guest is entered again at the return address).
// execve: the host starts the module of the program the path names, with this process's number, arguments,
// environment, working directory and descriptors. wait4: the host knows which children have ended.
#include "linux.h"

HOST("proc_fork") s32 host_proc_fork(void);                                  // the child's number, or < 0
HOST("proc_exec") s32 host_proc_exec(const char *path, u32 len, const char *args, u32 args_len, const char *env, u32 env_len);   // returns only on failure (-errno)
HOST("proc_wait") s32 host_proc_wait(s32 pid, u32 nohang, u32 *code);       // the child that ended (0: none yet with nohang; -ECHILD)

// ---- fork ----
static Cpu *fork_cpu; static u64 fork_pc;
static u8 fork_tls[4096] __attribute__((aligned(16))), child_tls[4096] __attribute__((aligned(16)));
static size_t fork_tls_size;
extern void __wasm_init_tls(void *);
s64 sys_fork(Cpu *c, u64 next) {
  fork_tls_size = __builtin_wasm_tls_size();
  if (fork_tls_size > sizeof fork_tls || __builtin_wasm_tls_align() > 16) rt_fatal("fork: the runtime's thread-local storage is larger than expected");
  memcpy(fork_tls, __builtin_wasm_tls_base(), fork_tls_size);   // (this thread's: the child gets a block of its own, these values in it)
  fork_cpu = c; fork_pc = next;
  fd_fork();
  s32 pid = host_proc_fork();
  if (pid < 0) { rt_fatal("fork: the host could not start a process"); }
  return pid;
}
__attribute__((export_name("rt_fork_resume"))) void rt_fork_resume(void) {
  __wasm_init_tls(child_tls); memcpy(child_tls, fork_tls, fork_tls_size);
  Cpu *c = fork_cpu;
  R[RAX] = 0;   // fork's result in the child
  if (rt_trace) rt_log("fork: the child goes on at 0x%llx", fork_pc);
  linux_run(c, fork_pc);
}

// ---- wait4 ----
s64 sys_wait4(s64 pid, u64 status, u64 options) {
  u32 code = 0; s32 r = host_proc_wait((s32)pid, (u32)(options & 1), &code);   // WNOHANG
  if (r > 0 && status) M32(status) = (code & 0xFF) << 8;   // ended by exit(code)
  return r;
}

// ---- exit ----
void linux_exit(u32 code) { fd_close_all(); rt_exit_process(code); __builtin_unreachable(); }

// ---- execve ----
static char *join(char *out, size_t *n, size_t *cap, const char *s, char sep) {
  size_t l = strlen(s);
  while (*n + l + 2 > *cap) { *cap *= 2; out = realloc(out, *cap); }
  memcpy(out + *n, s, l); *n += l; out[(*n)++] = sep; out[*n] = 0;
  return out;
}
s64 sys_execve(const char *path, u64 argv, u64 envp) {
  enum { MAXARGS = 4096 };
  const char **args = calloc(MAXARGS + 8, sizeof *args); unsigned argc = 0;
  for (u64 p = argv; p && M64(p) && argc < MAXARGS; p += 8) args[argc++] = (const char *)GP(M64(p));
  char abs[1024], first[256];
  s64 e = fs_path(-100, path, abs, sizeof abs); if (e) { free(args); return e; }
  // a script names the program that runs it (#!): that program, with the script's path among its arguments
  for (int depth = 0;; depth++) {
    Node n; e = fs_lookup(abs, &n, 1); if (e) { free(args); return e; }
    if (n.type == N_DIR) { free(args); return -L_EACCES; }
    if (n.type != N_FILE) { free(n.data); free(args); return -L_EACCES; }
    u64 len = n.size < sizeof first - 1 ? n.size : sizeof first - 1;
    if (n.data) memcpy(first, n.data, len); else len = host_read_file(n.host, (u32)strlen(n.host), 0, len, (u64)(uptr)first);
    first[len] = 0; free(n.data);
    if (len >= 4 && !memcmp(first, "\x7f" "ELF", 4)) break;
    if (len < 2 || first[0] != '#' || first[1] != '!' || depth == 4) { free(args); return depth == 4 ? -L_ELOOP : -8; }   // ENOEXEC
    char *line = first + 2; line[strcspn(line, "\n")] = 0;
    while (*line == ' ' || *line == '\t') line++;
    char *interp = line; while (*line && *line != ' ' && *line != '\t') line++;
    char *opt = 0; if (*line) { *line++ = 0; while (*line == ' ' || *line == '\t') line++; if (*line) { opt = line; size_t l = strlen(opt); while (l && (opt[l - 1] == ' ' || opt[l - 1] == '\t' || opt[l - 1] == '\r')) opt[--l] = 0; } }
    if (argc + 2 > MAXARGS) { free(args); return -7; }   // E2BIG
    unsigned add = opt ? 2 : 1;
    memmove(args + add + 1, args + 1, (argc ? argc - 1 : 0) * sizeof *args);
    char *script = strdup(abs);
    args[0] = strdup(interp); if (opt) args[1] = strdup(opt); args[add] = script;
    argc = (argc ? argc : 1) + add;
    e = fs_path(-100, interp, abs, sizeof abs); if (e) { free(args); return e; }
  }
  // what the next program is given: its arguments after the first (the host's for a WASI program), and the
  // runtime's settings: its path, its first argument, environment, working directory and descriptors
  size_t an = 0, acap = 256, sn = 0, scap = 1024; char *ab = malloc(acap), *sb = malloc(scap); ab[0] = sb[0] = 0;
  for (unsigned i = 1; i < argc; i++) ab = join(ab, &an, &acap, args[i], 0);
  char *env = malloc(1); size_t en = 0, ecap = 1; env[0] = 0;
  for (u64 p = envp; p && M64(p); p += 8) { const char *v = (const char *)GP(M64(p)); if (strchr(v, '\x1e')) continue; env = join(env, &en, &ecap, v, '\x1e'); }
  if (en) env[--en] = 0;
  char *fds = fd_export();
  char setting[4096];
  #define SET(fmt, ...) do { snprintf(setting, sizeof setting, fmt, __VA_ARGS__); sb = join(sb, &sn, &scap, setting, 0); } while (0)
  SET("SW_EXE=%s", abs); SET("SW_ARGV0=%s", argc ? args[0] : abs); SET("SW_CWD=%s", linux_cwd);
  sb = join(sb, &sn, &scap, "SW_ENVIRON=", 0); sn--; sb = join(sb, &sn, &scap, env, 0);
  sb = join(sb, &sn, &scap, "SW_FDS=", 0); sn--; sb = join(sb, &sn, &scap, fds, 0);
  char host[1100]; snprintf(host, sizeof host, "%s%s", linux_drive ? "c:" : "", linux_drive ? abs : abs + 1);
  if (rt_trace) rt_log("execve %s", abs);
  s32 r = host_proc_exec(host, (u32)strlen(host), ab, (u32)an, sb, (u32)sn);
  free(ab); free(sb); free(env); free(fds); free(args);
  return r;   // (the descriptors marked close-on-exec are closed already)
}
