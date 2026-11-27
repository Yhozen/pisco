// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/linux-files: what a Linux drive is (docs/spec/image.md#files), run with the root as its working folder:
// two names that differ by case are two files, a symbolic link (to a file, to a folder) is one, and a file without
// its executable bit is not run. The same tree natively (tests/run.sh linux) is the oracle.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cat(const char *p) {
  char b[256]; int fd = open(p, O_RDONLY);
  if (fd < 0) { printf("open %s: errno %d\n", p, errno); return; }
  ssize_t n = read(fd, b, sizeof b - 1); close(fd); b[n < 0 ? 0 : n] = 0; b[strcspn(b, "\n")] = 0;
  printf("open %s: \"%s\"\n", p, b);
}
static void kind(const char *p) {
  struct stat l, s; int a = lstat(p, &l), b = stat(p, &s);
  printf("%s: lstat %s, stat %s, mode %o\n", p, a ? "missing" : S_ISLNK(l.st_mode) ? "link" : S_ISDIR(l.st_mode) ? "folder" : "file",
         b ? "missing" : S_ISDIR(s.st_mode) ? "folder" : "file", b ? 0 : (unsigned)(s.st_mode & 0111) ? 1 : 0);
}
int main(void) {
  cat("Makefile"); cat("makefile"); cat("MAKEFILE");
  char t[256]; ssize_t n = readlink("link-to-data", t, sizeof t - 1); t[n < 0 ? 0 : n] = 0;
  printf("readlink link-to-data: \"%s\"\n", t);
  n = readlink("Makefile", t, sizeof t); printf("readlink Makefile: %zd errno %d\n", n, n < 0 ? errno : 0);
  cat("link-to-data"); cat("d/hello.txt");
  kind("link-to-data"); kind("d"); kind("data"); kind("script.sh"); kind("run.sh");
  printf("access script.sh X_OK: %d\n", access("script.sh", X_OK) ? errno : 0);
  printf("access run.sh X_OK: %d\n", access("run.sh", X_OK) ? errno : 0);
  char *argv[] = { "script.sh", 0 }, *envp[] = { 0 };
  execve("script.sh", argv, envp);
  printf("execve script.sh: errno %d\n", errno);
  return 0;
}
