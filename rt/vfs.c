// SPDX-License-Identifier: GPL-2.0-or-later
// The app's file system. Console paths map to mounts:
//   G:\ GAME:\ D:\ C:\title\ and relative paths -> "game": the title's folder, read-only, read through the host;
//   S:\ -> "pls" (persistent local storage), T:\ -> "temp", any other drive -> "<letter>".
// The rest of C:\ is read through the host too when the app runs from an image: its file layers make the drive
// (web/image.mjs; C:\WINDOWS comes from the base). The title's folder is reached as C:\title.
// Writes go to an in-memory overlay (a copy of the file is made on first write); deletes leave a whiteout;
// listings merge the overlay with the folder. Nothing in the app's folder is ever changed.
// What persists (the host keeps it between runs, below): S:\ for every app; for a Windows desktop app
// (the desktop runtimes' glue.c set vfs_writable_game) also what it writes in its own folder, the top layer over its files
// (Narbacular Drop's system.opt): the files it changed or made there, whole (files it deletes there are not recorded).
#include <ctype.h>
#include <time.h>
#include "rt.h"
#include "vfs.h"
static const char *static_mount(const char *d);

HOST("stat") s64 host_stat(const char *name, u32 len);                        // size, -1 missing, -2 directory
HOST("list") u32 host_list(const char *name, u32 len, char *buf, u32 cap);   // "name\n" lines, directories end in '/'
// persistent local storage (S:\, mount "pls") survives the app: the host keeps its files (OPFS in the browser,
// a folder in Node). It hands them over at the start; every change is sent back.
HOST("store_info") s64 host_store_info(u32 index, char *name, u32 cap);          // size of stored file `index` and its path, -1 after the last
HOST("store_read") void host_store_read(u32 index, void *dst);
HOST("store_save") void host_store_save(const char *name, u32 len, const void *data, u32 n);
HOST("store_remove") void host_store_remove(const char *name, u32 len);

typedef struct Node { char *path; int dir, gone; u8 *data; u64 size, cap; u64 mtime; struct Node *next; } Node;
#define BUCKETS 4096
static Node *nodes[BUCKETS];
static pthread_mutex_t vfs_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 hash(const char *s) { u32 h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h % BUCKETS; }
static Node *node_raw(const char *p) { for (Node *n = nodes[hash(p)]; n; n = n->next) if (!strcmp(n->path, p)) return n; return 0; }
static Node *node_new(const char *p);
__attribute__((weak)) const int vfs_writable_game = 0;   // rt/windows-x86/glue.c and rt/windows-x64/glue.c: 1
static int persistent(const char *p) { return !strncmp(p, "pls/", 4) || (vfs_writable_game && !strncmp(p, "game/", 5)); }
static int store_loaded;
static void store_load(void) {
  if (store_loaded) return;
  store_loaded = 1;
  char name[1024];
  for (u32 i = 0;; i++) {
    s64 size = host_store_info(i, name, sizeof name);
    if (size < 0) break;
    if (!persistent(name)) continue;
    for (char *sl = strchr(strchr(name, '/') + 1, '/'); sl; sl = strchr(sl + 1, '/')) { *sl = 0; Node *d = node_new(name); d->dir = 1; d->gone = 0; *sl = '/'; }
    Node *n = node_new(name); n->dir = 0; n->gone = 0; n->size = (u64)size; n->cap = (u64)size + 1; n->data = malloc((size_t)size + 1); n->mtime = 132000000000000000ull;
    if (size) host_store_read(i, n->data);
  }
}
static Node *node(const char *p) { store_load(); return node_raw(p); }
static Node *node_new(const char *p) { Node *n = node_raw(p); if (n) return n; n = calloc(1, sizeof *n); n->path = strdup(p); u32 h = hash(p); n->next = nodes[h]; nodes[h] = n; return n; }
static u64 now_ft(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (u64)t.tv_sec * 10000000 + (u64)t.tv_nsec / 100 + 116444736000000000ull; }

// ---- paths: "mount/a/b", lower case, '/' separated, no "." or ".." ----
int vfs_path(const char *in, char *out, size_t cap) {
  char tmp[1024]; size_t n = 0;
  const char *p = in;
  const char *mount = "game";
  if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "\\\\.\\", 4)) p += 4;
  const char *colon = strchr(p, ':'), *sep = strpbrk(p, "\\/");
  if (colon && (!sep || colon < sep) && colon - p <= 8) {
    size_t dl = (size_t)(colon - p);
    char d[16]; for (size_t i = 0; i < dl; i++) d[i] = (char)tolower((u8)p[i]); d[dl] = 0;
    p = colon + 1;
    if (!strcmp(d, "g") || !strcmp(d, "game") || !strcmp(d, "d")) mount = "game";
    else if (!strcmp(d, "s")) mount = "pls";
    else if (!strcmp(d, "t")) mount = "temp";
    else if (!strcmp(d, "c") && (p[0] == '\\' || p[0] == '/') && !strncasecmp(p + 1, "title", 5) && (!p[6] || p[6] == '\\' || p[6] == '/')) { p += 6; mount = "game"; }
    else mount = dl == 1 ? static_mount(d) : "game";
  }
  n = (size_t)snprintf(tmp, sizeof tmp, "%s", mount);
  // components
  const char *s = p;
  while (*s) {
    while (*s == '\\' || *s == '/') s++;
    if (!*s) break;
    const char *e = s; while (*e && *e != '\\' && *e != '/') e++;
    size_t l = (size_t)(e - s);
    if (l == 1 && s[0] == '.') { s = e; continue; }
    if (l == 2 && s[0] == '.' && s[1] == '.') { char *slash = strrchr(tmp, '/'); if (slash) { *slash = 0; n = (size_t)(slash - tmp); } s = e; continue; }
    if (n + 1 + l >= sizeof tmp) return 0;
    tmp[n++] = '/';
    for (size_t i = 0; i < l; i++) tmp[n++] = (char)tolower((u8)s[i]);
    // trailing dots and spaces are dropped, as Windows does
    while (n > 0 && (tmp[n - 1] == '.' || tmp[n - 1] == ' ') && tmp[n - 2] != '/') n--;
    tmp[n] = 0;
    s = e;
  }
  if (n >= cap) return 0;
  memcpy(out, tmp, n + 1);
  return 1;
}
static const char *static_mount(const char *d) {
  static char names[26][4];
  int k = d[0] - 'a'; if (k < 0 || k >= 26) k = 25;
  names[k][0] = (char)('a' + k); names[k][1] = 0;
  return names[k];
}
// what the host is asked for: a path in the app's folder ("game/a/b" -> "a/b"), or on the C: drive of an app image
// ("c/windows/win.ini" -> "c:/windows/win.ini": the image's file layers, web/image.mjs; a host with no image has no
// such files). Other mounts are the overlay's alone. `buf` holds the name when it has to be made.
static const char *game_rel(const char *p, char *buf, size_t cap) {
  if (!strncmp(p, "game/", 5)) return p + 5;
  if (!strcmp(p, "game")) return "";
  if (p[0] == 'c' && (!p[1] || p[1] == '/')) { snprintf(buf, cap, "c:%s", p + 1); return buf; }
  return 0;
}

// what is at p: 0 nothing, 1 file, 2 directory; *size for files
static int kind(const char *p, u64 *size) {
  Node *n = node(p);
  if (n) { if (n->gone) return 0; if (size) *size = n->size; return n->dir ? 2 : 1; }
  if (!strchr(p, '/')) return 2; // a mount root
  char gb[1100]; const char *g = game_rel(p, gb, sizeof gb);
  if (g) { s64 s = host_stat(g, (u32)strlen(g)); if (s == -2) return 2; if (s >= 0) { if (size) *size = (u64)s; return 1; } }
  return 0;
}
static int parent_exists(const char *p) {
  char d[1024]; snprintf(d, sizeof d, "%s", p);
  char *slash = strrchr(d, '/'); if (!slash) return 1;
  *slash = 0; return kind(d, 0) == 2;
}
// the overlay copy of a file, made from the folder's file on first write
static Node *writable(const char *p) {
  Node *n = node(p);
  if (n && !n->gone && !n->dir) return n;
  u64 size = 0; int k = n && n->gone ? 0 : kind(p, &size);
  n = node_new(p); n->gone = 0; n->dir = 0; n->mtime = now_ft();
  if (k == 1) {
    char gb[1100]; const char *g = game_rel(p, gb, sizeof gb);
    n->data = malloc(size + 1); n->cap = size + 1; n->size = size;
    if (size) host_read_file(g, (u32)strlen(g), 0, size, (u64)(uptr)n->data);
  }
  return n;
}

// ---- files ----
struct VFile { char path[1024]; int write; u64 pos; VFile *next_open, *prev_open; };
// the files open for writing: the process's end saves them as their closing would (Windows writes what was written)
static VFile *open_writes;
VFile *vfs_open(const char *guest, u32 access, u32 disposition, u32 *err) {
  char p[1024]; if (!vfs_path(guest, p, sizeof p)) { *err = 3; return 0; }
  pthread_mutex_lock(&vfs_lock);
  u64 size = 0; int k = kind(p, &size);
  int write = (access & 0x40000000u) || (access & 0x10000000u) || (access & 2) || (access & 4); // GENERIC_WRITE / ALL, FILE_WRITE_DATA / APPEND
  *err = 0;
  // CREATE_NEW 1, CREATE_ALWAYS 2, OPEN_EXISTING 3, OPEN_ALWAYS 4, TRUNCATE_EXISTING 5
  if (k == 2 && disposition != 3 && disposition != 4) { *err = 5; pthread_mutex_unlock(&vfs_lock); return 0; }
  if (disposition == 1 && k) { *err = 80; pthread_mutex_unlock(&vfs_lock); return 0; }
  if ((disposition == 3 || disposition == 5) && !k) { *err = parent_exists(p) ? 2 : 3; pthread_mutex_unlock(&vfs_lock); return 0; }
  if (!k && !parent_exists(p)) { *err = 3; pthread_mutex_unlock(&vfs_lock); return 0; }
  if (k == 1 && (disposition == 2 || disposition == 4)) *err = 183; // ERROR_ALREADY_EXISTS (success)
  if (!k || disposition == 2 || disposition == 5 || (write && k == 1)) {
    Node *n = writable(p);
    if (disposition == 2 || disposition == 5) { n->size = 0; n->mtime = now_ft(); }
  }
  pthread_mutex_unlock(&vfs_lock);
  VFile *f = calloc(1, sizeof *f); snprintf(f->path, sizeof f->path, "%s", p); f->write = write;
  if (write) { pthread_mutex_lock(&vfs_lock); f->next_open = open_writes; if (open_writes) open_writes->prev_open = f; open_writes = f; pthread_mutex_unlock(&vfs_lock); }
  return f;
}
int vfs_is_dir(VFile *f) { pthread_mutex_lock(&vfs_lock); int k = kind(f->path, 0); pthread_mutex_unlock(&vfs_lock); return k == 2; }
void vfs_close(VFile *f) {
  if (f->write) {
    pthread_mutex_lock(&vfs_lock);
    if (f->prev_open) f->prev_open->next_open = f->next_open; else open_writes = f->next_open;
    if (f->next_open) f->next_open->prev_open = f->prev_open;
    Node *n = persistent(f->path) ? node(f->path) : 0; if (n && !n->gone && !n->dir) host_store_save(f->path, (u32)strlen(f->path), n->data, (u32)n->size);
    pthread_mutex_unlock(&vfs_lock);
  }
  free(f);
}
// at the process's end (rt/core.c): what is still open for writing is saved
void vfs_sync_open(void) {
  pthread_mutex_lock(&vfs_lock);
  for (VFile *f = open_writes; f; f = f->next_open) { Node *n = persistent(f->path) ? node(f->path) : 0; if (n && !n->gone && !n->dir) host_store_save(f->path, (u32)strlen(f->path), n->data, (u32)n->size); }
  pthread_mutex_unlock(&vfs_lock);
}
s64 vfs_read(VFile *f, void *dst, u64 n) {
  pthread_mutex_lock(&vfs_lock);
  Node *nd = node(f->path); s64 got = 0;
  if (nd && !nd->gone) { if (f->pos < nd->size) { got = (s64)((nd->size - f->pos) < n ? nd->size - f->pos : n); memcpy(dst, nd->data + f->pos, (size_t)got); } }
  else { char gb[1100]; const char *g = game_rel(f->path, gb, sizeof gb); if (g) got = host_read_file(g, (u32)strlen(g), f->pos, n, (u64)(uptr)dst); }
  pthread_mutex_unlock(&vfs_lock);
  if ((u64)got < n && rt_trace) rt_log("vfs: read of %llu at %llu from %s gave %lld (%s)", n, f->pos, f->path, got, nd ? "overlay" : "host");
  f->pos += (u64)got;
  return got;
}
s64 vfs_write(VFile *f, const void *src, u64 n) {
  pthread_mutex_lock(&vfs_lock);
  Node *nd = writable(f->path);
  if (f->pos + n > nd->cap) { u64 c = (f->pos + n) * 2; nd->data = realloc(nd->data, c); nd->cap = c; }
  if (f->pos > nd->size) memset(nd->data + nd->size, 0, f->pos - nd->size);
  memcpy(nd->data + f->pos, src, n);
  f->pos += n; if (f->pos > nd->size) nd->size = f->pos;
  nd->mtime = now_ft();
  pthread_mutex_unlock(&vfs_lock);
  return (s64)n;
}
u64 vfs_size(VFile *f) { u64 s = 0; pthread_mutex_lock(&vfs_lock); kind(f->path, &s); pthread_mutex_unlock(&vfs_lock); return s; }
u64 *vfs_pos(VFile *f) { return &f->pos; }
const char *vfs_file_path(VFile *f) { return f->path; }
void vfs_truncate(VFile *f, u64 size) {
  pthread_mutex_lock(&vfs_lock);
  Node *nd = writable(f->path);
  if (size > nd->cap) { nd->data = realloc(nd->data, size); nd->cap = size; }
  if (size > nd->size) memset(nd->data + nd->size, 0, size - nd->size);
  nd->size = size;
  pthread_mutex_unlock(&vfs_lock);
}
u64 vfs_mtime(VFile *f) { Node *n = node(f->path); return n ? n->mtime : 132000000000000000ull; }

// ---- names ----
u32 vfs_stat(const char *guest, u64 *size) {
  char p[1024]; if (!vfs_path(guest, p, sizeof p)) return 0;
  pthread_mutex_lock(&vfs_lock); int k = kind(p, size); pthread_mutex_unlock(&vfs_lock);
  return (u32)k;
}
u32 vfs_mkdir(const char *guest) {
  char p[1024]; if (!vfs_path(guest, p, sizeof p)) return 3;
  pthread_mutex_lock(&vfs_lock);
  u32 e = 0;
  if (kind(p, 0)) e = 183; else if (!parent_exists(p)) e = 3;
  else { Node *n = node_new(p); n->gone = 0; n->dir = 1; n->mtime = now_ft(); }
  pthread_mutex_unlock(&vfs_lock);
  return e;
}
u32 vfs_remove(const char *guest, int dir) {
  char p[1024]; if (!vfs_path(guest, p, sizeof p)) return 3;
  pthread_mutex_lock(&vfs_lock);
  int k = kind(p, 0); u32 e = 0;
  if (!k) e = 2; else if ((k == 2) != dir) e = dir ? 267 : 5;
  else { Node *n = node_new(p); n->gone = 1; n->size = 0; if (persistent(p) && !dir) host_store_remove(p, (u32)strlen(p)); }
  pthread_mutex_unlock(&vfs_lock);
  return e;
}
u32 vfs_rename(const char *from_g, const char *to_g, int replace) {
  char a[1024], b[1024]; if (!vfs_path(from_g, a, sizeof a) || !vfs_path(to_g, b, sizeof b)) return 3;
  pthread_mutex_lock(&vfs_lock);
  u64 size; int ka = kind(a, &size), kb = kind(b, 0); u32 e = 0;
  if (!ka) e = 2; else if (kb && !replace) e = 183; else if (!parent_exists(b)) e = 3;
  else if (ka == 2) { Node *n = node_new(b); n->gone = 0; n->dir = 1; Node *o = node_new(a); o->gone = 1; }
  else {
    Node *src = writable(a);
    Node *dst = node_new(b); free(dst->data);
    dst->data = src->data; dst->size = src->size; dst->cap = src->cap; dst->dir = 0; dst->gone = 0; dst->mtime = src->mtime;
    src->data = 0; src->size = src->cap = 0; src->gone = 1;
    if (persistent(a)) host_store_remove(a, (u32)strlen(a));
    if (persistent(b)) host_store_save(b, (u32)strlen(b), dst->data, (u32)dst->size);
  }
  pthread_mutex_unlock(&vfs_lock);
  return e;
}
// a directory's entries: the folder's merged with the overlay; `cb` gets each name and whether it is a directory
void vfs_list(const char *guest, void (*cb)(void *, const char *, int, u64), void *ctx) {
  char p[1024]; if (!vfs_path(guest, p, sizeof p)) return;
  pthread_mutex_lock(&vfs_lock);
  store_load();
  size_t pl = strlen(p);
  char **seen = 0; int nseen = 0;
  // overlay entries directly below p
  for (int b = 0; b < BUCKETS; b++) for (Node *n = nodes[b]; n; n = n->next) {
    if (strncmp(n->path, p, pl) || n->path[pl] != '/' || strchr(n->path + pl + 1, '/')) continue;
    seen = realloc(seen, sizeof(char *) * (size_t)(nseen + 1)); seen[nseen++] = n->path + pl + 1;
    if (!n->gone) cb(ctx, n->path + pl + 1, n->dir, n->size);
  }
  char gb[1100]; const char *g = game_rel(p, gb, sizeof gb);
  if (g) {
    u32 need = host_list(g, (u32)strlen(g), 0, 0);
    if (need) {
      char *buf = malloc(need + 1); host_list(g, (u32)strlen(g), buf, need); buf[need] = 0;
      for (char *line = strtok(buf, "\n"); line; line = strtok(0, "\n")) {
        size_t l = strlen(line); int dir = l && line[l - 1] == '/'; if (dir) line[--l] = 0;
        char low[512]; snprintf(low, sizeof low, "%s", line); for (char *q = low; *q; q++) *q = (char)tolower((u8)*q);
        int dup = 0; for (int i = 0; i < nseen && !dup; i++) dup = !strcmp(seen[i], low);
        if (dup) continue;
        char full[1024]; snprintf(full, sizeof full, "%s/%s", g, line);
        s64 s = dir ? 0 : host_stat(full, (u32)strlen(full));
        cb(ctx, line, dir, s > 0 ? (u64)s : 0);
      }
      free(buf);
    }
  }
  free(seen);
  pthread_mutex_unlock(&vfs_lock);
}
