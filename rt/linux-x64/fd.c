// SPDX-License-Identifier: GPL-2.0-or-later
// File descriptors: the three standard streams (the host's), files of the title's folder (read from the host as
// they are read), the runtime's generated files (their bytes made when they are opened) and directories.
#include <unistd.h>
#include "linux.h"

enum { F_FREE, F_STD, F_FILE, F_MEM, F_DIR, F_NULL, F_RANDOM };
typedef struct { int kind, std; u64 pos, size, ino; char *data; char *host; char *path; u32 flags; } Fd;
#define FDS 256
static Fd fds[FDS] = { { F_STD, 0 }, { F_STD, 1 }, { F_STD, 2 } };
static Fd *get(s64 fd) { return fd >= 0 && fd < FDS && fds[fd].kind ? &fds[fd] : 0; }
int fd_is_file(s64 fd) { Fd *f = get(fd); return f && (f->kind == F_FILE || f->kind == F_MEM); }
static s64 fd_new(s64 from) { for (s64 i = from < 0 ? 0 : from; i < FDS; i++) if (!fds[i].kind) return i; return -L_EMFILE; }

static s64 at_path(s64 dirfd, const char *path, char *abs, size_t cap) {
  if (path && path[0] != '/' && dirfd != -100) {   // relative to an open directory
    Fd *d = get(dirfd); if (!d) return -L_EBADF; if (d->kind != F_DIR) return -L_ENOTDIR;
    char joined[1024]; if (snprintf(joined, sizeof joined, "%s/%s", d->path, path) >= (int)sizeof joined) return -L_ENAMETOOLONG;
    return fs_path(-100, joined, abs, cap);
  }
  return fs_path(dirfd, path, abs, cap);
}

s64 sys_openat(s64 dirfd, const char *path, u64 flags) {
  enum { O_ACCMODE = 3, O_CREAT = 0100, O_EXCL = 0200, O_TRUNC = 01000, O_DIRECTORY = 0200000, O_NOFOLLOW = 0400000, O_PATH = 010000000 };
  char abs[1024]; s64 e = at_path(dirfd, path, abs, sizeof abs); if (e) return e;
  Node n; e = fs_lookup(abs, &n, !(flags & O_NOFOLLOW));
  if (e) return e == -L_ENOENT && (flags & O_CREAT) ? -L_EROFS : e;
  if (n.type == N_LINK) { free(n.data); return -L_ELOOP; }
  if ((flags & O_DIRECTORY) && n.type != N_DIR) { free(n.data); return -L_ENOTDIR; }
  int writes = (flags & O_ACCMODE) != 0 || (flags & O_TRUNC);
  if (writes && (n.type == N_FILE || n.type == N_DIR)) { free(n.data); return n.type == N_DIR ? -L_EISDIR : -L_EROFS; }
  s64 fd = fd_new(0); if (fd < 0) { free(n.data); return fd; }
  Fd *f = &fds[fd]; memset(f, 0, sizeof *f); f->ino = n.ino; f->size = n.size; f->flags = (u32)flags; f->path = strdup(abs);
  switch (n.type) {
  case N_DIR: f->kind = F_DIR; { u32 len = 0; f->data = fs_list(abs, &len); f->size = len; } break;
  case N_FILE: if (n.data) { f->kind = F_MEM; f->data = n.data; } else { f->kind = F_FILE; f->host = strdup(n.host); } break;
  case N_NULL: f->kind = F_NULL; break;
  case N_RANDOM: f->kind = F_RANDOM; break;
  case N_TTY: f->kind = F_STD; f->std = (flags & O_ACCMODE) ? 1 : 0; break;
  }
  return fd;
}
s64 sys_close(s64 fd) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  free(f->data); free(f->host); free(f->path); memset(f, 0, sizeof *f);
  return 0;
}
s64 sys_dup(s64 fd, s64 to) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  if (to < 0) to = fd_new(-to - 1); else if (to >= FDS) return -L_EBADF; else if (to == fd) return to; else sys_close(to);
  if (to < 0) return to;
  fds[to] = *f;
  if (f->data) { fds[to].data = malloc(f->size + 1); memcpy(fds[to].data, f->data, f->size + 1); }
  if (f->host) fds[to].host = strdup(f->host);
  if (f->path) fds[to].path = strdup(f->path);
  return to;
}

s64 sys_pread_fd(s64 fd, void *buf, u64 n, u64 off) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  if (f->kind == F_DIR) return -L_EISDIR;
  if (f->kind != F_FILE && f->kind != F_MEM) return -L_ESPIPE;
  if (off >= f->size) return 0;
  if (n > f->size - off) n = f->size - off;
  if (f->kind == F_MEM) { memcpy(buf, f->data + off, n); return (s64)n; }
  return (s64)host_read_file(f->host, (u32)strlen(f->host), off, n, (u64)(uptr)buf);
}
s64 sys_pread(s64 fd, void *buf, u64 n, u64 off) { return sys_pread_fd(fd, buf, n, off); }
s64 sys_read(s64 fd, void *buf, u64 n) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  switch (f->kind) {
  case F_STD: { if (f->std) return -L_EBADF; ssize_t k = read(0, buf, n); return k < 0 ? -L_EIO : k; }
  case F_NULL: return 0;
  case F_RANDOM: arc4random_buf(buf, n); return (s64)n;
  case F_DIR: return -L_EISDIR;
  default: { s64 k = sys_pread_fd(fd, buf, n, f->pos); if (k > 0) f->pos += (u64)k; return k; }
  }
}
s64 sys_write(s64 fd, const void *buf, u64 n) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  if (f->kind == F_NULL || f->kind == F_RANDOM) return (s64)n;
  if (f->kind != F_STD) return -L_EBADF;
  int to = f->std == 2 ? 2 : 1; u64 done = 0;
  while (done < n) { ssize_t k = write(to, (const char *)buf + done, n - done); if (k <= 0) return done ? (s64)done : -L_EIO; done += (u64)k; }
  return (s64)done;
}
s64 sys_readv(s64 fd, u64 iov, u64 n) {
  s64 total = 0;
  for (u64 i = 0; i < n; i++) { u64 len = M64(iov + 16 * i + 8); if (!len) continue; s64 k = sys_read(fd, (void *)GP(M64(iov + 16 * i)), len); if (k < 0) return total ? total : k; total += k; if ((u64)k < len) break; }
  return total;
}
s64 sys_writev(s64 fd, u64 iov, u64 n) {
  s64 total = 0;
  for (u64 i = 0; i < n; i++) { u64 len = M64(iov + 16 * i + 8); if (!len) continue; s64 k = sys_write(fd, (const void *)GP(M64(iov + 16 * i)), len); if (k < 0) return total ? total : k; total += k; if ((u64)k < len) break; }
  return total;
}
s64 sys_lseek(s64 fd, s64 off, u64 whence) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  if (f->kind == F_STD) return -L_ESPIPE;
  s64 to = whence == 0 ? off : whence == 1 ? (s64)f->pos + off : whence == 2 ? (s64)f->size + off : -1;
  if (whence > 2 || to < 0) return -L_EINVAL;
  f->pos = (u64)to; return to;
}

// ---- what a file is ----
typedef struct { u32 mode; u64 size, ino, rdev; } What;
static void what_node(const Node *n, What *w) {
  w->ino = n->ino; w->size = n->size; w->rdev = 0;
  w->mode = n->type == N_DIR ? 0040555 : n->type == N_LINK ? 0120777 : n->type == N_FILE ? 0100555 : 0020666;
  if (n->type == N_NULL) w->rdev = 0x103; else if (n->type == N_RANDOM) w->rdev = 0x109; else if (n->type == N_TTY) w->rdev = 0x500;
}
static int what_fd(s64 fd, What *w) {
  Fd *f = get(fd); if (!f) return -L_EBADF;
  w->ino = f->ino; w->size = f->size; w->rdev = 0;
  switch (f->kind) {
  case F_STD: w->mode = linux_tty ? 0020620 : 0010600; w->size = 0; w->ino = 3 + (u64)f->std; w->rdev = linux_tty ? 0x8800 : 0; break;   // a terminal, or a pipe
  case F_DIR: w->mode = 0040555; w->size = 0; break;
  case F_NULL: w->mode = 0020666; w->rdev = 0x103; break;
  case F_RANDOM: w->mode = 0020666; w->rdev = 0x109; break;
  default: w->mode = 0100555;
  }
  return 0;
}
static u64 file_time(void) { return (rt_now_us() - rt_uptime_us()) / 1000000; }   // every file is as old as the system
static void put_stat(u64 st, const What *w) {
  memset((void *)GP(st), 0, 144);
  M64(st) = 1; M64(st + 8) = w->ino; M64(st + 16) = 1; M32(st + 24) = w->mode; M32(st + 28) = 0; M32(st + 32) = 0; M64(st + 40) = w->rdev;
  M64(st + 48) = w->size; M64(st + 56) = 4096; M64(st + 64) = (w->size + 511) / 512;
  u64 t = file_time(); M64(st + 72) = t; M64(st + 88) = t; M64(st + 104) = t;
}
s64 sys_fstat(s64 fd, u64 st) { What w; int e = what_fd(fd, &w); if (e) return e; put_stat(st, &w); return 0; }
static s64 what_at(s64 dirfd, const char *path, u64 flags, What *w) {
  enum { AT_SYMLINK_NOFOLLOW = 0x100, AT_EMPTY_PATH = 0x1000 };
  if ((flags & AT_EMPTY_PATH) && path && !*path) return what_fd(dirfd, w);
  char abs[1024]; s64 e = at_path(dirfd, path, abs, sizeof abs); if (e) return e;
  Node n; e = fs_lookup(abs, &n, !(flags & AT_SYMLINK_NOFOLLOW)); if (e) return e;
  what_node(&n, w); free(n.data);
  return 0;
}
s64 sys_fstatat(s64 dirfd, const char *path, u64 st, u64 flags) { What w; s64 e = what_at(dirfd, path, flags, &w); if (e) return e; put_stat(st, &w); return 0; }
s64 sys_statx(s64 dirfd, const char *path, u64 flags, u64 mask, u64 x) {
  (void)mask; What w; s64 e = what_at(dirfd, path, flags, &w); if (e) return e;
  memset((void *)GP(x), 0, 256);
  M32(x) = 0x7FF; M32(x + 4) = 4096; M32(x + 16) = 1; M16(x + 28) = (u16)w.mode; M64(x + 32) = w.ino; M64(x + 40) = w.size; M64(x + 48) = (w.size + 511) / 512;
  u64 t = file_time(); M64(x + 64) = t; M64(x + 80) = t; M64(x + 96) = t; M64(x + 112) = t;
  M32(x + 128) = (u32)(w.rdev >> 8); M32(x + 132) = (u32)(w.rdev & 255); M32(x + 140) = 1;
  return 0;
}
s64 sys_faccessat(s64 dirfd, const char *path, u64 mode) {
  What w; s64 e = what_at(dirfd, path, 0, &w); if (e) return e;
  return (mode & 2) && (w.mode & 0170000) != 0020000 ? -L_EROFS : 0;   // nothing but a device can be written
}
s64 sys_readlinkat(s64 dirfd, const char *path, char *buf, u64 n) {
  char abs[1024]; s64 e = at_path(dirfd, path, abs, sizeof abs); if (e) return e;
  Node node; e = fs_lookup(abs, &node, 0); if (e) return e;
  if (node.type != N_LINK) { free(node.data); return -L_EINVAL; }
  u64 l = strlen(node.data); if (l > n) l = n;
  memcpy(buf, node.data, l); free(node.data);
  return (s64)l;
}
s64 sys_getdents64(s64 fd, u64 buf, u64 n) {
  Fd *f = get(fd); if (!f) return -L_EBADF; if (f->kind != F_DIR) return -L_ENOTDIR;
  u64 at = 0;
  // "." and ".." first (positions 0 and 1 before the listing's bytes), then one entry a line
  while (f->pos < f->size + 2) {
    const char *name; u64 len, next; int dir = 1;
    if (f->pos == 0) { name = "."; len = 1; next = 1; } else if (f->pos == 1) { name = ".."; len = 2; next = 2; }
    else { name = f->data + f->pos - 2; const char *q = strchr(name, '\n'); len = q ? (u64)(q - name) : strlen(name); next = f->pos + len + (q != 0);
      dir = len && name[len - 1] == '/'; if (dir) len--; if (!len) { f->pos = next; continue; } }
    u64 rec = (19 + len + 1 + 7) & ~7ull;
    if (at + rec > n) { if (!at) return -L_EINVAL; break; }
    u64 d = buf + at; memset((void *)GP(d), 0, rec);
    M64(d) = f->ino + next; M64(d + 8) = next; M16(d + 16) = (u16)rec; M8(d + 18) = dir ? 4 : 8; memcpy((void *)GP(d + 19), name, len);
    at += rec; f->pos = next;
  }
  return (s64)at;
}

// ---- terminals, descriptor flags, waiting ----
s64 sys_ioctl(s64 fd, u64 req, u64 arg) {
  enum { TCGETS = 0x5401, TCSETS = 0x5402, TCSETSW = 0x5403, TCSETSF = 0x5404, TIOCGPGRP = 0x540F, TIOCGWINSZ = 0x5413, FIONREAD = 0x541B };
  Fd *f = get(fd); if (!f) return -L_EBADF;
  if (f->kind != F_STD || !linux_tty) return -L_ENOTTY;
  switch (req) {
  case TIOCGWINSZ: M16(arg) = linux_rows; M16(arg + 2) = linux_cols; M16(arg + 4) = 0; M16(arg + 6) = 0; return 0;
  case TCGETS: memset((void *)GP(arg), 0, 36); M32(arg) = 0x4500; M32(arg + 4) = 5; M32(arg + 8) = 0xBF; M32(arg + 12) = 0x8A3B; return 0;   // a cooked terminal that echoes
  case TCSETS: case TCSETSW: case TCSETSF: return 0;
  case TIOCGPGRP: M32(arg) = 1; return 0;
  case FIONREAD: M32(arg) = 0; return 0;
  }
  return -L_EINVAL;
}
s64 sys_fcntl(s64 fd, u64 cmd, u64 arg) {
  enum { F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, F_DUPFD_CLOEXEC = 1030 };
  Fd *f = get(fd); if (!f) return -L_EBADF;
  switch (cmd) {
  case F_DUPFD: case F_DUPFD_CLOEXEC: return sys_dup(fd, -(s64)arg - 1);
  case F_GETFD: case F_SETFD: case F_SETFL: return 0;
  case F_GETFL: return f->kind == F_STD ? (f->std ? 1 : 0) : (s64)(f->flags & 3);
  }
  return -L_EINVAL;
}
// poll: a file or a device can always be read; the terminal's input never has anything (no keyboard is wired to
// a title yet), so a wait for it lasts as long as it was asked to
s64 sys_poll(u64 list, u64 n, s64 ms) {
  s64 ready = 0;
  for (u64 i = 0; i < n; i++) {
    s32 fd = (s32)M32(list + 8 * i); u16 want = M16(list + 8 * i + 4), got = 0; Fd *f = get(fd);
    if (fd < 0) got = 0; else if (!f) got = 0x20;   // POLLNVAL
    else if (f->kind == F_STD) got = f->std ? (want & 4) : 0;   // POLLOUT
    else got = want & 5;
    M16(list + 8 * i + 6) = got; if (got) ready++;
  }
  if (!ready && ms > 0) { if (rt_clock_virtual()) rt_clock_advance((u64)ms * 1000); else usleep((useconds_t)ms * 1000); }
  return ready;
}
s64 sys_statfs(const char *path, u64 buf) {
  char abs[1024]; s64 e = fs_path(-100, path, abs, sizeof abs); if (e) return e;
  Node n; e = fs_lookup(abs, &n, 1); if (e) return e; free(n.data);
  memset((void *)GP(buf), 0, 120);
  int proc = !strncmp(abs, "/proc", 5), sys = !strncmp(abs, "/sys", 4);
  M64(buf) = proc ? 0x9FA0 : sys ? 0x62656572 : 0x73717368;   // proc, sysfs, or a read-only image (squashfs's number)
  M64(buf + 8) = 4096; M64(buf + 64) = 255; M64(buf + 72) = 4096; M64(buf + 80) = 1;   // ST_RDONLY
  return 0;
}
