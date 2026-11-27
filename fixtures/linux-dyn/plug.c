// SPDX-License-Identifier: GPL-2.0-or-later
// libplug.so: a library fixtures/linux-dyn's program opens itself (dlopen) and calls by name (dlsym)
#include <string.h>
#include <stdlib.h>
static int cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
int plug_sort(const char **v, int n) { qsort(v, n, sizeof *v, cmp); return n; }
