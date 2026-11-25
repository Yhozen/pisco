// SPDX-License-Identifier: GPL-2.0-or-later
// See com.h.
#include <stdarg.h>
#include "com.h"

void rt_import_set_pop(u64 addr, int bytes); // process.c
u64 rt_method_import(const char *dll, const char *name);

static struct { const ComInterface *iface; char dll[16]; u32 vtable; } made[64]; static unsigned made_count;
static pthread_mutex_t com_lock = PTHREAD_MUTEX_INITIALIZER;
u32 com_vtable(const char *dll, const ComInterface *iface) {
  pthread_mutex_lock(&com_lock);
  for (unsigned i = 0; i < made_count; i++) if (made[i].iface == iface && !strcmp(made[i].dll, dll)) { u32 v = made[i].vtable; pthread_mutex_unlock(&com_lock); return v; }
  u32 *vt = calloc(iface->count, 4);
  for (unsigned i = 0; i < iface->count; i++) {
    char m[160]; snprintf(m, sizeof m, "%s_%s", iface->name, iface->methods[i].name);
    u64 a = rt_method_import(dll, m);
    if (!a) { char full[200]; snprintf(full, sizeof full, "%s.dll!%s::%s", dll, iface->name, iface->methods[i].name); a = rt_stub_import(full); }
    rt_import_set_pop(a, iface->methods[i].bytes);
    vt[i] = (u32)a;
  }
  if (made_count < 64) { made[made_count].iface = iface; snprintf(made[made_count].dll, sizeof made[0].dll, "%s", dll); made[made_count++].vtable = (u32)(uptr)vt; }
  pthread_mutex_unlock(&com_lock);
  return (u32)(uptr)vt;
}
u32 com_new(const char *dll, const ComInterface *iface, u32 size) {
  u32 *o = calloc(1, size < 8 ? 8 : size);
  o[0] = com_vtable(dll, iface);
  return (u32)(uptr)o;
}
int com_is_iid(u64 guid, const char *text) {
  static const signed char order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15}; // the first three fields are little-endian
  unsigned k = 0;
  for (const char *p = text; *p && k < 16; p++) {
    if (*p == '-') continue;
    unsigned v; if (sscanf(p, "%2x", &v) != 1) return 0;
    if (M8(guid + order[k++]) != v) return 0;
    p++;
  }
  return k == 16;
}
int com_strict(void) { static int strict = -1; if (strict < 0) { const char *e = getenv("SW_STRICT"); strict = e && *e == '1'; } return strict; }
void com_unsure(const char *fmt, ...) {
  char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (com_strict()) rt_fatal("%s", buf);
  rt_log("%s", buf);
}
