// SPDX-License-Identifier: GPL-2.0-or-later
// The files the runtime makes up: /proc and /sys as a program reads them to learn about the machine, the devices
// of /dev a program opens without thinking, and the files of /etc every system has. They say what the machine
// is: a Linux x86-64 system imitated in WebAssembly, with the virtual CPU the lifter gives every title.
#include <stdarg.h>
#include "linux.h"

typedef struct { char *p; size_t n; } Buf;
static void put(Buf *b, const char *fmt, ...) {
  char tmp[2048]; va_list ap; va_start(ap, fmt); int k = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  if (k < 0) return; if (k > (int)sizeof tmp - 1) k = sizeof tmp - 1;
  b->p = realloc(b->p, b->n + (size_t)k + 1); memcpy(b->p + b->n, tmp, (size_t)k + 1); b->n += (size_t)k;
}

static void cpu_brand(char out[49]) {
  Cpu id; memset(&id, 0, sizeof id); Cpu *c = &id;
  for (u32 leaf = 0; leaf < 3; leaf++) {
    R[RAX] = 0x80000002 + leaf; R[RCX] = 0; rt_cpuid(c);
    u32 w[4] = { (u32)R[RAX], (u32)R[RBX], (u32)R[RCX], (u32)R[RDX] }; memcpy(out + 16 * leaf, w, 16);
  }
  out[48] = 0;
}
static void g_cpuinfo(Buf *b) {
  char brand[49]; cpu_brand(brand);
  unsigned f = rt_cpu_features;
  put(b, "processor\t: 0\nvendor_id\t: GenuineIntel\ncpu family\t: 6\nmodel\t\t: 158\nmodel name\t: %s\nstepping\t: 10\n", brand);
  put(b, "cpu MHz\t\t: 3000.000\ncache size\t: 0 KB\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\napicid\t\t: 0\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 13\nwp\t\t: yes\n");
  put(b, "flags\t\t:%s%s%s%s%s%s%s%s syscall nx lm%s%s%s%s%s%s%s lahf_lm\n", f & CPU_FPU ? " fpu" : "", f & CPU_TSC ? " tsc" : "", f & CPU_CX8 ? " cx8" : "", f & CPU_CMOV ? " cmov" : "",
      f & CPU_MMX ? " mmx" : "", f & CPU_FXSR ? " fxsr" : "", f & CPU_SSE ? " sse" : "", f & CPU_SSE2 ? " sse2" : "", f & CPU_SSE3 ? " pni" : "", f & CPU_PCLMUL ? " pclmulqdq" : "",
      f & CPU_SSSE3 ? " ssse3" : "", f & CPU_CX16 ? " cx16" : "", f & CPU_SSE41 ? " sse4_1" : "", f & CPU_SSE42 ? " sse4_2" : "", f & CPU_POPCNT ? " popcnt" : "");
  put(b, "bogomips\t: 6000.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 32 bits physical, 32 bits virtual\npower management:\n\n");
}
static void g_meminfo(Buf *b) {
  u64 total = LINUX_MEM_TOTAL >> 10, used = linux_mem_used() >> 10, free = total - used;
  put(b, "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:        %8u kB\nCached:         %8u kB\nSwapCached:     %8u kB\n", total, free, free, 0, 0, 0);
  put(b, "Active:         %8llu kB\nInactive:       %8u kB\nSwapTotal:      %8u kB\nSwapFree:       %8u kB\nShmem:          %8u kB\nSReclaimable:   %8u kB\n", used, 0, 0, 0, 0, 0);
}
static void g_uptime(Buf *b) { u64 us = rt_uptime_us(); put(b, "%llu.%02llu %llu.%02llu\n", us / 1000000, us / 10000 % 100, us / 1000000, us / 10000 % 100); }
static void g_loadavg(Buf *b) { put(b, "0.00 0.00 0.00 1/1 1\n"); }
static void g_version(Buf *b) { put(b, "Linux version " LINUX_RELEASE " (pisco@pisco) (the lifter) " LINUX_VERSION "\n"); }
static void g_stat(Buf *b) { u64 j = rt_uptime_us() / 10000; put(b, "cpu  %llu 0 0 0 0 0 0 0 0 0\ncpu0 %llu 0 0 0 0 0 0 0 0 0\nintr 0\nctxt 0\nbtime %llu\nprocesses 1\nprocs_running 1\nprocs_blocked 0\n", j, j, (rt_now_us() - rt_uptime_us()) / 1000000); }
static void g_mounts(Buf *b) { put(b, "pisco / pisco ro,relatime 0 0\nproc /proc proc ro,relatime 0 0\nsysfs /sys sysfs ro,relatime 0 0\n"); }
static void g_mountinfo(Buf *b) { put(b, "1 0 0:1 / / ro,relatime - pisco pisco ro\n2 1 0:2 / /proc ro,relatime - proc proc ro\n3 1 0:3 / /sys ro,relatime - sysfs sysfs ro\n"); }
static void g_cmdline(Buf *b) { for (int i = 0; i < linux_argc; i++) { put(b, "%s", linux_argv[i]); b->p = realloc(b->p, b->n + 2); b->p[b->n++] = 0; b->p[b->n] = 0; } }
static void g_environ(Buf *b) { for (int i = 0; linux_env[i]; i++) { put(b, "%s", linux_env[i]); b->p = realloc(b->p, b->n + 2); b->p[b->n++] = 0; b->p[b->n] = 0; } }
static void g_self_stat(Buf *b) { put(b, "1 (%s) R 0 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 %llu 0 -1 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n", linux_exe + 1, linux_mem_used()); }
static void g_self_status(Buf *b) { put(b, "Name:\t%s\nState:\tR (running)\nTgid:\t1\nPid:\t1\nPPid:\t0\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nVmRSS:\t%llu kB\nThreads:\t1\n", linux_exe + 1, LINUX_UID, LINUX_UID, LINUX_UID, LINUX_UID, LINUX_UID, LINUX_UID, LINUX_UID, LINUX_UID, linux_mem_used() >> 10); }
static void g_self_maps(Buf *b) {
  const ModuleDef *m = &rt_modules[0];
  for (unsigned i = 0; i < m->section_count; i++) { u64 a = m->sections[i].va & ~4095ull, e = (m->sections[i].va + m->sections[i].vsize + 4095) & ~4095ull;
    put(b, "%08llx-%08llx r-xp %08llx 00:01 1 %s\n", a, e, m->sections[i].off & ~4095ull, linux_exe); }
}
static void g_os_release(Buf *b) { put(b, "NAME=\"PISCO\"\nID=pisco\nPRETTY_NAME=\"PISCO (Linux x86-64 imitated in WebAssembly)\"\nVERSION_ID=\"1\"\nHOME_URL=\"https://github.com/Yhozen/series-web\"\n"); }
// the login shell: the first of bash and sh the file system has (a title with no shell names none)
static const char *login_shell(void) {
  static const char *const shells[] = { "/bin/bash", "/bin/sh" };
  for (unsigned i = 0; i < 2; i++) { Node n; if (!fs_lookup(shells[i], &n, 1)) { free(n.data); if (n.type == N_FILE) return shells[i]; } }
  return "";
}
static void g_passwd(Buf *b) { const char *sh = login_shell(); put(b, "root:x:0:0:root:/root:%s\n" LINUX_USER ":x:%d:%d:" LINUX_USER ":/home/" LINUX_USER ":%s\n", sh, LINUX_UID, LINUX_UID, sh); }
static void g_group(Buf *b) { put(b, "root:x:0:\n" LINUX_USER ":x:%d:\n", LINUX_UID); }
static void g_hostname(Buf *b) { put(b, LINUX_HOSTNAME "\n"); }
static void g_hosts(Buf *b) { put(b, "127.0.0.1\tlocalhost " LINUX_HOSTNAME "\n"); }
static void g_ostype(Buf *b) { put(b, "Linux\n"); }
static void g_osrelease(Buf *b) { put(b, LINUX_RELEASE "\n"); }
static void g_kversion(Buf *b) { put(b, LINUX_VERSION "\n"); }
static void g_cpus(Buf *b) { put(b, "0\n"); }
static void g_pid_max(Buf *b) { put(b, "4194304\n"); }
static void g_product(Buf *b) { put(b, "WebAssembly\n"); }
static void g_vendor(Buf *b) { put(b, "pisco\n"); }
static void g_comm(Buf *b) { put(b, "%s\n", linux_exe + 1); }
static void g_empty(Buf *b) { put(b, "%s", ""); }

typedef struct { const char *path; void (*gen)(Buf *); const char *link; int type; } Made;
static const Made made[] = {
  { "/proc/cpuinfo", g_cpuinfo }, { "/proc/meminfo", g_meminfo }, { "/proc/uptime", g_uptime }, { "/proc/loadavg", g_loadavg }, { "/proc/version", g_version },
  { "/proc/stat", g_stat }, { "/proc/mounts", 0, "/proc/self/mounts" }, { "/proc/self", 0, "/proc/1" }, { "/proc/thread-self", 0, "/proc/1/task/1" },
  { "/proc/1/exe", 0, linux_exe }, { "/proc/1/cwd", 0, "/" }, { "/proc/1/root", 0, "/" }, { "/proc/1/cmdline", g_cmdline }, { "/proc/1/environ", g_environ },
  { "/proc/1/comm", g_comm }, { "/proc/1/stat", g_self_stat }, { "/proc/1/status", g_self_status }, { "/proc/1/maps", g_self_maps }, { "/proc/1/mounts", g_mounts },
  { "/proc/1/mountinfo", g_mountinfo }, { "/proc/1/fd/0", 0, "/dev/tty" }, { "/proc/1/fd/1", 0, "/dev/tty" }, { "/proc/1/fd/2", 0, "/dev/tty" },
  { "/proc/sys/kernel/ostype", g_ostype }, { "/proc/sys/kernel/osrelease", g_osrelease }, { "/proc/sys/kernel/version", g_kversion },
  { "/proc/sys/kernel/hostname", g_hostname }, { "/proc/sys/kernel/pid_max", g_pid_max },
  { "/sys/devices/system/cpu/online", g_cpus }, { "/sys/devices/system/cpu/possible", g_cpus }, { "/sys/devices/system/cpu/present", g_cpus },
  { "/sys/devices/system/cpu/cpu0/online", g_empty },
  { "/sys/devices/virtual/dmi/id/product_name", g_product }, { "/sys/devices/virtual/dmi/id/sys_vendor", g_vendor }, { "/sys/class/dmi/id", 0, "/sys/devices/virtual/dmi/id" },
  { "/dev/null", 0, 0, N_NULL }, { "/dev/zero", 0, 0, N_NULL }, { "/dev/urandom", 0, 0, N_RANDOM }, { "/dev/random", 0, 0, N_RANDOM }, { "/dev/tty", 0, 0, N_TTY },
  { "/etc/os-release", g_os_release }, { "/etc/passwd", g_passwd }, { "/etc/group", g_group }, { "/etc/hostname", g_hostname }, { "/etc/hosts", g_hosts },
};
#define MADE (sizeof made / sizeof made[0])

int fs_generated(const char *abs, Node *n) {
  for (unsigned i = 0; i < MADE; i++) if (!strcmp(abs, made[i].path)) {
    if (made[i].link) { n->type = N_LINK; n->data = strdup(made[i].link); n->size = strlen(n->data); return 1; }
    if (made[i].type) { n->type = made[i].type; return 1; }
    Buf b = { calloc(1, 1), 0 }; made[i].gen(&b);
    n->type = N_FILE; n->data = b.p; n->size = b.n;
    return 1;
  }
  // a path through one of the links above ("/proc/self/exe"): the link's target, then the rest
  for (unsigned i = 0; i < MADE; i++) if (made[i].link) {
    size_t l = strlen(made[i].path);
    if (!strncmp(abs, made[i].path, l) && abs[l] == '/') {
      char *to = malloc(strlen(made[i].link) + strlen(abs + l) + 1); strcpy(to, made[i].link); strcat(to, abs + l);
      Node t; int e = fs_lookup(to, &t, 0); free(to);
      if (e) return 0;
      *n = t; return 1;
    }
  }
  return 0;
}
int fs_generated_dir(const char *abs) {
  size_t l = strlen(abs); if (l == 1) l = 0;
  for (unsigned i = 0; i < MADE; i++) if (!strncmp(abs, made[i].path, l) && made[i].path[l] == '/') return 1;
  return 0;
}
void fs_generated_list(const char *abs, char **out, u32 *len) {
  size_t l = strlen(abs); if (l == 1) l = 0;
  for (unsigned i = 0; i < MADE; i++) if (!strncmp(abs, made[i].path, l) && made[i].path[l] == '/') {
    const char *name = made[i].path + l + 1; size_t k = strcspn(name, "/"); int dir = name[k] == '/';
    char line[300]; int w = snprintf(line, sizeof line, "%.*s%s\n", (int)k, name, dir ? "/" : "");
    // once each, and not a name the title's folder already has
    int seen = 0;
    for (const char *p = *out; p && *p;) { const char *q = strchr(p, '\n'); size_t pl = q ? (size_t)(q - p) : strlen(p); size_t bare = pl && p[pl - 1] == '/' ? pl - 1 : pl;
      if (bare == k && !strncmp(p, name, k)) { seen = 1; break; } if (!q) break; p = q + 1; }
    if (seen) continue;
    *out = realloc(*out, *len + (size_t)w + 1); memcpy(*out + *len, line, (size_t)w + 1); *len += (u32)w;
  }
}
