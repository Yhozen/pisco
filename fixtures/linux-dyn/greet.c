// SPDX-License-Identifier: GPL-2.0-or-later
// libgreet.so: a library fixtures/linux-dyn's program is linked against. A constructor the loader runs, data the
// loader relocates (a table of function pointers), and a function the program calls through its PLT.
#include <stdio.h>
static int ready;
__attribute__((constructor)) static void start(void) { ready = 42; }
static int twice(int x) { return 2 * x; }
static int square(int x) { return x * x; }
int (*const greet_ops[])(int) = { twice, square };
const char *greet(const char *who, int n) {
  static char b[96];
  snprintf(b, sizeof b, "hello %s from a library, %d, ops %d %d", who, ready, greet_ops[0](n), greet_ops[1](n));
  return b;
}
