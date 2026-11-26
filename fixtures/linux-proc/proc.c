// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/linux-proc: processes, as a shell uses them. fork, a pipe from the child to the parent, execve of this
// program again, wait4 and the child's exit code, a script run through its #! line, and a longjmp to before a
// fork. It prints the same run natively and under the runtime (tests/run.sh linux).
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char self[512];
static jmp_buf back;
static void child_says(const char *what) {
  int p[2]; if (pipe(p)) { perror("pipe"); exit(1); }
  pid_t pid = fork();
  if (pid == 0) { close(p[0]); dup2(p[1], 1); close(p[1]); printf("%s from the child", what); fflush(stdout); _exit(7); }
  close(p[1]);
  char buf[256]; size_t n = 0; ssize_t k;
  while ((k = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)k;
  buf[n] = 0; close(p[0]);
  int st = 0; pid_t w = waitpid(pid, &st, 0);
  printf("pipe: \"%s\", waited for the child: %s, exit code %d\n", buf, w == pid ? "yes" : "no", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}
static void run(char *const argv[], const char *label) {
  int p[2]; pipe(p);
  pid_t pid = fork();
  if (pid == 0) { dup2(p[1], 1); close(p[0]); close(p[1]); execv(argv[0], argv); printf("execv failed: %s\n", strerror(errno)); _exit(127); }
  close(p[1]);
  char buf[512]; size_t n = 0; ssize_t k;
  while ((k = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)k;
  buf[n] = 0; close(p[0]);
  int st; waitpid(pid, &st, 0);
  printf("%s: \"%s\", exit code %d\n", label, buf, WEXITSTATUS(st));
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--echo")) { for (int i = 2; i < argc; i++) printf("%s%s", i > 2 ? " " : "", argv[i]); return 5; }
  if (argc > 2 && !strcmp(argv[1], "--script")) { for (int i = 1; i < argc; i++) printf("[%s]", argv[i][0] == '/' ? strrchr(argv[i], '/') + 1 : argv[i]); return 6; }
  setvbuf(stdout, 0, _IOLBF, 0);   // (nothing left in the buffer to be copied into a child)
  ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1); self[l > 0 ? l : 0] = 0;
  printf("pid %s 1, parent %d\n", getpid() > 0 ? ">=" : "<", getppid() >= 0);
  child_says("hello");
  child_says("again");
  { char *a[] = { self, "--echo", "an", "exec'd", "copy", 0 }; run(a, "execv"); }
  if (argc > 1) { char *a[] = { argv[1], "x", 0 }; run(a, "script"); }
  // a longjmp in the child to a setjmp made before the fork
  int p[2]; pipe(p);
  if (setjmp(back)) { printf("longjmp in the child"); fflush(stdout); _exit(3); }
  pid_t pid = fork();
  if (pid == 0) { dup2(p[1], 1); close(p[0]); close(p[1]); longjmp(back, 1); }
  close(p[1]); char buf[128]; ssize_t k = read(p[0], buf, sizeof buf - 1); buf[k > 0 ? k : 0] = 0;
  int st; waitpid(pid, &st, 0);
  printf("setjmp before the fork: \"%s\", exit code %d\n", buf, WEXITSTATUS(st));
  // three children at once, waited for in any order
  int codes = 0;
  for (int i = 1; i <= 3; i++) if (fork() == 0) _exit(10 * i);
  for (int i = 0; i < 3; i++) { int s; if (wait(&s) > 0) codes += WEXITSTATUS(s); }
  printf("three children: their codes add up to %d; wait then says %s\n", codes, wait(0) < 0 && errno == ECHILD ? "no children" : "something else");
  return 0;
}
