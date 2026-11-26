// SPDX-License-Identifier: GPL-2.0-or-later
// Memory: mmap gives zeroed blocks of the module's own memory (there is one address space and no protection, so
// mprotect has nothing to do); a mapped file is a copy read once. The program break never moves: musl's malloc
// takes that as "no more" and uses mmap.
#include "linux.h"

typedef struct Map { u64 at, len; struct Map *next; } Map;
static Map *maps;
static Map **find(u64 at) { for (Map **p = &maps; *p; p = &(*p)->next) if ((*p)->at == at) return p; return 0; }
static Map *inside(u64 at, u64 len) { for (Map *m = maps; m; m = m->next) if (at >= m->at && at + len <= m->at + m->len) return m; return 0; }

s64 sys_brk(u64 to) { (void)to; return (s64)linux_image_end; }
s64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, s64 fd, u64 off) {
  enum { MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_FIXED_NOREPLACE = 0x100000 };
  (void)prot;
  if (!len) return -L_EINVAL;
  len = (len + 4095) & ~4095ull;
  if (!(flags & MAP_ANONYMOUS) && !fd_is_file(fd)) return -L_EBADF;
  u64 at;
  if (flags & MAP_FIXED) {   // only over memory a mapping already holds (a program replacing part of its own mapping)
    if (!inside(addr, len)) return -L_ENOMEM;
    at = addr; memset((void *)GP(at), 0, len);
  } else {
    if (flags & MAP_FIXED_NOREPLACE) return -L_EEXIST;
    at = rt_valloc(len, 4096); if (!at) return -L_ENOMEM;
    Map *m = malloc(sizeof *m); m->at = at; m->len = len; m->next = maps; maps = m;
  }
  if (!(flags & MAP_ANONYMOUS)) { s64 k = sys_pread_fd(fd, (void *)GP(at), len, off); if (k < 0) { if (!(flags & MAP_FIXED)) sys_munmap(at, len); return k; } }
  return (s64)at;
}
s64 sys_munmap(u64 addr, u64 len) {
  len = (len + 4095) & ~4095ull;
  Map **p = find(addr);
  if (p && (*p)->len == len) { Map *m = *p; *p = m->next; rt_vfree(m->at); free(m); return 0; }
  if (p && len < (*p)->len) return 0;   // part of a mapping: it stays until the whole of it goes
  return inside(addr, len) ? 0 : -L_EINVAL;
}
s64 sys_mremap(u64 addr, u64 old, u64 len, u64 flags, u64 to) {
  (void)to;
  old = (old + 4095) & ~4095ull; len = (len + 4095) & ~4095ull;
  Map **p = find(addr); if (!p || (*p)->len != old || !len) return -L_EINVAL;
  if (len <= old) return (s64)addr;   // (the tail is kept)
  if (!(flags & 1)) return -L_ENOMEM;   // MREMAP_MAYMOVE
  u64 at = rt_valloc(len, 4096); if (!at) return -L_ENOMEM;
  memcpy((void *)GP(at), (void *)GP(addr), old);
  rt_vfree(addr); (*p)->at = at; (*p)->len = len;
  return (s64)at;
}
