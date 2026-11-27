// SPDX-License-Identifier: GPL-2.0-or-later
// The file system a Linux title sees, read-only. Its root is the title's folder or, for a title image, the image's
// drive (the host knows a file there as "c:/<path>", as it does for a Windows title: web/image.mjs): the host
// answers for the files there. /proc, /sys and /dev are the runtime's own (proc.c), and so are the files of /etc
// a program expects on any system when the title's folder has none.
#include "linux.h"

HOST("stat") s64 host_stat(const char *name, u32 len);                        // size, -1 missing, -2 directory
HOST("list") u32 host_list(const char *name, u32 len, char *buf, u32 cap);   // "name\n" lines, directories end in '/'
// A Linux drive has symbolic links and executable bits (docs/spec/image.md#files): the target of the link at a name
// (its length; -1: not a link), and whether the file at a name is executable (1, 0; -1: the host does not know)
HOST("link") s32 host_link(const char *name, u32 len, char *buf, u32 cap);
HOST("exec") s32 host_exec(const char *name, u32 len);

char linux_cwd[1024] = "/";
// The absolute, normal form of `path` ("." and ".." and doubled slashes gone); a relative one is relative to the
// working directory (an open directory's path is joined to it before: fd.c)
int fs_path(s64 dirfd, const char *path, char *out, size_t cap) {
  (void)dirfd;
  if (!path) return -L_EFAULT;
  if (!*path) return -L_ENOENT;
  char joined[2048];
  if (path[0] != '/') { if (snprintf(joined, sizeof joined, "%s/%s", linux_cwd, path) >= (int)sizeof joined) return -L_ENAMETOOLONG; path = joined; }
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
      char target[512]; s32 tl = abs[1] ? host_link(name, (u32)strlen(name), target, sizeof target - 1) : -1;
      if (tl >= 0) {   // a link of the drive: relative to its folder
        target[tl] = 0; n->type = N_LINK;
        if (!follow) { n->data = strdup(target); return 0; }
        static char dir[1024], next[1024]; strcpy(dir, abs); char *slash = strrchr(dir, '/'); if (slash == dir) dir[1] = 0; else *slash = 0;
        if (target[0] == '/') { if (fs_path(-100, target, next, sizeof next)) return -L_ENAMETOOLONG; }
        else { char joined[1100]; if (snprintf(joined, sizeof joined, "%s/%s", dir, target) >= (int)sizeof joined) return -L_ENAMETOOLONG; if (fs_path(-100, joined, next, sizeof next)) return -L_ENAMETOOLONG; }
        abs = next; continue;
      }
      s64 s = host_stat(name, (u32)strlen(name));
      if (!abs[1]) s = -2;   // the root is always there
      if (s == -2) { n->type = N_DIR; return 0; }
      if (s >= 0) { n->type = N_FILE; n->size = (u64)s; strcpy(n->host, name); n->noexec = host_exec(name, (u32)strlen(name)) == 0; return 0; }
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
