// SPDX-License-Identifier: GPL-2.0-or-later
// user32 for Windows desktop titles: window classes, one top-level window (the host shows its client area),
// and the messages Windows sends a window as it is created, sized, shown and closed, delivered by calling the
// title's own window procedure. Input (rt/input.c's events) becomes keyboard and mouse messages for the window
// that has the focus, as Windows posts them; a title may read DirectInput instead.
#include "win32.h"
#include "../input.h"

enum { WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
       WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46,
       WM_WINDOWPOSCHANGED = 0x47, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCACTIVATE = 0x86 };
static s32 SCREEN_W = 1024, SCREEN_H = 768;   // the desktop: what the oracle's has, until a title changes the display mode
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
static u32 focus_hwnd, capture_hwnd;   // the window with the keyboard focus (the one last shown, or set), the one capturing the mouse

static Window *window_of(u64 hwnd) { for (unsigned i = 0; i < window_count; i++) if (windows[i].alive && windows[i].hwnd == (u32)hwnd) return &windows[i]; return 0; }
static WndClass *class_of(u64 name) {
  for (unsigned i = 0; i < class_count; i++) if (name < 0x10000 ? classes[i].atom == (u32)name : !strcasecmp(classes[i].name, GSTR(name))) return &classes[i];
  return 0;
}
static void show(Cpu *c, Window *w, int visible);
static u64 send(Cpu *c, Window *w, u32 msg, u64 wp, u64 lp) { return w->proc ? rt_guest_call(c, w->proc, w->hwnd, msg, wp, lp) : 0; }
static void client_size(const Window *w, s32 *cw, s32 *ch) {
  *cw = w->w - (w->has_frame ? 2 * FRAME : 0); *ch = w->h - (w->has_frame ? 2 * FRAME + CAPTION : 0);
  if (*cw < 0) *cw = 0; if (*ch < 0) *ch = 0;
}
void win32_screen_size(s32 *w, s32 *h) { *w = SCREEN_W; *h = SCREEN_H; }
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
  if (w->style & 0x10000000) show(c, w, 1); // WS_VISIBLE: shown as ShowWindow shows it, activated, with the focus
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
    focus_hwnd = w->hwnd;
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

// Posted messages: input, WM_CHAR from TranslateMessage, timers. MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y
typedef struct { u32 hwnd, msg, wp, lp, time; s32 x, y; } Msg;
enum { QUEUE = 256 };
static Msg queue[QUEUE]; static unsigned q_head, q_tail;   // q_head: the next to take
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static s32 cursor_x = 512, cursor_y = 384;                 // the cursor, in screen coordinates
static void post(u32 hwnd, u32 msg, u32 wp, u32 lp) {
  if (rt_trace > 2) rt_log("post 0x%x to 0x%x: 0x%x 0x%x", msg, hwnd, wp, lp);
  if (q_tail - q_head == QUEUE) return;   // full: Windows drops input too when a queue is not read
  queue[q_tail++ % QUEUE] = (Msg){hwnd, msg, wp, lp, (u32)(rt_uptime_us() / 1000), cursor_x, cursor_y};
}
static Window *focus_window(void) {
  Window *w = window_of(focus_hwnd);
  if (!w) for (unsigned i = 0; i < window_count; i++) if (windows[i].alive && windows[i].visible) { w = &windows[i]; break; }
  return w;
}
static void client_origin(const Window *w, s32 *x, s32 *y) { *x = w->x + (w->has_frame ? FRAME : 0); *y = w->y + (w->has_frame ? FRAME + CAPTION : 0); }
// the virtual key a message carries: the generic one for the shifts, controls and alts, as Windows sends
static u32 message_vk(u32 scan) { u32 vk = input_vk_of_scan(scan); return vk == 0xA0 || vk == 0xA1 ? 0x10 : vk == 0xA2 || vk == 0xA3 ? 0x11 : vk == 0xA4 || vk == 0xA5 ? 0x12 : vk; }
static u32 input_seen;
// rt/input.c's events since the last look, as the messages Windows would post for them
static void pump_input(void) {
  input_poll();
  u32 n = input_event_count();
  if (n - input_seen > 1024) input_seen = n - 1024;
  for (; input_seen < n; input_seen++) {
    const InputEvent *e = input_event(input_seen); if (!e) continue;
    Window *w = focus_window();
    if (rt_trace > 2) rt_log("input event %u: kind %u code %u value %d (window 0x%x)", input_seen, e->kind, e->code, e->value, w ? w->hwnd : 0);
    if (!w) continue;
    const u8 *keys = input_keys(); int alt = keys[0x38] || keys[0xB8];
    if (e->kind == INPUT_KEY) {
      u32 scan = e->code, vk = message_vk(scan);
      u32 lp = 1 | (scan & 0x7F) << 16 | (scan & 0x80 ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) | (e->value ? 0 : 3u << 30);
      int sys = alt || vk == 0x12 || vk == 0x79;   // with Alt down, Alt itself, and F10: WM_SYSKEYDOWN / WM_SYSKEYUP
      post(w->hwnd, (e->value ? 0x100 : 0x101) + (sys ? 4 : 0), vk, lp);
    } else if (e->kind != INPUT_KEY && input_mouse_exclusive) {
      continue;   // a mouse DirectInput holds exclusively: no messages, and the cursor does not move
    } else if (e->kind == INPUT_AXIS && e->code < 2) {
      if (e->code == 0) cursor_x += e->value; else cursor_y += e->value;
      if (cursor_x < 0) cursor_x = 0; if (cursor_x >= SCREEN_W) cursor_x = SCREEN_W - 1;
      if (cursor_y < 0) cursor_y = 0; if (cursor_y >= SCREEN_H) cursor_y = SCREEN_H - 1;
      s32 ox, oy; client_origin(w, &ox, &oy);
      // one WM_MOUSEMOVE for the x and y of one motion: the y event's
      const InputEvent *next = input_event(input_seen + 1);
      if (e->code == 0 && next && next->kind == INPUT_AXIS && next->code == 1 && next->sequence == e->sequence) continue;
      const u8 *b = input_mouse_buttons();
      u32 wp = (b[0] ? 1 : 0) | (b[1] ? 2 : 0) | (b[2] ? 0x10 : 0) | (b[3] ? 0x20 : 0) | (b[4] ? 0x40 : 0) | (keys[0x2A] || keys[0x36] ? 4 : 0) | (keys[0x1D] || keys[0x9D] ? 8 : 0);
      post(w->hwnd, 0x200, wp, (u32)((cursor_x - ox) & 0xFFFF) | (u32)((cursor_y - oy) & 0xFFFF) << 16);
    } else if (e->kind == INPUT_AXIS && e->code == 2) {
      post(w->hwnd, 0x20A, (u32)(e->value & 0xFFFF) << 16, (u32)(cursor_x & 0xFFFF) | (u32)(cursor_y & 0xFFFF) << 16);   // WM_MOUSEWHEEL: screen coordinates
    } else if (e->kind == INPUT_BUTTON) {
      static const u32 down[5] = {0x201, 0x204, 0x207, 0x20B, 0x20B};
      s32 ox, oy; client_origin(w, &ox, &oy);
      // wParam: the buttons and keys down once this happened (MK_*), as for WM_MOUSEMOVE; an X button's number in the high word
      const u8 *bt = input_mouse_buttons();
      u32 mk = (bt[0] ? 1 : 0) | (bt[1] ? 2 : 0) | (bt[2] ? 0x10 : 0) | (bt[3] ? 0x20 : 0) | (bt[4] ? 0x40 : 0) | (keys[0x2A] || keys[0x36] ? 4 : 0) | (keys[0x1D] || keys[0x9D] ? 8 : 0);
      u32 msg = down[e->code < 5 ? e->code : 0] + (e->value ? 0 : 1), wp = mk | (e->code >= 3 ? (e->code - 2) << 16 : 0);
      post(w->hwnd, msg, wp, (u32)((cursor_x - ox) & 0xFFFF) | (u32)((cursor_y - oy) & 0xFFFF) << 16);
    }
  }
}
// SetTimer: a window's timers post WM_TIMER (0x113) when due, or call their procedure
typedef struct { u32 hwnd, id, ms, proc; u64 due; int used; } Timer;
static Timer timers[16];
static void put_msg(u64 m, const Msg *q) { M32(m) = q->hwnd; M32(m + 4) = q->msg; M32(m + 8) = q->wp; M32(m + 12) = q->lp; M32(m + 16) = q->time; M32(m + 20) = (u32)q->x; M32(m + 24) = (u32)q->y; }
// The next message for PeekMessageA / GetMessageA (MSG *, window, first, last, flags): posted ones first, then
// WM_PAINT, then timers; WM_QUIT when nothing else is there
static int next_message(Cpu *c, u64 m, int remove) {
  pump_input();
  pthread_mutex_lock(&q_lock);
  for (unsigned k = q_head; k != q_tail; k++) {
    Msg *q = &queue[k % QUEUE];
    if (!wanted(c, q->msg) || (ARG(1) && (u32)ARG(1) != q->hwnd)) continue;
    Msg got = *q;
    if (remove) { for (unsigned j = k; j != q_head; j--) queue[j % QUEUE] = queue[(j - 1) % QUEUE]; q_head++; }
    pthread_mutex_unlock(&q_lock);
    put_msg(m, &got); return 1;
  }
  pthread_mutex_unlock(&q_lock);
  if (wanted(c, WM_PAINT)) for (unsigned i = 0; i < window_count; i++) {
    Window *w = &windows[i];
    if (!w->alive || !w->visible || !w->needs_paint || (ARG(1) && (u32)ARG(1) != w->hwnd)) continue;
    Msg p = {w->hwnd, WM_PAINT, 0, 0, (u32)(rt_uptime_us() / 1000), cursor_x, cursor_y}; put_msg(m, &p);
    return 1;
  }
  u64 now = rt_uptime_us();
  if (wanted(c, 0x113)) for (unsigned i = 0; i < 16; i++) {
    Timer *t = &timers[i];
    if (!t->used || now < t->due || (ARG(1) && (u32)ARG(1) != t->hwnd)) continue;
    if (remove) t->due = now + 1000ull * t->ms;
    Msg p = {t->hwnd, 0x113, t->id, t->proc, (u32)(now / 1000), cursor_x, cursor_y}; put_msg(m, &p);
    return 1;
  }
  if (quit_posted && wanted(c, WM_QUIT)) {
    Msg p = {0, WM_QUIT, quit_code, 0, (u32)(now / 1000), cursor_x, cursor_y}; put_msg(m, &p);
    if (remove) quit_posted = 0;
    return 1;
  }
  return 0;
}
IMP(user32, PeekMessageA) { RET(next_message(c, ARG(0), (u32)ARG(4) & 1)); }   // PM_REMOVE
// GetMessageA waits for a message; FALSE for WM_QUIT
IMP(user32, GetMessageA) {
  while (!next_message(c, ARG(0), 1)) { if (rt_clock_virtual()) rt_clock_advance(1000); else { struct timespec t = {0, 1000000}; nanosleep(&t, 0); } }
  RET(M32(ARG(0) + 4) != WM_QUIT);
}
IMP(user32, SetTimer) { // (window, id, ms, procedure): the id (a new one for a window of 0)
  u32 hwnd = (u32)ARG(0), id = (u32)ARG(1);
  int k = -1; for (int i = 0; i < 16; i++) if (timers[i].used && timers[i].hwnd == hwnd && timers[i].id == id && hwnd) k = i;
  if (k < 0) for (int i = 0; i < 16; i++) if (!timers[i].used) { k = i; break; }
  if (k < 0) { RET(0); return; }
  if (!hwnd) id = 0x7000 + (u32)k;
  timers[k] = (Timer){hwnd, id, (u32)ARG(2) ? (u32)ARG(2) : 10, (u32)ARG(3), rt_uptime_us() + 1000ull * ((u32)ARG(2) ? (u32)ARG(2) : 10), 1};
  RET(id);
}
IMP(user32, KillTimer) { for (int i = 0; i < 16; i++) if (timers[i].used && timers[i].hwnd == (u32)ARG(0) && timers[i].id == (u32)ARG(1)) { timers[i].used = 0; RET(1); return; } RET(0); }
// TranslateMessage: a key that makes a character posts WM_CHAR after it (a US keyboard, Caps Lock off)
static u32 char_of(u32 scan, int shift, int ctrl) {
  static const char plain[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./", shifted[] = "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
  switch (scan) { case 0x01: return 27; case 0x0E: return 8; case 0x0F: return 9; case 0x1C: case 0x9C: return 13; case 0x39: return ' '; }
  if (scan < 0x02 || scan > 0x35) return 0;
  u32 ch = (u8)(shift ? shifted : plain)[scan - 2];
  if (ctrl && ch) { if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1; if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 1; return 0; }
  return ch;
}
IMP(user32, TranslateMessage) {
  u64 m = ARG(0); u32 msg = M32(m + 4);
  if (msg != 0x100 && msg != 0x104) { RET(0); return; }
  const u8 *k = input_keys();
  u32 ch = char_of((M32(m + 12) >> 16 & 0x7F) | (M32(m + 12) & (1u << 24) ? 0x80 : 0), k[0x2A] || k[0x36], k[0x1D] || k[0x9D]);
  if (!ch) { RET(0); return; }
  pthread_mutex_lock(&q_lock);
  // first in the queue: the character comes before anything posted after the key
  if (q_tail - q_head < QUEUE) { q_head--; queue[q_head % QUEUE] = (Msg){M32(m), msg == 0x104 ? 0x106u : 0x102u, ch, M32(m + 12), M32(m + 16), cursor_x, cursor_y}; }
  pthread_mutex_unlock(&q_lock);
  RET(1);
}
IMP(user32, DispatchMessageA) {
  u64 m = ARG(0); u32 msg = M32(m + 4);
  if (msg == 0x113 && M32(m + 12)) { RET(rt_guest_call(c, M32(m + 12), M32(m), 0x113, M32(m + 8), (u32)(rt_uptime_us() / 1000))); return; }   // a timer's procedure
  Window *w = window_of(M32(m)); RET(w ? send(c, w, msg, M32(m + 8), M32(m + 12)) : 0);
}
IMP(user32, SendMessageA) { Window *w = window_of(ARG(0)); RET(w ? send(c, w, (u32)ARG(1), ARG(2), ARG(3)) : 0); }
IMP(user32, CallWindowProcA) { RET(ARG(0) ? rt_guest_call(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)) : 0); }
IMP(user32, RegisterWindowMessageA) { static u32 next = 0xC100; RET(next++); }
IMP(user32, InvalidateRect) { Window *w = window_of(ARG(0)); if (w) w->needs_paint = 1; RET(1); }
// ---- focus, activation, the cursor ----
IMP(user32, GetForegroundWindow) { Window *w = focus_window(); RET(w ? w->hwnd : 0); }
IMP(user32, GetActiveWindow) { Window *w = focus_window(); RET(w ? w->hwnd : 0); }
IMP(user32, SetForegroundWindow) { if (window_of(ARG(0))) focus_hwnd = (u32)ARG(0); RET(window_of(ARG(0)) != 0); }
IMP(user32, SetFocus) { u32 old = focus_hwnd; if (window_of(ARG(0))) focus_hwnd = (u32)ARG(0); RET(old); }
IMP(user32, SetCapture) { u32 old = capture_hwnd; capture_hwnd = (u32)ARG(0); RET(old); }
IMP(user32, ReleaseCapture) { capture_hwnd = 0; RET(1); }
IMP(user32, ClipCursor) { RET(1); }
IMP(user32, IsIconic) { RET(0); }
IMP(user32, CloseWindow) { RET(window_of(ARG(0)) != 0); }   // minimizes: the page has nothing to minimize to
IMP(user32, GetCursorPos) { pump_input(); if (rt_trace > 2) rt_log("GetCursorPos %d,%d", cursor_x, cursor_y); M32(ARG(0)) = (u32)cursor_x; M32(ARG(0) + 4) = (u32)cursor_y; RET(1); }
IMP(user32, GetKeyState) { // the key's state as of the messages read; bit 0 toggles (left at 0)
  pump_input(); int s = input_scan_of_vk((u32)ARG(0) & 0xFF);
  if ((u32)ARG(0) >= 1 && (u32)ARG(0) <= 6) { const u8 *b = input_mouse_buttons(); u32 v = (u32)ARG(0); int i = v == 1 ? 0 : v == 2 ? 1 : v == 4 ? 2 : -1; RET(i >= 0 && b[i] ? 0xFF80u : 0); return; }
  RET(s >= 0 && input_keys()[s] ? 0xFF80u : 0);
}
IMP(user32, GetTopWindow) { Window *w = focus_window(); RET(!ARG(0) && w ? w->hwnd : 0); }
IMP(user32, GetWindow) { RET(0); }   // no window has another next to it, above it or inside it
IMP(user32, GetWindowThreadProcessId) { if (ARG(1)) M32(ARG(1)) = 4; RET(window_of(ARG(0)) ? 1 : 0); }   // the first thread of process 4 (rt/process.c's TEB)
IMP(user32, FindWindowA) { // (class, title)
  for (unsigned i = 0; i < window_count; i++) {
    Window *w = &windows[i]; if (!w->alive) continue;
    if (ARG(0) && (ARG(0) < 0x10000 ? w->cls->atom != (u32)ARG(0) : strcasecmp(w->cls->name, GSTR(ARG(0))))) continue;
    if (ARG(1) && strcmp(w->title, GSTR(ARG(1)))) continue;
    RET(w->hwnd); return;
  }
  RET(0);
}
IMP(user32, GetWindowTextA) { Window *w = window_of(ARG(0)); u32 cap = (u32)ARG(2); if (!w || !cap) { RET(0); return; } snprintf(GSTR(ARG(1)), cap, "%s", w->title); RET(strlen(GSTR(ARG(1)))); }
IMP(user32, SetWindowTextA) { Window *w = window_of(ARG(0)); if (!w) { RET(0); return; } snprintf(w->title, sizeof w->title, "%s", ARG(1) ? GSTR(ARG(1)) : ""); if (rt_trace) rt_log("window title \"%s\"", w->title); RET(1); }
IMP(user32, AdjustWindowRect) { // (RECT *, style, menu): the window's rectangle around that client area
  if ((u32)ARG(1) & 0x00C00000) { u64 r = ARG(0); M32(r) -= FRAME; M32(r + 4) -= FRAME + CAPTION; M32(r + 8) += FRAME; M32(r + 12) += FRAME; }
  RET(1);
}
// ---- hooks and hot keys: kept, never called (no other program's input passes through this one) ----
IMP(user32, SetWindowsHookExA) { static u32 next = 0x00050000; RET(next += 4); }
IMP(user32, UnhookWindowsHookEx) { RET(1); }
IMP(user32, CallNextHookEx) { RET(0); }
IMP(user32, RegisterHotKey) { RET(1); }
IMP(user32, UnregisterHotKey) { RET(1); }
// ---- the display: the modes a 2001 monitor offers, 32 bits a pixel; a mode change makes it the screen's size ----
static const u16 modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024}, {1600, 1200}};
// DEVMODEA: dmSize at 36, dmFields at 40, dmBitsPerPel at 104, dmPelsWidth 108, dmPelsHeight 112, dmDisplayFlags 116, dmDisplayFrequency 120
IMP(user32, EnumDisplaySettingsA) {
  u32 i = (u32)ARG(1); u64 d = ARG(2); u32 w, h;
  if (i == 0xFFFFFFFFu || i == 0xFFFFFFFEu) { w = (u32)SCREEN_W; h = (u32)SCREEN_H; }   // ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS
  else if (i < sizeof modes / sizeof modes[0]) { w = modes[i][0]; h = modes[i][1]; }
  else { RET(0); return; }
  memset((void *)GP(d), 0, 124); strcpy(GSTR(d), "PISCO WebGPU display"); M16(d + 36) = 124;
  M32(d + 40) = 0x40000 | 0x80000 | 0x100000 | 0x400000; M32(d + 104) = 32; M32(d + 108) = w; M32(d + 112) = h; M32(d + 120) = 60;
  RET(1);
}
IMP(user32, ChangeDisplaySettingsA) { // (DEVMODEA * or 0 for the registry's mode, flags): DISP_CHANGE_SUCCESSFUL
  u64 d = ARG(0);
  if (!d) { SCREEN_W = 1024; SCREEN_H = 768; RET(0); return; }
  u32 fields = M32(d + 40);
  if (fields & 0x80000) SCREEN_W = (s32)M32(d + 108);
  if (fields & 0x100000) SCREEN_H = (s32)M32(d + 112);
  if (rt_trace) rt_log("display mode %dx%d", SCREEN_W, SCREEN_H);
  RET(0);
}
IMP(user32, SystemParametersInfoA) { // (action, uint, pointer, flags): what is asked is answered with nothing set
  u32 a = (u32)ARG(0); u64 p = ARG(2);
  if (a == 3 && p) { M32(p) = 6; M32(p + 4) = 10; M32(p + 8) = 1; }   // SPI_GETMOUSE: thresholds and acceleration
  else if ((a == 0x10 || a == 0x2A || a == 0x3A || a == 0x34 || a == 0x32 || a == 0x30) && p) M32(p) = 0;   // screen saver, sticky / filter / toggle keys...
  RET(1);
}
IMP(user32, LoadBitmapA) { RET(0x00040000u + ((u32)ARG(1) & 0xFFFF)); }
// ---- the clipboard: empty, and what is put on it is not kept ----
IMP(user32, OpenClipboard) { RET(1); }
IMP(user32, CloseClipboard) { RET(1); }
IMP(user32, EmptyClipboard) { RET(1); }
IMP(user32, GetClipboardData) { RET(0); }
IMP(user32, SetClipboardData) { RET(ARG(1)); }
// ---- device contexts: a window's is its handle with the DC bit (gdi32 draws into it) ----
IMP(user32, GetDC) { RET(window_of(ARG(0)) || !ARG(0) ? 0x01000000u | ((u32)ARG(0) & 0xFFFFFF) : 0); }
IMP(user32, ReleaseDC) { RET(1); }
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
IMP(user32, SetCursorPos) { if (rt_trace > 2) rt_log("SetCursorPos %d,%d", (s32)ARG(0), (s32)ARG(1)); cursor_x = (s32)ARG(0); cursor_y = (s32)ARG(1); RET(1); }   // ours to move: the page's pointer gives motion (Pointer Lock)
static s32 cursor_count;
IMP(user32, ShowCursor) { cursor_count += ARG(0) ? 1 : -1; RET((u32)cursor_count); }
IMP(user32, CharLowerBuffA) { char *s = GSTR(ARG(0)); u32 n = (u32)ARG(1); for (u32 i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32); RET(n); }
