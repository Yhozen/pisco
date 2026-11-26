// SPDX-License-Identifier: GPL-2.0-or-later
// The title's window: the ntuser and DWM functions a 64-bit title (console or desktop) uses. Shared by the Xbox Series
// and Windows x64 runtimes: on the console it is the screen, on the desktop the window DXGI presents to.
#include <strings.h>
#include "gdk.h"
#include "input.h"

// ---- the title's window: on the console the screen, a hidden 1920x1080 frameless tool window; on the desktop the
// window DXGI presents to, the size it was created with (its client area: there is no frame, AdjustWindowRect leaves
// a rectangle as it is). Classes keep their window procedure; a queue holds posted messages (WM_QUIT), which
// PeekMessageW / GetMessageW hand out and DispatchMessageW passes to the procedure. Nothing is sent at creation:
// the titles seen hand every message but WM_CLOSE to DefWindowProc. ----
typedef struct { u32 magic; u32 style, exstyle; u64 proc; s32 w, h; char *title; } Window;
typedef struct { u16 atom; char name[64]; u64 proc; } WinClass;
static WinClass classes[32]; static int nclasses;
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 main_window; // the first window created: the foreground one
static u16 class_add(const char *name, u64 proc) {
  pthread_mutex_lock(&win_lock);
  u16 atom = 0;
  for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, name)) { atom = classes[i].atom; classes[i].proc = proc; }
  if (!atom && nclasses < 32) { WinClass *k = &classes[nclasses++]; k->atom = (u16)(0xC000 + nclasses); snprintf(k->name, sizeof k->name, "%s", name); k->proc = proc; atom = k->atom; }
  pthread_mutex_unlock(&win_lock);
  return atom ? atom : 0xC000;
}
static u64 class_proc(u64 name, int wide) { // a class by its atom (below 0x10000) or name
  char *n = name < 0x10000 ? 0 : wide ? rt_narrow(GWSTR(name)) : strdup(GSTR(name)); u64 proc = 0;
  pthread_mutex_lock(&win_lock);
  for (int i = 0; i < nclasses; i++) if (n ? !strcasecmp(classes[i].name, n) : classes[i].atom == (u16)name) proc = classes[i].proc;
  pthread_mutex_unlock(&win_lock);
  free(n); return proc;
}
// WNDCLASS(EX)W: the procedure at +8 (+8 in both: cbSize and style, or style and padding), the name at +64 (EX) or +64
IMP(user32, RegisterClassW) { u64 k = ARG(0); char *n = M64(k + 64) >= 0x10000 ? rt_narrow(GWSTR(M64(k + 64))) : strdup("#"); RET(class_add(n, M64(k + 8))); free(n); }
IMP(user32, RegisterClassExW) { u64 k = ARG(0); char *n = M64(k + 64) >= 0x10000 ? rt_narrow(GWSTR(M64(k + 64))) : strdup("#"); RET(class_add(n, M64(k + 8))); free(n); }
IMP(user32, RegisterClassA) { u64 k = ARG(0); RET(class_add(M64(k + 64) >= 0x10000 ? GSTR(M64(k + 64)) : "#", M64(k + 8))); }
IMP(user32, UnregisterClassW) { RET(1); }
static u64 make_window(Cpu *c, int wide) { // (exstyle, class, title, style, x, y, w, h, parent, menu, instance, param)
  Window *w = calloc(1, sizeof *w); w->magic = 0x57494E44u; w->style = 0x90000000u; w->exstyle = 0x80;
  w->proc = class_proc(ARG(1), wide);
  s32 cw = (s32)ARG(6), ch = (s32)ARG(7);
  w->w = cw > 0 && cw != (s32)0x80000000 ? cw : 1920; w->h = ch > 0 && cw != (s32)0x80000000 ? ch : 1080;
  if (ARG(2)) w->title = wide ? rt_narrow(GWSTR(ARG(2))) : strdup(GSTR(ARG(2)));
  if (ARG(3) || ARG(0)) { w->style = (u32)ARG(3); w->exstyle = (u32)ARG(0); }
  if (!main_window) main_window = (uptr)w;
  return (uptr)w;
}
static Window *window(u64 h) { Window *w = (Window *)GP(h); return h && w->magic == 0x57494E44u ? w : 0; }
IMP(user32, CreateWindowExW) { RET(make_window(c, 1)); }
IMP(user32, CreateWindowExA) { RET(make_window(c, 0)); }
// ---- the message queue: posted messages, for every window of the title ----
typedef struct { u64 hwnd; u32 msg; u64 wp, lp; } Msg;
static Msg queue[256]; static u32 q_head, q_tail;
void rt_window_post(u64 hwnd, u32 msg, u64 wp, u64 lp) {
  pthread_mutex_lock(&win_lock);
  if (q_tail - q_head < 256) queue[q_tail++ % 256] = (Msg){hwnd, msg, wp, lp};
  pthread_mutex_unlock(&win_lock);
}
static int take(u64 out, int remove) { // MSG: hwnd, message, wParam (+16), lParam (+24), time (+32), pt (+36)
  pthread_mutex_lock(&win_lock);
  int got = q_head != q_tail;
  if (got) {
    Msg m = queue[q_head % 256]; if (remove) q_head++;
    memset((void *)GP(out), 0, 48); M64(out) = m.hwnd; M32(out + 8) = m.msg; M64(out + 16) = m.wp; M64(out + 24) = m.lp;
    M32(out + 32) = (u32)(rt_uptime_us() / 1000);
  }
  pthread_mutex_unlock(&win_lock);
  return got;
}
IMP(user32, PeekMessageW) { RET(take(ARG(0), (u32)ARG(4) & 1)); } // (msg, hwnd, min, max, PM_REMOVE 1)
IMP(user32, PeekMessageA) { RET(take(ARG(0), (u32)ARG(4) & 1)); }
IMP(user32, GetMessageW) { RET(take(ARG(0), 1) && M32(ARG(0) + 8) != 0x12); } // nothing queued: 0, as for WM_QUIT (a title would wait for ever)
IMP(user32, TranslateMessage) { RET(0); }
static u64 send(Cpu *c, u64 hwnd, u32 msg, u64 wp, u64 lp) { Window *w = window(hwnd); return w && w->proc ? rt_guest_call(c, w->proc, hwnd, msg, wp, lp) : 0; }
IMP(user32, DispatchMessageW) { u64 m = ARG(0); RET(M32(m + 8) == 0x12 ? 0 : send(c, M64(m), M32(m + 8), M64(m + 16), M64(m + 24))); }
IMP(user32, DispatchMessageA) { u64 m = ARG(0); RET(M32(m + 8) == 0x12 ? 0 : send(c, M64(m), M32(m + 8), M64(m + 16), M64(m + 24))); }
IMP(user32, PostQuitMessage) { rt_window_post(0, 0x12, (u32)ARG(0), 0); } // WM_QUIT
IMP(user32, PostMessageW) { rt_window_post(ARG(0), (u32)ARG(1), ARG(2), ARG(3)); RET(1); }
IMP(user32, SendMessageW) { RET(send(c, ARG(0), (u32)ARG(1), ARG(2), ARG(3))); }
IMP(user32, DestroyWindow) { u64 h = ARG(0); send(c, h, 0x2, 0, 0); if (h == main_window) main_window = 0; RET(1); } // WM_DESTROY
IMP(user32, ShowWindow) { RET(1); }
IMP(user32, UpdateWindow) { RET(1); }
IMP(user32, DefWindowProcW) { RET(0); }
IMP(user32, DefWindowProcA) { RET(0); }
IMP(user32, GetWindowLongW) { Window *w = window(ARG(0)); s32 i = (s32)ARG(1); RET(!w ? 0 : i == -16 ? w->style : i == -20 ? w->exstyle : 0); }
IMP(user32, SetWindowLongW) { RET(0); }
IMP(user32, LoadIconW) { RET(0x10001); }   // icons, cursors and brushes: handles nothing draws
IMP(user32, LoadIconA) { RET(0x10001); }
IMP(user32, LoadCursorW) { RET(0x10002); }
IMP(user32, LoadCursorA) { RET(0x10002); }
IMP(user32, GetSysColorBrush) { RET(0x10100 + ((u32)ARG(0) & 0xff)); }
IMP(user32, AdjustWindowRect) { RET(1); }  // no frame: the rect stays
IMP(user32, AdjustWindowRectEx) { RET(1); }
static void rect(u64 h, u64 r) { Window *w = window(h); M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = w ? (u32)w->w : 1920; M32(r + 12) = w ? (u32)w->h : 1080; }
IMP(user32, GetClientRect) { rect(ARG(0), ARG(1)); RET(1); }
IMP(user32, GetWindowRect) { rect(ARG(0), ARG(1)); RET(1); }
// The title's window is the one in front, never minimised; the cursor rests at its centre (titles that read the
// mouse do it through GameInput or raw input)
IMP(user32, GetForegroundWindow) { RET(main_window); }
IMP(user32, GetActiveWindow) { RET(main_window); }
IMP(user32, IsIconic) { RET(0); }
IMP(user32, GetCursorPos) { Window *w = window(main_window); M32(ARG(0)) = w ? (u32)(w->w / 2) : 960; M32(ARG(0) + 4) = w ? (u32)(w->h / 2) : 540; RET(1); }
// The window's title: kept, and each new one logged ("window title: ..."), for a title that shows its state there
IMP(user32, SetWindowTextW) {
  Window *w = window(ARG(0)); char *t = ARG(1) ? rt_narrow(GWSTR(ARG(1))) : strdup("");
  if (w && (!w->title || strcmp(w->title, t))) { rt_log("window title: %s", t); free(w->title); w->title = t; } else free(t);
  RET(1);
}
IMP(dwmapi, DwmGetWindowAttribute) { if ((u32)ARG(1) == 14 && ARG(3) >= 4) { M32(ARG(2)) = 1; RET(S_OK); return; } RET(E_INVALIDARG); } // DWMWA_CLOAKED
// The keyboard: the desktop titles' one input state (rt/input.c: the page's keys, a session's), by virtual key.
// The high bit says the key is down; the low bit (pressed since the last call) is not kept, as on current Windows
// where it is not reliable either.
IMP(user32, GetAsyncKeyState) { input_poll(); int s = input_scan_of_vk((u32)ARG(0) & 0xff); RET(s >= 0 && input_keys()[s] ? 0x8000 : 0); }
IMP(user32, GetKeyState) { input_poll(); int s = input_scan_of_vk((u32)ARG(0) & 0xff); RET(s >= 0 && input_keys()[s] ? 0xFF80 : 0); }
