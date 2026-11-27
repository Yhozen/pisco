// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/linux-dyn: a position-independent, dynamically linked program with libraries of its own: libgreet.so,
// which it is linked against, and libplug.so, which it opens (dlopen) and calls by name (dlsym).
#include <dlfcn.h>
#include <stdio.h>
const char *greet(const char *who, int n);
int main(int argc, char **argv) {
  printf("%s\n", greet(argc > 1 ? argv[1] : "world", 7));
  void *h = dlopen("libplug.so", RTLD_NOW);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  int (*sort)(const char **, int) = (int (*)(const char **, int))dlsym(h, "plug_sort");
  const char *v[] = { "pear", "fig", "apple", "kiwi" };
  int n = sort(v, 4);
  printf("a plugin opened: %d sorted:", n); for (int i = 0; i < n; i++) printf(" %s", v[i]); printf("\n");
  return dlclose(h);
}
