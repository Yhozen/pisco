// SPDX-License-Identifier: GPL-2.0-or-later
// gdi32 for Windows desktop titles. For now: objects that can be made, selected and deleted, so a title can
// register its window class. Drawing text into bitmaps (memory DCs, DIB sections, fonts) comes with the pixels.
#include "../rt.h"

// GDI handles are small numbers no pointer can be mistaken for; stock objects get their own range
static u32 next_object = 0x00100000;
IMP(gdi32, CreateSolidBrush) { RET(__atomic_add_fetch(&next_object, 4, __ATOMIC_SEQ_CST)); }
IMP(gdi32, GetStockObject) { RET(0x00080000u + ((u32)ARG(0) & 0xFF)); }
IMP(gdi32, DeleteObject) { RET(1); }
