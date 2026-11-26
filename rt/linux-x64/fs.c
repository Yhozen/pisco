// SPDX-License-Identifier: GPL-2.0-or-later
// The file system a Linux title sees, read-only. Its root is the title's folder or, for a title image, the image's
// drive (the host knows a file there as "c:/<path>", as it does for a Windows title: web/image.mjs): the host
// answers for the files there. /proc, /sys and /dev are the runtime's own (proc.c), and so are the files of /etc
// a program expects on any system when the title's folder has none.
#include "linux.h"

HOST("stat") s64 host_stat(const char *name, u32 len);                        // size, -1 missing, -2 directory
HOST("list") u32 host_list(const char *name, u32 len, char *buf, u32 cap);   // "name\n" lines, directories end in '/'

// The absolute, normal form of `path` ("." and ".." and doubled slashes gone). The working directory is always /.
int fs_path(s64 dirfd, const char *path, char *out, size_t cap) {
  (void)dirfd;   // a relative path is relative to /: no title is given another directory, and none can change it
  if (!path) return -L_EFAULT;
  if (!*path) return -L_ENOENT;
  size_t n = 0; out[n++] = '/';
  for (const char *p = path; *p;) {
    while (*p == '/') p++;
    const char *q = p; while (*q && *q != '/') q++;
    size_t l = (size_t)(q - p);
    if (l == 2 && p[0] == '.' && p[1] == '.') { while (n > 1 && out[n - 1] != '/') n--; if (n > 1) n--; }
    else if (l && !(l == 1 && p[0] == '.')) {
      if (n + l + 2 > cap) return -L_ENAMETOOLONG;
      if (n > 1) out[n++] = '/';
      memcpy(out + n, p, l); n += l;
    }
    p = q;
  }
  out[n] = 0;
  return 0;
}

static u64 ino_of(const char *abs) { u64 h = 1469598103934665603ull; for (; *abs; abs++) h = (h ^ (u8)*abs) * 1099511628211ull; return h | 1; }
static int runtime_only(const char *abs) {
  static const char *const own[] = { "/proc", "/sys", "/dev" };
  for (unsigned i = 0; i < 3; i++) { size_t l = strlen(own[i]); if (!strncmp(abs, own[i], l) && (!abs[l] || abs[l] == '/')) return 1; }
  return 0;
}

static int host_name(const char *abs, char out[512]) { return snprintf(out, 512, "%s%s", linux_drive ? "c:/" : "", abs + 1) < 512; }

int fs_lookup(const char *abs, Node *n, int follow) {
  for (int hops = 0; hops < 8; hops++) {
    memset(n, 0, sizeof *n); n->ino = ino_of(abs);
    if (!runtime_only(abs)) {
      char name[sizeof n->host]; if (!host_name(abs, name)) return -L_ENAMETOOLONG;
      s64 s = host_stat(name, (u32)strlen(name));
      if (!abs[1]) s = -2;   // the root is always there
      if (s == -2) { n->type = N_DIR; return 0; }
      if (s >= 0) { n->type = N_FILE; n->size = (u64)s; strcpy(n->host, name); return 0; }
    }
    if (fs_generated(abs, n)) {
      if (n->type != N_LINK || !follow) return 0;
      static char next[512]; int e = fs_path(-100, n->data, next, sizeof next); free(n->data); n->data = 0;
      if (e) return e;
      abs = next; continue;
    }
    if (fs_generated_dir(abs)) { n->type = N_DIR; return 0; }
    return -L_ENOENT;
  }
  return -L_ELOOP;
}

char *fs_list(const char *abs, u32 *len) {
  Node n; if (fs_lookup(abs, &n, 1)) return 0;
  if (n.type != N_DIR) { free(n.data); return 0; }
  char *out = 0; u32 have = 0;
  if (!runtime_only(abs)) {
    char name[sizeof n.host]; if (!host_name(abs, name)) return 0;
    have = host_list(name, (u32)strlen(name), 0, 0);
    out = malloc(have + 1);
    if (have) host_list(name, (u32)strlen(name), out, have);
    out[have] = 0;
  } else out = calloc(1, 1);
  fs_generated_list(abs, &out, &have);
  *len = have;
  return out;
}
