// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/linux-hello: a Linux program, statically linked with musl, that uses what a small C tool uses of its
// C library and of the system. `hello --pure` prints only what does not depend on the machine: there the oracle
// is the program itself, run on the Linux that builds it (tests/run.sh linux). Without it, it also prints the
// machine it finds, which under the runtime is the one rt/linux-x64/ describes (expected.txt).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static __thread int tls_counter = 40;
static int by_length(const void *a, const void *b) {
  const char *x = *(const char *const *)a, *y = *(const char *const *)b;
  return strlen(x) != strlen(y) ? (int)strlen(x) - (int)strlen(y) : strcmp(x, y);
}
__attribute__((noinline)) static const char *name_of(int n) {
  switch (n) {   // dense: compiled to a jump table
  case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 3: return "three"; case 4: return "four";
  case 5: return "five"; case 6: return "six"; case 7: return "seven"; case 8: return "eight"; case 9: return "nine";
  default: return "many";
  }
}
static jmp_buf out;
__attribute__((noinline)) static void deep(int n) { if (n == 0) longjmp(out, 42); deep(n - 1); }
static unsigned long hash(const unsigned char *p, size_t n) { unsigned long h = 5381; while (n--) h = h * 33 + *p++; return h; }
static void first_line(const char *path, const char *key) {
  FILE *f = fopen(path, "r"); if (!f) { printf("%s: %s\n", path, strerror(errno)); return; }
  char line[512]; int found = 0;
  while (fgets(line, sizeof line, f)) if (!key || !strncmp(line, key, strlen(key))) { line[strcspn(line, "\n")] = 0; printf("%s: %s\n", path, line); found = 1; break; }
  if (!found) printf("%s: no line %s\n", path, key ? key : "");
  fclose(f);
}

int main(int argc, char **argv) {
  int pure = argc > 1 && !strcmp(argv[1], "--pure");
  // ---- the C library ----
  printf("hello: %d argument%s\n", argc - 1, argc == 2 ? "" : "s");
  printf("numbers: %d %u %x %lld %5.2f %e %g\n", -7, 3000000000u, 0xBEEF, 1234567890123LL, 3.14159, 6.02e23, 0.1 + 0.2);
  printf("maths: sqrt %.6f sin %.6f pow %.3f strtod %.4f\n", sqrt(2.0), sin(1.0), pow(2.0, 0.5), strtod("2.71828", 0));
  const char *words[] = { "pear", "fig", "banana", "kiwi", "apple", "date" };
  qsort(words, 6, sizeof *words, by_length);
  printf("sorted:"); for (int i = 0; i < 6; i++) printf(" %s", words[i]); printf("\n");
  printf("switch:"); for (int i = 0; i < 12; i += 3) printf(" %s", name_of(i)); printf("\n");
  tls_counter += 2; printf("thread-local: %d\n", tls_counter);
  { volatile int r = setjmp(out); if (!r) deep(10); printf("longjmp: %d\n", r); }
  { size_t n = 3 << 20; unsigned char *big = malloc(n), *small = malloc(100);   // a block from mmap, one from the heap
    for (size_t i = 0; i < n; i++) big[i] = (unsigned char)(i * 7); memset(small, 'x', 100);
    big = realloc(big, 2 * n); printf("malloc: %lx %lx\n", hash(big, n), hash(small, 100)); free(big); free(small); }
  { void *p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); printf("mmap: %s, zeroed %d\n", p == MAP_FAILED ? "failed" : "ok", p != MAP_FAILED && !*(char *)p); if (p != MAP_FAILED) munmap(p, 65536); }
  { char b[64]; snprintf(b, sizeof b, "%s-%03d", "id", 7); printf("snprintf: %s, length %zu\n", b, strlen(b)); }
  { struct tm tm = {0}; time_t t = 1104580800; gmtime_r(&t, &tm); char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm); printf("gmtime: %s\n", b); }
  printf("missing file: %s\n", open("/no/such/file", O_RDONLY) < 0 ? strerror(errno) : "opened?");
  { int fd = open("/dev/null", O_WRONLY); printf("/dev/null: wrote %zd\n", write(fd, "gone", 4)); close(fd); }
  { FILE *f = fopen("/proc/self/cmdline", "r"); char b[256]; size_t n = f ? fread(b, 1, sizeof b, f) : 0; if (f) fclose(f);
    b[n < sizeof b ? n : sizeof b - 1] = 0; printf("cmdline: the program's name ends in %s\n", n >= 5 && !strcmp(b + strlen(b) - 5, "hello") ? "hello" : "something else"); }
  fflush(stdout); fprintf(stderr, "this line goes to standard error\n");
  if (pure) return 3;
  // ---- the machine ----
  struct utsname u; uname(&u); printf("uname: %s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
  first_line("/etc/os-release", "PRETTY_NAME=");
  first_line("/proc/cpuinfo", "model name");
  first_line("/proc/meminfo", "MemTotal:");
  first_line("/proc/version", 0);
  first_line("/etc/hostname", 0);
  { char b[256]; ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1); if (n < 0) n = 0; b[n] = 0; printf("exe: %s\n", b); }
  { struct passwd *pw = getpwuid(getuid()); printf("user: %s (%d), home %s, shell %s\n", pw ? pw->pw_name : "?", (int)getuid(), pw ? pw->pw_dir : "?", pw && *pw->pw_shell ? pw->pw_shell : "(none named)"); }
  printf("environment: HOME=%s LANG=%s\n", getenv("HOME"), getenv("LANG"));
  { struct sysinfo si; sysinfo(&si); printf("sysinfo: %lu MiB of memory, %d process, %ld processor\n", (unsigned long)(si.totalram * si.mem_unit >> 20), si.procs, sysconf(_SC_NPROCESSORS_ONLN)); }
  { struct stat st; int e = stat("/proc/cpuinfo", &st); printf("stat /proc/cpuinfo: %s\n", e ? strerror(errno) : S_ISREG(st.st_mode) ? "a file" : "not a file");
    e = stat("/proc", &st); printf("stat /proc: %s\n", e ? strerror(errno) : S_ISDIR(st.st_mode) ? "a directory" : "not a directory"); }
  { DIR *d = opendir("/proc/sys/kernel"); int n = 0, seen = 0; struct dirent *e; while (d && (e = readdir(d))) { n++; if (!strcmp(e->d_name, "ostype")) seen = 1; } if (d) closedir(d);
    printf("/proc/sys/kernel: %d entries, ostype %s\n", n, seen ? "among them" : "missing"); }
  { struct winsize w; printf("terminal: %s\n", isatty(1) ? (ioctl(1, TIOCGWINSZ, &w), "yes") : "no"); }
  { time_t t = time(0); struct tm tm; gmtime_r(&t, &tm); char b[64]; strftime(b, sizeof b, "%Y-%m-%d", &tm); printf("date: %s\n", b); }
  return 0;
}
