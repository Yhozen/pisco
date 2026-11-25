// SPDX-License-Identifier: GPL-2.0-or-later
// kernel32's file functions over the title's file system (vfs.c), and the standard handles.
#include <ctype.h>
#include "rt.h"
#include "vfs.h"

int rt_file_write(int fd, const void *p, u32 n);
int rt_file_read(int fd, void *p, u32 n);
#define INVALID 0xFFFFFFFFFFFFFFFFull
#define ATTR_DIR 0x10
#define ATTR_NORMAL 0x80
static const u64 FT = 133000000000000000ull; // a fixed time for the title's folder

void rt_file_closed(Object *o) {
  if (!o->vf) return;
  if (o->delete_on_close) { char p[1024]; snprintf(p, sizeof p, "%s", vfs_file_path(o->vf)); vfs_close(o->vf); o->vf = 0;
    // vfs paths are already normalised: "mount/a/b"; turn them back into a guest path the VFS accepts
    char g[1100]; const char *slash = strchr(p, '/'); snprintf(g, sizeof g, "%s:\\%s", !strncmp(p, "game", 4) ? "G" : !strncmp(p, "pls", 3) ? "S" : !strncmp(p, "temp", 4) ? "T" : p, slash ? slash + 1 : "");
    vfs_remove(g, 0); return; }
  vfs_close(o->vf); o->vf = 0;
}
static char *name_w(u64 a) { return a ? rt_narrow(GWSTR(a)) : strdup(""); }
static Object *file_of(Cpu *c, u64 h) { Object *o = rt_handle_get(h); if (!o || o->type != OBJ_FILE) { SET_LAST_ERROR(6); return 0; } return o; }

// ---- standard handles ----
static u64 std_handle[3];
u64 rt_std_handle(int fd) {
  if (!std_handle[fd]) { Object *o = rt_object_new(OBJ_FILE); o->fd = fd; std_handle[fd] = rt_handle_new(o); }
  return std_handle[fd];
}
IMP(kernel32, GetStdHandle) { s32 n = (s32)ARG(0); int fd = n == -10 ? 0 : n == -11 ? 1 : n == -12 ? 2 : -1; RET(fd < 0 ? INVALID : rt_std_handle(fd)); }
IMP(kernel32, SetStdHandle) { RET(1); }
IMP(kernel32, GetConsoleOutputCP) { RET(65001); }
IMP(kernel32, WriteConsoleW) {
  wchar16 *w = malloc(2 * (ARG(2) + 1)); memcpy(w, GWSTR(ARG(1)), 2 * ARG(2)); w[ARG(2)] = 0;
  char *s = rt_narrow(w); Object *o = rt_handle_get(ARG(0)); rt_file_write(o && o->fd >= 0 ? o->fd : 1, s, (u32)strlen(s)); free(s); free(w);
  if (ARG(3)) M32(ARG(3)) = (u32)ARG(2);
  RET(1);
}
IMP(kernel32, WriteConsoleA) { Object *o = rt_handle_get(ARG(0)); rt_file_write(o && o->fd >= 0 ? o->fd : 1, GSTR(ARG(1)), (u32)ARG(2)); if (ARG(3)) M32(ARG(3)) = (u32)ARG(2); RET(1); }

// ---- open, read, write ----
static u64 open_file(Cpu *c, const char *name, u32 access, u32 disposition, u32 flags) {
  u32 err;
  VFile *f = vfs_open(name, access, disposition, &err);
  if (!f) { SET_LAST_ERROR(err); if (rt_trace) rt_log("CreateFile %s: error %u", name, err); return INVALID; }
  if (vfs_is_dir(f) && !(flags & 0x02000000)) { vfs_close(f); SET_LAST_ERROR(5); return INVALID; } // FILE_FLAG_BACKUP_SEMANTICS
  Object *o = rt_object_new(OBJ_FILE); o->vf = f;
  o->delete_on_close = (flags & 0x04000000) != 0; // FILE_FLAG_DELETE_ON_CLOSE
  SET_LAST_ERROR(err);
  static int files = -1; if (files < 0) { const char *e = getenv("SW_FILES"); files = e && *e == '1'; }
  if (files) rt_log("file %s", name);   // SW_FILES=1: every file a title opens, to set against Wine's +file trace
  return rt_handle_new(o);
}
IMP(kernel32, CreateFileW) { char *n = name_w(ARG(0)); RET(open_file(c, n, (u32)ARG(1), (u32)ARG(4), (u32)ARG(5))); free(n); }
IMP(kernel32, CreateFileA) { RET(open_file(c, GSTR(ARG(0)), (u32)ARG(1), (u32)ARG(4), (u32)ARG(5))); }
IMP(kernel32, CreateFile2) { // (name, access, share, disposition, CREATEFILE2_EXTENDED_PARAMETERS*)
  u32 flags = ARG(4) ? M32(ARG(4) + 8) | M32(ARG(4) + 12) : 0;
  char *n = name_w(ARG(0)); RET(open_file(c, n, (u32)ARG(1), (u32)ARG(3), flags)); free(n);
}
IMP(kernel32, ReadFile) {
  Object *o = file_of(c, ARG(0)); if (!o) { RET(0); return; }
  s64 n = o->vf ? vfs_read(o->vf, (void *)GP(ARG(1)), (u32)ARG(2)) : rt_file_read(o->fd, (void *)GP(ARG(1)), (u32)ARG(2));
  if (ARG(3)) M32(ARG(3)) = n < 0 ? 0 : (u32)n;
  RET(n >= 0);
}
IMP(kernel32, WriteFile) {
  Object *o = file_of(c, ARG(0)); if (!o) { RET(0); return; }
  s64 n = o->vf ? vfs_write(o->vf, (void *)GP(ARG(1)), (u32)ARG(2)) : rt_file_write(o->fd, (void *)GP(ARG(1)), (u32)ARG(2));
  if (ARG(3)) M32(ARG(3)) = n < 0 ? 0 : (u32)n;
  RET(n >= 0);
}
IMP(kernel32, FlushFileBuffers) { RET(1); }
IMP(kernel32, GetFileType) { Object *o = rt_handle_get(ARG(0)); RET(!o || o->type != OBJ_FILE ? 0 : o->vf ? 1 : 2); }
static int seek(Object *o, s64 d, u32 how, u64 *out) {
  if (!o->vf) return 0;
  u64 *pos = vfs_pos(o->vf); s64 base = how == 0 ? 0 : how == 1 ? (s64)*pos : (s64)vfs_size(o->vf);
  if (base + d < 0) return 0;
  *pos = (u64)(base + d); if (out) *out = *pos; return 1;
}
IMP(kernel32, SetFilePointerEx) {
  Object *o = file_of(c, ARG(0)); u64 np;
  if (!o || !seek(o, (s64)ARG(1), (u32)ARG(3), &np)) { SET_LAST_ERROR(131); RET(0); return; }
  if (ARG(2)) M64(ARG(2)) = np;
  RET(1);
}
IMP(kernel32, SetFilePointer) {
  Object *o = file_of(c, ARG(0)); u64 np;
  s64 d = ARG(2) ? (s64)(((u64)M32(ARG(2)) << 32) | (u32)ARG(1)) : (s64)(s32)ARG(1);
  if (!o || !seek(o, d, (u32)ARG(3), &np)) { SET_LAST_ERROR(131); RET(0xFFFFFFFF); return; }
  if (ARG(2)) M32(ARG(2)) = (u32)(np >> 32);
  SET_LAST_ERROR(0); RET((u32)np);
}
IMP(kernel32, GetFileSizeEx) { Object *o = file_of(c, ARG(0)); if (!o || !o->vf) { RET(0); return; } M64(ARG(1)) = vfs_size(o->vf); RET(1); }
IMP(kernel32, GetFileSize) { Object *o = file_of(c, ARG(0)); u64 s = o && o->vf ? vfs_size(o->vf) : 0; if (ARG(1)) M32(ARG(1)) = (u32)(s >> 32); RET((u32)s); }
IMP(kernel32, SetEndOfFile) { Object *o = file_of(c, ARG(0)); if (!o || !o->vf) { RET(0); return; } vfs_truncate(o->vf, *vfs_pos(o->vf)); RET(1); }
IMP(kernel32, SetFileTime) { RET(1); }
static u32 attrs_of(Object *o) { return o->vf && vfs_is_dir(o->vf) ? ATTR_DIR : ATTR_NORMAL; }
IMP(kernel32, GetFileInformationByHandle) {
  Object *o = file_of(c, ARG(0)); if (!o) { RET(0); return; }
  u64 b = ARG(1), size = o->vf ? vfs_size(o->vf) : 0, t = o->vf ? vfs_mtime(o->vf) : FT;
  memset((void *)GP(b), 0, 52);
  M32(b) = attrs_of(o); M64(b + 4) = t; M64(b + 12) = t; M64(b + 20) = t; M32(b + 28) = 0x5345524Bu;
  M32(b + 32) = (u32)(size >> 32); M32(b + 36) = (u32)size; M32(b + 40) = 1;
  M32(b + 48) = o->vf ? (u32)(uptr)o->vf : 0;
  RET(1);
}
IMP(kernel32, GetFileInformationByHandleEx) {
  Object *o = file_of(c, ARG(0)); if (!o) { RET(0); return; }
  u32 cls = (u32)ARG(1); u64 b = ARG(2), size = o->vf ? vfs_size(o->vf) : 0, t = o->vf ? vfs_mtime(o->vf) : FT;
  switch (cls) {
  case 0: memset((void *)GP(b), 0, 40); M64(b) = t; M64(b + 8) = t; M64(b + 16) = t; M64(b + 24) = t; M32(b + 32) = attrs_of(o); RET(1); return;          // FileBasicInfo
  case 1: memset((void *)GP(b), 0, 24); M64(b) = (size + 4095) & ~4095ull; M64(b + 8) = size; M32(b + 16) = 1; M8(b + 21) = attrs_of(o) == ATTR_DIR; RET(1); return; // FileStandardInfo
  case 9: M32(b) = attrs_of(o); M32(b + 4) = 0; RET(1); return;                                                                                             // FileAttributeTagInfo
  default: SET_LAST_ERROR(87); RET(0);
  }
}
IMP(kernel32, SetFileInformationByHandle) {
  Object *o = file_of(c, ARG(0)); if (!o || !o->vf) { RET(0); return; }
  u32 cls = (u32)ARG(1); u64 b = ARG(2);
  switch (cls) {
  case 6: vfs_truncate(o->vf, M64(b)); RET(1); return;                         // FileEndOfFileInfo
  case 5: RET(1); return;                                                        // FileAllocationInfo
  case 0: RET(1); return;                                                        // FileBasicInfo
  case 4: o->delete_on_close = M8(b) != 0; RET(1); return;                      // FileDispositionInfo
  case 21: o->delete_on_close = (M32(b) & 1) != 0; RET(1); return;              // FileDispositionInfoEx
  case 3: case 22: {                                                             // FileRenameInfo(Ex)
    int replace = cls == 3 ? M8(b) != 0 : (M32(b) & 1) != 0;
    u32 len = M32(b + 16) / 2; wchar16 *w = calloc(len + 1, 2); memcpy(w, (void *)GP(b + 20), 2ull * len);
    char *to = rt_narrow(w); free(w);
    char from[1100]; const char *p = vfs_file_path(o->vf), *slash = strchr(p, '/');
    snprintf(from, sizeof from, "%s:\\%s", !strncmp(p, "game", 4) ? "G" : !strncmp(p, "pls", 3) ? "S" : !strncmp(p, "temp", 4) ? "T" : p, slash ? slash + 1 : "");
    u32 e = vfs_rename(from, to, replace); free(to);
    if (e) { SET_LAST_ERROR(e); RET(0); } else RET(1);
    return;
  }
  default: SET_LAST_ERROR(87); RET(0);
  }
}

// ---- names ----
static void attr_data(u64 b, u32 kind, u64 size) {
  memset((void *)GP(b), 0, 36); M32(b) = kind == 2 ? ATTR_DIR : ATTR_NORMAL;
  M64(b + 4) = FT; M64(b + 12) = FT; M64(b + 20) = FT; M32(b + 28) = (u32)(size >> 32); M32(b + 32) = (u32)size;
}
static u32 attributes(Cpu *c, const char *n) { u64 s; u32 k = vfs_stat(n, &s); if (!k) { SET_LAST_ERROR(2); return 0xFFFFFFFF; } return k == 2 ? ATTR_DIR : ATTR_NORMAL; }
IMP(kernel32, GetFileAttributesW) { char *n = name_w(ARG(0)); RET(attributes(c, n)); free(n); }
IMP(kernel32, GetFileAttributesA) { RET(attributes(c, GSTR(ARG(0)))); }
IMP(kernel32, GetFileAttributesExW) {
  char *n = name_w(ARG(0)); u64 s = 0; u32 k = vfs_stat(n, &s); free(n);
  if (!k) { SET_LAST_ERROR(2); RET(0); return; }
  attr_data(ARG(2), k, s); RET(1);
}
IMP(kernel32, SetFileAttributesW) { RET(1); }
static void result(Cpu *c, u32 e) { if (e && e != 183) { SET_LAST_ERROR(e); RET(0); } else if (e == 183) { SET_LAST_ERROR(183); RET(0); } else RET(1); }
IMP(kernel32, CreateDirectoryW) { char *n = name_w(ARG(0)); result(c, vfs_mkdir(n)); free(n); }
IMP(kernel32, CreateDirectoryA) { result(c, vfs_mkdir(GSTR(ARG(0)))); }
IMP(kernel32, RemoveDirectoryW) { char *n = name_w(ARG(0)); result(c, vfs_remove(n, 1)); free(n); }
IMP(kernel32, DeleteFileW) { char *n = name_w(ARG(0)); result(c, vfs_remove(n, 0)); free(n); }
IMP(kernel32, DeleteFileA) { result(c, vfs_remove(GSTR(ARG(0)), 0)); }
IMP(kernel32, MoveFileExW) { char *a = name_w(ARG(0)), *b = name_w(ARG(1)); result(c, vfs_rename(a, b, (ARG(2) & 1) != 0)); free(a); free(b); }
IMP(kernel32, MoveFileW) { char *a = name_w(ARG(0)), *b = name_w(ARG(1)); result(c, vfs_rename(a, b, 0)); free(a); free(b); }
IMP(kernel32, CopyFileW) {
  char *a = name_w(ARG(0)), *b = name_w(ARG(1)); u32 e;
  VFile *s = vfs_open(a, 0x80000000u, 3, &e), *d = s ? vfs_open(b, 0x40000000u, ARG(2) ? 1 : 2, &e) : 0;
  if (s && d) { u8 buf[65536]; s64 n; while ((n = vfs_read(s, buf, sizeof buf)) > 0) vfs_write(d, buf, (u64)n); e = 0; }
  if (s) vfs_close(s); if (d) vfs_close(d);
  result(c, s && d ? 0 : e); free(a); free(b);
}
IMP(kernel32, GetFullPathNameW) {
  char *n = name_w(ARG(0)); char full[1100];
  if (strchr(n, ':') || (n[0] == '\\' && n[1] == '\\')) snprintf(full, sizeof full, "%s", n); else snprintf(full, sizeof full, "C:\\title\\%s", n);
  for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
  u32 len = (u32)strlen(full), cap = (u32)ARG(1);
  if (len + 1 > cap) { RET(len + 1); free(n); return; }
  for (u32 i = 0; i <= len; i++) GWSTR(ARG(2))[i] = (u8)full[i];
  if (ARG(3)) { char *last = strrchr(full, '\\'); M64(ARG(3)) = last ? ARG(2) + 2ull * (u64)(last + 1 - full) : 0; }
  free(n); RET(len);
}
IMP(kernel32, GetCurrentDirectoryW) { const char *d = "C:\\title"; u32 l = (u32)strlen(d); if (ARG(0) <= l) { RET(l + 1); return; } for (u32 i = 0; i <= l; i++) GWSTR(ARG(1))[i] = (u8)d[i]; RET(l); }
IMP(kernel32, SetCurrentDirectoryW) { RET(1); }
IMP(kernel32, GetTempPathW) { const char *d = "T:\\"; u32 l = 3; if (ARG(0) <= l) { RET(l + 1); return; } for (u32 i = 0; i <= l; i++) GWSTR(ARG(1))[i] = (u8)d[i]; RET(l); }
IMP(kernel32, GetDiskFreeSpaceExW) {
  u64 total = 64ull << 30, avail = 48ull << 30;
  if (ARG(1)) M64(ARG(1)) = avail; if (ARG(2)) M64(ARG(2)) = total; if (ARG(3)) M64(ARG(3)) = avail; RET(1);
}
IMP(kernel32, GetDiskFreeSpaceW) {
  if (ARG(1)) M32(ARG(1)) = 8; if (ARG(2)) M32(ARG(2)) = 512; if (ARG(3)) M32(ARG(3)) = 12582912; if (ARG(4)) M32(ARG(4)) = 16777216; RET(1);
}
IMP(kernel32, GetVolumeInformationW) { if (ARG(4)) M32(ARG(4)) = 0x5345524Bu; if (ARG(5)) M32(ARG(5)) = 255; if (ARG(6)) M32(ARG(6)) = 0x3; RET(1); }

// ---- directory listings: FindFirstFile / FindNextFile ----
typedef struct { char name[260]; int dir; u64 size; } Entry;
typedef struct { Entry *e; int n, cap, at; char pattern[260]; } Find;
static int wild(const char *p, const char *s) {
  if (!*p) return !*s;
  if (*p == '*') { for (; ; s++) { if (wild(p + 1, s)) return 1; if (!*s) return 0; } }
  if (!*s) return 0;
  return (*p == '?' || tolower((u8)*p) == tolower((u8)*s)) && wild(p + 1, s + 1);
}
static void collect(void *ctx, const char *name, int dir, u64 size) {
  Find *f = ctx;
  if (!wild(f->pattern, name)) return;
  if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 32; f->e = realloc(f->e, sizeof(Entry) * (size_t)f->cap); }
  snprintf(f->e[f->n].name, 260, "%s", name); f->e[f->n].dir = dir; f->e[f->n].size = size; f->n++;
}
static void fill(u64 b, const Entry *e) {
  memset((void *)GP(b), 0, 592);
  M32(b) = e->dir ? ATTR_DIR : ATTR_NORMAL; M64(b + 4) = FT; M64(b + 12) = FT; M64(b + 20) = FT;
  M32(b + 28) = (u32)(e->size >> 32); M32(b + 32) = (u32)e->size;
  for (int i = 0; e->name[i] && i < 259; i++) M16(b + 44 + 2ull * (u64)i) = (u8)e->name[i];
}
static u64 find_first(Cpu *c, const char *spec, u64 out) {
  char dir[1100]; snprintf(dir, sizeof dir, "%s", spec);
  char *sep = strrchr(dir, '\\'); char *sep2 = strrchr(dir, '/'); if (sep2 > sep) sep = sep2;
  Find *f = calloc(1, sizeof *f);
  if (sep) { snprintf(f->pattern, sizeof f->pattern, "%s", sep + 1); sep[1] = 0; } else { snprintf(f->pattern, sizeof f->pattern, "%s", dir); dir[0] = 0; }
  if (!strcmp(f->pattern, "*") || !strcmp(f->pattern, "*.*")) {
    strcpy(f->pattern, "*");
    if (vfs_stat(dir[0] ? dir : ".", 0) != 2) { free(f); SET_LAST_ERROR(3); return INVALID; }
    collect(f, ".", 1, 0); collect(f, "..", 1, 0);
  }
  vfs_list(dir[0] ? dir : ".", collect, f);
  if (!f->n) { free(f->e); free(f); SET_LAST_ERROR(2); return INVALID; }
  fill(out, &f->e[0]); f->at = 1;
  Object *o = rt_object_new(OBJ_FILE); o->fd = -2; o->vf = 0; o->view = (u64)(uptr)f;
  return rt_handle_new(o);
}
IMP(kernel32, FindFirstFileExW) { char *n = name_w(ARG(0)); RET(find_first(c, n, ARG(2))); free(n); }
IMP(kernel32, FindFirstFileW) { char *n = name_w(ARG(0)); RET(find_first(c, n, ARG(1))); free(n); }
IMP(kernel32, FindNextFileW) {
  Object *o = rt_handle_get(ARG(0)); Find *f = o ? (Find *)GP(o->view) : 0;
  if (!f || f->at >= f->n) { SET_LAST_ERROR(18); RET(0); return; } // ERROR_NO_MORE_FILES
  fill(ARG(1), &f->e[f->at++]); RET(1);
}
// the ANSI forms: WIN32_FIND_DATAA has the same head, then the name as bytes at +44
static void fill_a(u64 b, const Entry *e) {
  memset((void *)GP(b), 0, 320);
  M32(b) = e->dir ? ATTR_DIR : ATTR_NORMAL; M64(b + 4) = FT; M64(b + 12) = FT; M64(b + 20) = FT;
  M32(b + 28) = (u32)(e->size >> 32); M32(b + 32) = (u32)e->size;
  strncpy(GSTR(b + 44), e->name, 259);
}
IMP(kernel32, FindFirstFileA) {
  u64 out = ARG(1); u64 tmp = (uptr)malloc(592);
  u64 h = find_first(c, GSTR(ARG(0)), tmp);
  if (h != INVALID) { Object *o = rt_handle_get(h); Find *f = (Find *)GP(o->view); fill_a(out, &f->e[f->at - 1]); }
  free((void *)GP(tmp)); RET(h);
}
IMP(kernel32, FindNextFileA) {
  Object *o = rt_handle_get(ARG(0)); Find *f = o ? (Find *)GP(o->view) : 0;
  if (!f || f->at >= f->n) { SET_LAST_ERROR(18); RET(0); return; } // ERROR_NO_MORE_FILES
  fill_a(ARG(1), &f->e[f->at++]); RET(1);
}
IMP(kernel32, FindClose) { Object *o = rt_handle_get(ARG(0)); if (o && o->view) { Find *f = (Find *)GP(o->view); free(f->e); free(f); o->view = 0; } rt_handle_close(ARG(0)); RET(1); }

// ---- ntdll's file calls (Rust's standard library uses them for synchronous I/O) ----
// (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock*, Buffer, Length, ByteOffset*, Key)
static void io_status(u64 iosb, u32 status, u64 n) { if (iosb) { M64(iosb) = status; M64(iosb + 8) = n; } }
IMP(ntdll, NtWriteFile) {
  Object *o = rt_handle_get(ARG(0)); u64 buf = M64(R[RSP] + 0x30), len = (u32)M64(R[RSP] + 0x38), off = M64(R[RSP] + 0x40), iosb = ARG(4);
  if (!o || o->type != OBJ_FILE) { RET(0xC0000008u); return; } // STATUS_INVALID_HANDLE
  if (off && o->vf) *vfs_pos(o->vf) = M64(off);
  s64 n = o->vf ? vfs_write(o->vf, (void *)GP(buf), len) : rt_file_write(o->fd, (void *)GP(buf), (u32)len);
  io_status(iosb, 0, n < 0 ? 0 : (u64)n); RET(0);
}
IMP(ntdll, NtReadFile) {
  Object *o = rt_handle_get(ARG(0)); u64 buf = M64(R[RSP] + 0x30), len = (u32)M64(R[RSP] + 0x38), off = M64(R[RSP] + 0x40), iosb = ARG(4);
  if (!o || o->type != OBJ_FILE) { RET(0xC0000008u); return; }
  if (off && o->vf) *vfs_pos(o->vf) = M64(off);
  s64 n = o->vf ? vfs_read(o->vf, (void *)GP(buf), len) : rt_file_read(o->fd, (void *)GP(buf), (u32)len);
  if (n <= 0 && len) { io_status(iosb, 0xC0000011u, 0); RET(0xC0000011u); return; } // STATUS_END_OF_FILE
  io_status(iosb, 0, (u64)n); RET(0);
}
IMP(ntdll, RtlNtStatusToDosError) { u32 s = (u32)ARG(0); RET(s == 0 ? 0 : s == 0xC0000011u ? 38 : s == 0xC0000034u ? 2 : s == 0xC0000008u ? 6 : 317); }
