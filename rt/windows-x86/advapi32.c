// SPDX-License-Identifier: GPL-2.0-or-later
// The registry, for Windows desktop apps: read-only, made of the values an app is given. They come from two
// places, read once at the first call: the table built into the module (rt_registry, recipe.gen.c: a recipe read
// by tools/build-app.sh), then the host's SW_REGISTRY, the registry of an app image (its config: the base's
// defaults and the recipe's [registry], web/image.mjs), whose values win. SW_REGISTRY is one value a line:
// "<full key path>\t<name>\tdword\t<number>" or "<full key path>\t<name>\tsz\t<text>".
// A key exists when a value is in it or below it; every other key is missing, as on a system where nothing was
// configured (D3DX looks for HKLM\Software\Microsoft\Direct3D and carries on without it). Writes are not kept.
#include <strings.h>
#include "../rt.h"

typedef struct { char *key, *name, *string; u32 number; } Value;
static Value *values; static u32 nvalues;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static int ieq(const char *a, const char *b) { return !strcasecmp(a, b); }
static void add(const char *key, const char *name, const char *string, u32 number) {
  for (u32 i = 0; i < nvalues; i++) if (ieq(values[i].key, key) && ieq(values[i].name, name)) { free(values[i].string); values[i].string = string ? strdup(string) : 0; values[i].number = number; return; }
  values = realloc(values, sizeof *values * (nvalues + 1));
  values[nvalues++] = (Value){strdup(key), strdup(name), string ? strdup(string) : 0, number};
}
static void load(void) {
  static int loaded; if (loaded) return; loaded = 1;
  for (u32 i = 0; i < rt_registry_count; i++) add(rt_registry[i].key, rt_registry[i].name, rt_registry[i].string, rt_registry[i].number);
  const char *e = getenv("SW_REGISTRY"); if (!e) return;
  char *text = strdup(e);
  for (char *line = strtok(text, "\n"); line; line = strtok(0, "\n")) {
    char *f[4] = {line, 0, 0, 0};
    for (int k = 1; k < 4 && f[k - 1]; k++) { char *t = strchr(f[k - 1], '\t'); if (t) { *t = 0; f[k] = t + 1; } }
    if (!f[3]) { rt_log("registry: SW_REGISTRY line left out (not key, name, type, value): %s", line); continue; }
    if (!strcmp(f[2], "dword")) add(f[0], f[1], 0, (u32)strtoul(f[3], 0, 0));
    else if (!strcmp(f[2], "sz")) add(f[0], f[1], f[3], 0);
    else rt_log("registry: SW_REGISTRY type %s not known (dword, sz)", f[2]);
  }
  free(text);
  if (rt_trace) rt_log("registry: %u values", nvalues);
}

// keys: the predefined roots, and the handles RegOpenKey hands out (their full paths)
static const char *root_name(u32 h) {
  switch (h) { case 0x80000000u: return "HKEY_CLASSES_ROOT"; case 0x80000001u: return "HKEY_CURRENT_USER"; case 0x80000002u: return "HKEY_LOCAL_MACHINE";
               case 0x80000003u: return "HKEY_USERS"; case 0x80000005u: return "HKEY_CURRENT_CONFIG"; default: return 0; }
}
#define KEY_BASE 0x1000u
static char **keys; static u32 nkeys;
static const char *key_path(u32 h) { const char *r = root_name(h); if (r) return r; h -= KEY_BASE; return h % 4 == 0 && h / 4 < nkeys ? keys[h / 4] : 0; }
static int key_exists(const char *path) {
  size_t l = strlen(path);
  if (!strchr(path, '\\')) return 1;   // a root
  for (u32 i = 0; i < nvalues; i++) if (!strncasecmp(values[i].key, path, l) && (!values[i].key[l] || values[i].key[l] == '\\')) return 1;
  return 0;
}
// RegOpenKey(Ex)A: 0 and a handle in *out, or ERROR_FILE_NOT_FOUND (2) / ERROR_INVALID_HANDLE (6)
static u32 open_key(u32 parent, u64 sub, u64 out) {
  pthread_mutex_lock(&reg_lock); load();
  const char *base = key_path(parent); u32 err = 0;
  char path[1024];
  if (!base) err = 6;
  else {
    const char *s = sub ? GSTR(sub) : ""; while (*s == '\\') s++;
    snprintf(path, sizeof path, *s ? "%s\\%s" : "%s", base, s);
    size_t l = strlen(path); while (l && path[l - 1] == '\\') path[--l] = 0;
    if (!key_exists(path)) err = 2;
  }
  if (err) { if (rt_trace) rt_log("registry: no key %s", base ? path : "(bad handle)"); if (out) M32(out) = 0; pthread_mutex_unlock(&reg_lock); return err; }
  u32 k = 0; while (k < nkeys && keys[k]) k++;
  if (k == nkeys) { keys = realloc(keys, sizeof *keys * (nkeys + 1)); nkeys++; }
  keys[k] = strdup(path);
  if (rt_trace) rt_log("registry: open %s", path);
  if (out) M32(out) = KEY_BASE + 4 * k;
  pthread_mutex_unlock(&reg_lock);
  return 0;
}
IMP(advapi32, RegOpenKeyA) { RET(open_key((u32)ARG(0), ARG(1), ARG(2))); }
IMP(advapi32, RegOpenKeyExA) { RET(open_key((u32)ARG(0), ARG(1), ARG(4))); }
// RegQueryValueExA(key, name, reserved, type*, data*, size*): REG_SZ 1, REG_DWORD 4; ERROR_MORE_DATA 234
IMP(advapi32, RegQueryValueExA) {
  pthread_mutex_lock(&reg_lock); load();
  const char *path = key_path((u32)ARG(0)), *name = ARG(1) ? GSTR(ARG(1)) : "";
  u64 type = ARG(3), data = ARG(4), size = ARG(5); u32 err = 2;
  Value *v = 0;
  for (u32 i = 0; path && i < nvalues && !v; i++) if (ieq(values[i].key, path) && ieq(values[i].name, name)) v = &values[i];
  if (!path) err = 6;
  else if (v) {
    u32 n = v->string ? (u32)strlen(v->string) + 1 : 4, have = size ? M32(size) : 0;
    if (type) M32(type) = v->string ? 1 : 4;
    if (data && n > have) err = 234;
    else { if (data) { if (v->string) memcpy((void *)GP(data), v->string, n); else M32(data) = v->number; } err = 0; }
    if (size) M32(size) = n;
    if (rt_trace) { if (v->string) rt_log("registry: %s\\%s = \"%s\"", path, name, v->string); else rt_log("registry: %s\\%s = %u", path, name, v->number); }
  } else if (rt_trace) rt_log("registry: no value %s\\%s", path, name);
  pthread_mutex_unlock(&reg_lock);
  RET(err);
}
IMP(advapi32, RegCloseKey) {
  u32 h = (u32)ARG(0);
  pthread_mutex_lock(&reg_lock);
  if (!root_name(h) && h >= KEY_BASE && (h - KEY_BASE) % 4 == 0 && (h - KEY_BASE) / 4 < nkeys) { free(keys[(h - KEY_BASE) / 4]); keys[(h - KEY_BASE) / 4] = 0; }
  pthread_mutex_unlock(&reg_lock);
  RET(0);
}
// (key, index, name, characters): the registry here has values, not subkeys an app would list
IMP(advapi32, RegEnumKeyA) { RET(259); }   // ERROR_NO_MORE_ITEMS

// Writes: kept in this run's registry (not on disk), so the app reads back what it wrote, as it would
IMP(advapi32, RegCreateKeyA) { // (key, subkey, &out): the key is made if missing
  pthread_mutex_lock(&reg_lock); load();
  const char *base = key_path((u32)ARG(0));
  if (!base) { pthread_mutex_unlock(&reg_lock); RET(6); return; }
  const char *sub = ARG(1) ? GSTR(ARG(1)) : ""; while (*sub == '\\') sub++;
  char path[1024]; snprintf(path, sizeof path, *sub ? "%s\\%s" : "%s", base, sub);
  if (!key_exists(path)) add(path, "", 0, 0);   // a key with its (default) value: it exists from now on
  pthread_mutex_unlock(&reg_lock);
  RET(open_key((u32)ARG(0), ARG(1), ARG(2)));
}
IMP(advapi32, RegSetValueExA) { // (key, name, reserved, type, data, size): REG_SZ 1, REG_EXPAND_SZ 2, REG_DWORD 4
  pthread_mutex_lock(&reg_lock); load();
  const char *path = key_path((u32)ARG(0)), *name = ARG(1) ? GSTR(ARG(1)) : ""; u32 type = (u32)ARG(3), err = 0;
  if (!path) err = 6;
  else if (type == 1 || type == 2) add(path, name, GSTR(ARG(4)), 0);
  else if (type == 4) add(path, name, 0, M32(ARG(4)));
  else if (rt_trace) rt_log("registry: %s\\%s of type %u not kept", path, name, type);
  if (!err && rt_trace) rt_log("registry: set %s\\%s", path, name);
  pthread_mutex_unlock(&reg_lock); RET(err);
}
IMP(advapi32, GetUserNameA) { // (buffer, &size): the size with the zero
  const char *n = "player"; u32 need = (u32)strlen(n) + 1;
  if (!ARG(1) || M32(ARG(1)) < need) { if (ARG(1)) M32(ARG(1)) = need; SET_LAST_ERROR(122); RET(0); return; }
  strcpy(GSTR(ARG(0)), n); M32(ARG(1)) = need; RET(1);
}
