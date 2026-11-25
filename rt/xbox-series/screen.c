// SPDX-License-Identifier: GPL-2.0-or-later
// The console's screen: the ntuser and DWM functions a title's window uses.
#include "../gdk/gdk.h"

// ---- the title's window: the console's screen, a hidden 1920x1080 frameless tool window ----
typedef struct { u32 magic; u32 style, exstyle; u64 proc; } Window;
static u64 make_window(Cpu *c) { (void)c; Window *w = calloc(1, sizeof *w); w->magic = 0x57494E44u; w->style = 0x90000000u; w->exstyle = 0x80; return (uptr)w; }
static Window *window(u64 h) { Window *w = (Window *)GP(h); return h && w->magic == 0x57494E44u ? w : 0; }
IMP(user32, RegisterClassW) { RET(0xC001); }
IMP(user32, RegisterClassExW) { RET(0xC001); }
IMP(user32, RegisterClassA) { RET(0xC001); }
IMP(user32, CreateWindowExW) { RET(make_window(c)); }
IMP(user32, CreateWindowExA) { RET(make_window(c)); }
IMP(user32, DestroyWindow) { RET(1); }
IMP(user32, ShowWindow) { RET(1); }
IMP(user32, UpdateWindow) { RET(1); }
IMP(user32, DefWindowProcW) { RET(0); }
IMP(user32, DefWindowProcA) { RET(0); }
IMP(user32, GetWindowLongW) { Window *w = window(ARG(0)); s32 i = (s32)ARG(1); RET(!w ? 0 : i == -16 ? w->style : i == -20 ? w->exstyle : 0); }
IMP(user32, SetWindowLongW) { RET(0); }
IMP(user32, LoadIconW) { RET(0x10001); }   // icons and cursors: handles nothing draws
IMP(user32, LoadIconA) { RET(0x10001); }
IMP(user32, LoadCursorW) { RET(0x10002); }
IMP(user32, LoadCursorA) { RET(0x10002); }
IMP(user32, AdjustWindowRect) { RET(1); }  // no frame around the console screen: the rect stays
IMP(user32, AdjustWindowRectEx) { RET(1); }
IMP(user32, GetClientRect) { u64 r = ARG(1); M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = 1920; M32(r + 12) = 1080; RET(1); }
IMP(user32, GetWindowRect) { u64 r = ARG(1); M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = 1920; M32(r + 12) = 1080; RET(1); }
IMP(user32, PeekMessageW) { RET(0); }
IMP(user32, GetMessageW) { RET(0); }
IMP(user32, TranslateMessage) { RET(0); }
IMP(user32, DispatchMessageW) { RET(0); }
IMP(user32, PostQuitMessage) {}
IMP(dwmapi, DwmGetWindowAttribute) { if ((u32)ARG(1) == 14 && ARG(3) >= 4) { M32(ARG(2)) = 1; RET(S_OK); return; } RET(E_INVALIDARG); } // DWMWA_CLOAKED
