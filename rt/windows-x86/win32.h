// SPDX-License-Identifier: GPL-2.0-or-later
// What the parts of the Windows desktop layer (rt/windows-x86/) ask of one another.
#pragma once
#include "../rt.h"

// user32.c: the client area of a window; 0 if there is no such window
int win32_client_size(u32 hwnd, s32 *width, s32 *height);
// user32.c: the screen's size in the display mode it is in
void win32_screen_size(s32 *width, s32 *height);
// gdi32.c: FillRect's work (user32 exports the function)
int gdi_fill_rect(u32 hdc, const s32 *rect, u32 brush);

#include "../input.h"

