// SPDX-License-Identifier: GPL-2.0-or-later
// What the core runtime and the shared D3D12 front end call in the Xbox Series runtime's libraries, for a Windows
// x64 title, which does not link them: a desktop title has no graphics memory region (rt/xbox-series/xmem.c).
#include "../rt.h"

u64 rt_gfx_alloc(u64 size, u64 align, u32 window) { (void)size; (void)align; (void)window; return 0; } // only a console device asks
int rt_gfx_free(u64 addr) { (void)addr; return 0; }
// A desktop title's writes in its own folder are kept (in the saves folder, or the browser's storage), as on a PC
// where it was unpacked somewhere writable: Desire writes its maze to Assets/Maps at every start (rt/vfs.c)
const int vfs_writable_game = 1;
