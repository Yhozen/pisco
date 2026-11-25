// SPDX-License-Identifier: GPL-2.0-or-later
// user32 for Windows desktop titles: window classes, one top-level window (the host shows its client area),
// and the messages Windows sends a window as it is created, sized, shown and closed, delivered by calling the
// title's own window procedure. Input never arrives as messages here yet: the first title reads DirectInput.
#include "win32.h"

enum { WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
       WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46,
       WM_WINDOWPOSCHANGED = 0x47, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCACTIVATE = 0x86 };
enum { SCREEN_W = 1024, SCREEN_H = 768 };   // the desktop: what the oracle's has
// a captioned window's frame, as Windows XP's classic theme (and Wine) draws it: 3 px each side, a 26 px caption
enum { FRAME = 3, CAPTION = 26 };

typedef struct { char name[64]; u32 atom, proc, style, wnd_extra, instance, icon, cursor, brush; } WndClass;
typedef struct Window {
  u32 hwnd, proc, style, exstyle, instance, parent, userdata;
  s32 x, y, w, h;          // the whole window, in screen coordinates
  int visible, alive, has_frame;
  int needs_paint;         // its client area has not been painted since it was shown: WM_PAINT is due until someone validates it
  char title[128]; WndClass *cls;
} Window;
static WndClass classes[32]; static unsigned class_count;
static Window windows[16]; static unsigned window_count;
#define DESKTOP_HWND 0x00010010u
static int quit_posted; static u32 quit_code;

static Window *window_of(u64 hwnd) { for (unsigned i = 0; i < window_count; i++) if (windows[i].alive && windows[i].hwnd == (u32)hwnd) return &windows[i]; return 0; }
static WndClass *class_of(u64 name) {
  for (unsigned i = 0; i < class_count; i++) if (name < 0x10000 ? classes[i].atom == (u32)name : !strcasecmp(classes[i].name, GSTR(name))) return &classes[i];
  return 0;
}
static u64 send(Cpu *c, Window *w, u32 msg, u64 wp, u64 lp) { return w->proc ? rt_guest_call(c, w->proc, w->hwnd, msg, wp, lp) : 0; }
static void client_size(const Window *w, s32 *cw, s32 *ch) {
  *cw = w->w - (w->has_frame ? 2 * FRAME : 0); *ch = w->h - (w->has_frame ? 2 * FRAME + CAPTION : 0);
  if (*cw < 0) *cw = 0; if (*ch < 0) *ch = 0;
}
int win32_client_size(u32 hwnd, s32 *cw, s32 *ch) { Window *w = window_of(hwnd); if (!w) return 0; client_size(w, cw, ch); return 1; }
static void put_rect(u64 p, s32 l, s32 t, s32 r, s32 b) { M32(p) = (u32)l; M32(p + 4) = (u32)t; M32(p + 8) = (u32)r; M32(p + 12) = (u32)b; }

IMP(user32, LoadIconA) { RET(0x00020000u + ((u32)ARG(1) & 0xFFFF)); }
IMP(user32, LoadCursorA) { RET(0x00030000u + ((u32)ARG(1) & 0xFFFF)); }
// WNDCLASSA at `p`: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground,
// lpszMenuName, lpszClassName (WNDCLASSEXA is the same after its cbSize, with hIconSm at the end)
static u32 register_class(Cpu *c, u64 p) {
  if (class_count == 32 || !M32(p + 36)) { SET_LAST_ERROR(87); return 0; }
  if (class_of(M32(p + 36))) { SET_LAST_ERROR(1410); return 0; } // ERROR_CLASS_ALREADY_EXISTS
  WndClass *k = &classes[class_count]; memset(k, 0, sizeof *k);
  if (M32(p + 36) >= 0x10000) strncpy(k->name, GSTR(M32(p + 36)), sizeof k->name - 1);
  k->atom = 0xC000 + class_count; k->style = M32(p); k->proc = M32(p + 4); k->wnd_extra = M32(p + 12); k->instance = M32(p + 16);
  k->icon = M32(p + 20); k->cursor = M32(p + 24); k->brush = M32(p + 28);
  class_count++; return k->atom;
}
IMP(user32, RegisterClassA) { RET(register_class(c, ARG(0))); }
IMP(user32, RegisterClassExA) { u64 p = ARG(0); if (!p || M32(p) < 48) { SET_LAST_ERROR(87); RET(0); return; } RET(register_class(c, p + 4)); }
IMP(user32, UnregisterClassA) { WndClass *k = class_of(ARG(0)); if (k) k->name[0] = 0, k->atom = 0; RET(k != 0); }

// CreateWindowExA(exstyle, class, title, style, x, y, w, h, parent, menu, instance, param). The window procedure
// sees WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE and WM_CREATE before the call returns, as on Windows.
IMP(user32, CreateWindowExA) {
  WndClass *k = class_of(ARG(1));
  if (!k || window_count == 16) { SET_LAST_ERROR(1407); RET(0); return; } // ERROR_CANNOT_FIND_WND_CLASS
  Window *w = &windows[window_count]; memset(w, 0, sizeof *w);
  w->hwnd = 0x00010060u + 4 * window_count++; w->alive = 1; w->cls = k; w->proc = k->proc; w->instance = (u32)ARG(10);
  w->exstyle = (u32)ARG(0); w->style = (u32)ARG(3); w->parent = (u32)ARG(8);
  w->has_frame = (w->style & 0x00C00000) == 0x00C00000; // WS_CAPTION
  if (ARG(2)) strncpy(w->title, GSTR(ARG(2)), sizeof w->title - 1);
  s32 x = (s32)ARG(4), y = (s32)ARG(5), cx = (s32)ARG(6), cy = (s32)ARG(7);
  if (x == (s32)0x80000000) x = y = 0;                    // CW_USEDEFAULT
  if (cx == (s32)0x80000000) { cx = SCREEN_W * 3 / 4; cy = SCREEN_H * 3 / 4; }
  w->x = x; w->y = y; w->w = cx; w->h = cy;
  u64 mm = (uptr)calloc(1, 40 + 48 + 16), cs = mm + 40, rc = cs + 48;
  // MINMAXINFO: reserved, max size, max position, min track, max track
  M32(mm + 8) = SCREEN_W; M32(mm + 12) = SCREEN_H; M32(mm + 24) = 112; M32(mm + 28) = 27; M32(mm + 32) = SCREEN_W; M32(mm + 36) = SCREEN_H;
  // CREATESTRUCTA: lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x, style, lpszName, lpszClass, dwExStyle
  M32(cs) = (u32)ARG(11); M32(cs + 4) = w->instance; M32(cs + 8) = (u32)ARG(9); M32(cs + 12) = w->parent; M32(cs + 16) = (u32)cy; M32(cs + 20) = (u32)cx;
  M32(cs + 24) = (u32)y; M32(cs + 28) = (u32)x; M32(cs + 32) = w->style; M32(cs + 36) = (u32)ARG(2); M32(cs + 40) = (u32)ARG(1); M32(cs + 44) = w->exstyle;
  put_rect(rc, x, y, x + cx, y + cy);
  send(c, w, WM_GETMINMAXINFO, 0, mm);
  if (!send(c, w, WM_NCCREATE, 0, cs)) { w->alive = 0; free((void *)GP(mm)); RET(0); return; }
  send(c, w, WM_NCCALCSIZE, 0, rc);
  if ((s32)send(c, w, WM_CREATE, 0, cs) == -1) { w->alive = 0; free((void *)GP(mm)); RET(0); return; }
  free((void *)GP(mm));
  if (rt_trace) rt_log("window 0x%x \"%s\" class %s %dx%d style 0x%x", w->hwnd, w->title, k->name, cx, cy, w->style);
  if (w->style & 0x10000000) w->visible = 1; // WS_VISIBLE
  RET(w->hwnd);
}
// What a window procedure passes on. WM_NCCREATE must answer TRUE for the window to be created; WM_CLOSE destroys.
static void destroy(Cpu *c, Window *w) { if (!w->alive) return; send(c, w, WM_DESTROY, 0, 0); send(c, w, WM_NCDESTROY, 0, 0); w->alive = 0; }
IMP(user32, DefWindowProcA) {
  u32 msg = (u32)ARG(1); Window *w = window_of(ARG(0));
  switch (msg) {
  case WM_NCCREATE: case WM_ERASEBKGND: case WM_NCACTIVATE: RET(1); return;
  case WM_PAINT: if (w) w->needs_paint = 0; RET(0); return;   // BeginPaint and EndPaint: the client area is valid again
  case WM_CLOSE: if (w) destroy(c, w); RET(0); return;
  default: RET(0); return;
  }
}
IMP(user32, IsWindow) { RET(window_of(ARG(0)) != 0 || (u32)ARG(0) == DESKTOP_HWND); }
IMP(user32, GetDesktopWindow) { RET(DESKTOP_HWND); }
IMP(user32, GetClientRect) {
  Window *w = window_of(ARG(0)); s32 cw = SCREEN_W, ch = SCREEN_H;
  if (!w && (u32)ARG(0) != DESKTOP_HWND) { SET_LAST_ERROR(1400); RET(0); return; } // ERROR_INVALID_WINDOW_HANDLE
  if (w) client_size(w, &cw, &ch);
  put_rect(ARG(1), 0, 0, cw, ch); RET(1);
}
IMP(user32, GetWindowRect) {
  Window *w = window_of(ARG(0));
  if (!w && (u32)ARG(0) != DESKTOP_HWND) { SET_LAST_ERROR(1400); RET(0); return; }
  if (w) put_rect(ARG(1), w->x, w->y, w->x + w->w, w->y + w->h); else put_rect(ARG(1), 0, 0, SCREEN_W, SCREEN_H);
  RET(1);
}
// SetWindowPos(hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_SHOWWINDOW 0x40, SWP_HIDEWINDOW 0x80
static void moved_or_sized(Cpu *c, Window *w, s32 x, s32 y, s32 cx, s32 cy, u32 flags) {
  u64 wp = (uptr)calloc(1, 28 + 40 + 16), mm = wp + 28, rc = mm + 40; // WINDOWPOS: hwnd, hwndInsertAfter, x, y, cx, cy, flags
  M32(wp) = w->hwnd; M32(wp + 8) = (u32)x; M32(wp + 12) = (u32)y; M32(wp + 16) = (u32)cx; M32(wp + 20) = (u32)cy; M32(wp + 24) = flags;
  send(c, w, WM_WINDOWPOSCHANGING, 0, wp);
  int sized = cx != w->w || cy != w->h, moved = x != w->x || y != w->y;
  if (sized) {
    M32(mm + 8) = SCREEN_W; M32(mm + 12) = SCREEN_H; M32(mm + 24) = 112; M32(mm + 28) = 27; M32(mm + 32) = SCREEN_W; M32(mm + 36) = SCREEN_H;
    send(c, w, WM_GETMINMAXINFO, 0, mm);
    put_rect(rc, x, y, x + cx, y + cy); send(c, w, WM_NCCALCSIZE, 0, rc);
  }
  w->x = x; w->y = y; w->w = cx; w->h = cy;
  send(c, w, WM_WINDOWPOSCHANGED, 0, wp);
  s32 cw, ch; client_size(w, &cw, &ch);
  if (sized) send(c, w, WM_SIZE, 0, (u32)(cw & 0xFFFF) | (u32)(ch & 0xFFFF) << 16);
  if (moved) send(c, w, WM_MOVE, 0, (u32)((w->x + (w->has_frame ? FRAME : 0)) & 0xFFFF) | (u32)((w->y + (w->has_frame ? FRAME + CAPTION : 0)) & 0xFFFF) << 16);
  free((void *)GP(wp));
}
static void show(Cpu *c, Window *w, int visible) {
  if (w->visible == visible) return;
  send(c, w, WM_SHOWWINDOW, (u64)visible, 0);
  w->visible = visible; w->needs_paint = visible;
  if (visible) { // it becomes the active window with the keyboard focus
    send(c, w, WM_ACTIVATEAPP, 1, 0); send(c, w, WM_NCACTIVATE, 1, 0); send(c, w, WM_ACTIVATE, 1, 0); send(c, w, WM_SETFOCUS, 0, 0);
    s32 cw, ch; client_size(w, &cw, &ch);
    send(c, w, WM_SIZE, 0, (u32)(cw & 0xFFFF) | (u32)(ch & 0xFFFF) << 16);
  }
}
IMP(user32, SetWindowPos) {
  Window *w = window_of(ARG(0)); u32 flags = (u32)ARG(6);
  if (!w) { SET_LAST_ERROR(1400); RET(0); return; }
  s32 x = (flags & 2) ? w->x : (s32)ARG(2), y = (flags & 2) ? w->y : (s32)ARG(3), cx = (flags & 1) ? w->w : (s32)ARG(4), cy = (flags & 1) ? w->h : (s32)ARG(5);
  moved_or_sized(c, w, x, y, cx, cy, flags);
  if (flags & 0x40) show(c, w, 1); else if (flags & 0x80) show(c, w, 0);
  RET(1);
}
IMP(user32, ShowWindow) { // SW_HIDE is 0; every other command shows
  Window *w = window_of(ARG(0));
  if (!w) { RET(0); return; }
  int was = w->visible; show(c, w, (u32)ARG(1) != 0); RET(was);
}
IMP(user32, UpdateWindow) { RET(1); }
IMP(user32, DestroyWindow) { Window *w = window_of(ARG(0)); if (w) destroy(c, w); RET(w != 0); }
IMP(user32, PostQuitMessage) { quit_posted = 1; quit_code = (u32)ARG(0); }
// The message queue holds WM_QUIT once it is posted, and WM_PAINT for a window that was shown and not painted
// since: that one stays due until the window is validated (DefWindowProcA does it), as on Windows. Input does
// not arrive as messages yet. PeekMessageA(MSG *, window, first, last, flags). MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y
static int wanted(Cpu *c, u32 msg) { u32 lo = (u32)ARG(2), hi = (u32)ARG(3); return (!lo && !hi) || (msg >= lo && msg <= hi); }
IMP(user32, PeekMessageA) {
  u64 m = ARG(0);
  if (quit_posted && wanted(c, WM_QUIT)) {
    memset((void *)GP(m), 0, 28); M32(m + 4) = WM_QUIT; M32(m + 8) = quit_code;
    if ((u32)ARG(4) & 1) quit_posted = 0; // PM_REMOVE
    RET(1); return;
  }
  if (wanted(c, WM_PAINT)) for (unsigned i = 0; i < window_count; i++) {
    Window *w = &windows[i];
    if (!w->alive || !w->visible || !w->needs_paint || (ARG(1) && (u32)ARG(1) != w->hwnd)) continue;
    memset((void *)GP(m), 0, 28); M32(m) = w->hwnd; M32(m + 4) = WM_PAINT; M32(m + 16) = (u32)(rt_uptime_us() / 1000);
    RET(1); return;
  }
  RET(0);
}
IMP(user32, TranslateMessage) { RET(0); }
IMP(user32, DispatchMessageA) { u64 m = ARG(0); Window *w = window_of(M32(m)); RET(w ? send(c, w, M32(m + 4), M32(m + 8), M32(m + 12)) : 0); }
IMP(user32, FillRect) { RET(ARG(1) ? gdi_fill_rect((u32)ARG(0), (const s32 *)GP(ARG(1)), (u32)ARG(2)) : 0); } // (dc, RECT *, brush)

// ---- window longs: GWL_WNDPROC -4, GWL_HINSTANCE -6, GWL_ID -12, GWL_STYLE -16, GWL_EXSTYLE -20, GWL_USERDATA -21 ----
static u32 *window_long(Window *w, s32 i) {
  switch (i) { case -4: return &w->proc; case -6: return &w->instance; case -16: return &w->style; case -20: return &w->exstyle; case -21: return &w->userdata; default: return 0; }
}
IMP(user32, GetWindowLongA) { Window *w = window_of(ARG(0)); u32 *v = w ? window_long(w, (s32)ARG(1)) : 0; if (!v) { SET_LAST_ERROR(w ? 1413 : 1400); RET(0); return; } RET(*v); }
IMP(user32, SetWindowLongA) { // the style's frame bits change what the client area is
  Window *w = window_of(ARG(0)); u32 *v = w ? window_long(w, (s32)ARG(1)) : 0;
  if (!v) { SET_LAST_ERROR(w ? 1413 : 1400); RET(0); return; }
  u32 old = *v; *v = (u32)ARG(2);
  if ((s32)ARG(1) == -16) w->has_frame = (w->style & 0x00C00000) == 0x00C00000;
  RET(old);
}
// ---- the screen and the cursor ----
IMP(user32, GetSystemMetrics) {
  switch ((s32)ARG(0)) {
  case 0: case 16: case 78: RET(SCREEN_W); return;   // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
  case 1: case 79: RET(SCREEN_H); return;             // SM_CYSCREEN, SM_CYVIRTUALSCREEN
  case 17: RET(SCREEN_H - CAPTION); return;           // SM_CYFULLSCREEN
  case 4: RET(CAPTION); return;                       // SM_CYCAPTION
  case 5: case 6: case 32: case 33: RET(FRAME); return; // borders and frames
  case 19: RET(1); return;                            // SM_MOUSEPRESENT
  case 43: RET(3); return;                            // SM_CMOUSEBUTTONS
  case 80: RET(1); return;                            // SM_CMONITORS
  default: RET(0); return;
  }
}
// (device name or 0, index, DISPLAY_DEVICEA*, flags): one display, the page's canvas
IMP(user32, EnumDisplayDevicesA) {
  u64 d = ARG(2);
  if (ARG(0) || ARG(1) != 0 || !d || M32(d) < 424) { RET(0); return; }
  memset((void *)GP(d + 4), 0, 420);
  strcpy(GSTR(d + 4), "\\\\.\\DISPLAY1"); strcpy(GSTR(d + 36), "PISCO WebGPU display");
  M32(d + 164) = 0x1 | 0x4;                           // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
  strcpy(GSTR(d + 168), "PCI\\VEN_0000&DEV_0000");
  RET(1);
}
IMP(user32, SetCursorPos) { RET(1); }                 // the page's pointer is not ours to move (Pointer Lock gives deltas)
static s32 cursor_count;
IMP(user32, ShowCursor) { cursor_count += ARG(0) ? 1 : -1; RET((u32)cursor_count); }
IMP(user32, CharLowerBuffA) { char *s = GSTR(ARG(0)); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32); RET(n); }
