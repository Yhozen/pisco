// SPDX-License-Identifier: GPL-2.0-or-later
// Input for Windows desktop titles: one state (keys by DirectInput scan code, the mouse's motion and buttons) with
// the events that changed it, read by DirectInput (rt/windows-x86/dinput8.c) and by user32's GetAsyncKeyState
// (rt/gdk/window.c, through a table of virtual keys). And the session player, which feeds that state
// from a session script on the title's clock, so a run can be driven without a person and repeated exactly.
//
// A session (tools/oracle-wine/README.md has the format) reaches the runtime as the host's SW_SESSION_STEPS
// (web/session.mjs makes it from the JSON file SW_SESSION names): steps separated by ';', each one of
//   wait <ms> | focus | move <dx> <dy> <ms> | click <button> <ms> | button <button> <0|1>
//   key <name> <ms> | keydown <name> | keyup <name> | shot <name>
// A step starts when the one before it ends; `ms` 0 means the oracle's player's own default (150 ms for a click,
// 120 ms for a key, 30 ms for every 12 pixels of a move). `shot` writes "checkpoint <name> frame <n>" with the
// Direct3D calls made since the checkpoint before, and marks the frame just presented for the host to save
// (rt_session_shot: Direct3D 9's d3d9_gpu_shot, DXGI's in rt/windows-x64/dxgi_swapchain.c): where the oracle saves a
// screenshot.
#include "input.h"

void rt_exit_process(u32 code);   // msvcrt.c

// ---- the state and its events ----
static u8 keys[256], buttons[8];
static s32 mouse_x, mouse_y, mouse_wheel;
enum { EVENTS = 1024 };
static InputEvent events[EVENTS]; static u32 event_count, sequence;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void add(u8 kind, u8 code, s32 value) {
  InputEvent *e = &events[event_count % EVENTS];
  e->kind = kind; e->code = code; e->value = value; e->time_ms = (u32)(rt_uptime_us() / 1000); e->sequence = sequence;
  e->position = kind != INPUT_AXIS ? 0 : code == 0 ? mouse_x : code == 1 ? mouse_y : mouse_wheel;
  __atomic_store_n(&event_count, event_count + 1, __ATOMIC_RELEASE);
}
void input_key(u32 scan, int down) {
  if (scan > 255) return;
  pthread_mutex_lock(&lock);
  if ((keys[scan] != 0) != (down != 0)) { keys[scan] = down ? 0x80 : 0; sequence++; add(INPUT_KEY, (u8)scan, down != 0); }
  pthread_mutex_unlock(&lock);
}
void input_mouse_move(s32 dx, s32 dy, s32 wheel) {
  if (!dx && !dy && !wheel) return;
  pthread_mutex_lock(&lock);
  sequence++; mouse_x += dx; mouse_y += dy; mouse_wheel += wheel;
  if (dx) add(INPUT_AXIS, 0, dx); if (dy) add(INPUT_AXIS, 1, dy); if (wheel) add(INPUT_AXIS, 2, wheel);
  pthread_mutex_unlock(&lock);
}
void input_mouse_button(u32 button, int down) {
  if (button > 7) return;
  pthread_mutex_lock(&lock);
  if ((buttons[button] != 0) != (down != 0)) { buttons[button] = down ? 0x80 : 0; sequence++; add(INPUT_BUTTON, (u8)button, down != 0); }
  pthread_mutex_unlock(&lock);
}
int input_mouse_exclusive;
const u8 *input_keys(void) { return keys; }
const u8 *input_mouse_buttons(void) { return buttons; }
void input_mouse_position(s32 *x, s32 *y, s32 *wheel) { pthread_mutex_lock(&lock); *x = mouse_x; *y = mouse_y; *wheel = mouse_wheel; pthread_mutex_unlock(&lock); }
u32 input_event_count(void) { return __atomic_load_n(&event_count, __ATOMIC_ACQUIRE); }
const InputEvent *input_event(u32 number) { u32 n = input_event_count(); return number < n && n - number <= EVENTS ? &events[number % EVENTS] : 0; }

// ---- input from the page ----
// A ring of events in shared memory: words [0] events written (by the page), [1] events read (here), [2] capacity;
// from word 4, four words an event: kind, a, b, 0. Kinds: 1 key (a: DirectInput scan code, b: down), 2 motion
// (a: dx, b: dy, in pixels), 3 button (a: 0 left, 1 right, 2 middle, 3 / 4 the side buttons; b: down), 4 wheel
// (a: 120 a notch, positive away from the user). Node has no page: its ring stays empty.
HOST("win32_input_open") void host_win32_input_open(u32 ring);
enum { RING_EVENTS = 512 };
static u32 *ring;
void input_poll(void) {
  if (!ring) { ring = calloc(4 + 4 * RING_EVENTS, 4); ring[2] = RING_EVENTS; host_win32_input_open((u32)(uptr)ring); return; }
  u32 w = __atomic_load_n(&ring[0], __ATOMIC_ACQUIRE), r = ring[1];
  for (; r != w; r++) {
    const s32 *e = (const s32 *)&ring[4 + 4 * (r % RING_EVENTS)];
    switch (e[0]) {
    case 1: input_key((u32)e[1], e[2]); break;
    case 2: input_mouse_move(e[1], e[2], 0); break;
    case 3: input_mouse_button((u32)e[1], e[2]); break;
    case 4: input_mouse_move(0, 0, e[1]); break;
    }
  }
  __atomic_store_n(&ring[1], r, __ATOMIC_RELEASE);
}

// ---- the session player ----
enum { S_WAIT, S_FOCUS, S_MOVE, S_CLICK, S_BUTTON, S_KEY, S_KEYDOWN, S_KEYUP, S_SHOT };
typedef struct { int kind; s32 a, b; u64 us; char name[40]; } Step;
static Step *steps; static unsigned step_count, step_at; static int loaded, started;
static u64 step_start; static s32 moved_x, moved_y;

static int button_of(const char *s) { return !strcmp(s, "left") ? 0 : !strcmp(s, "right") ? 1 : !strcmp(s, "middle") ? 2 : -1; }
// A key by the name a session gives it (an X keysym name, or one of the oracle's player's own) -> its scan code
static int scan_of(const char *s) {
  static const struct { const char *name; u8 scan; } named[] = {{"space", 0x39}, {"enter", 0x1C}, {"Return", 0x1C}, {"esc", 0x01}, {"escape", 0x01}, {"Escape", 0x01}, {"tab", 0x0F}, {"Tab", 0x0F},
    {"shift", 0x2A}, {"Shift_L", 0x2A}, {"ctrl", 0x1D}, {"Control_L", 0x1D}, {"up", 0xC8}, {"Up", 0xC8}, {"down", 0xD0}, {"Down", 0xD0}, {"left", 0xCB}, {"Left", 0xCB}, {"right", 0xCD}, {"Right", 0xCD},
    {"grave", 0x29}, {"`", 0x29}, {"~", 0x29}, {" ", 0x39},
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58}};
  static const char rows[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";   // scan codes 0x02..0x35, row by row
  for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) if (!strcmp(named[i].name, s)) return named[i].scan;
  if (s[0] && !s[1]) { char ch = s[0] >= 'A' && s[0] <= 'Z' ? s[0] + 32 : s[0]; for (unsigned i = 0; i < sizeof rows - 1; i++) if (rows[i] == ch) return 2 + (int)i; }
  return -1;
}
static void load(void) {
  loaded = 1;
  const char *text = getenv("SW_SESSION_STEPS");
  if (!text || !*text) return;
  char *copy = strdup(text), *save = 0;
  for (char *part = strtok_r(copy, ";", &save); part; part = strtok_r(0, ";", &save)) {
    Step s = {0}; char word[16] = "", name[40] = ""; long a = 0, b = 0, ms = 0; int ok = 0;
    if (sscanf(part, " %15s", word) != 1) continue;
    if (!strcmp(word, "wait")) { ok = sscanf(part, " %*s %ld", &ms) == 1; s.kind = S_WAIT; }
    else if (!strcmp(word, "focus")) { ok = 1; s.kind = S_FOCUS; }
    else if (!strcmp(word, "move")) { ok = sscanf(part, " %*s %ld %ld %ld", &a, &b, &ms) == 3; s.kind = S_MOVE; if (ok && !ms) { long m = labs(a) > labs(b) ? labs(a) : labs(b); ms = 30 * (m / 12 > 1 ? m / 12 : 1); } }
    else if (!strcmp(word, "click")) { ok = sscanf(part, " %*s %39s %ld", name, &ms) == 2 && (a = button_of(name)) >= 0; s.kind = S_CLICK; if (!ms) ms = 150; }
    else if (!strcmp(word, "button")) { ok = sscanf(part, " %*s %39s %ld", name, &b) == 2 && (a = button_of(name)) >= 0; s.kind = S_BUTTON; }
    else if (!strcmp(word, "key")) { ok = sscanf(part, " %*s %39s %ld", name, &ms) == 2 && (a = scan_of(name)) >= 0; s.kind = S_KEY; if (!ms) ms = 120; }
    else if (!strcmp(word, "keydown") || !strcmp(word, "keyup")) { ok = sscanf(part, " %*s %39s", name) == 1 && (a = scan_of(name)) >= 0; s.kind = word[3] == 'd' ? S_KEYDOWN : S_KEYUP; }
    else if (!strcmp(word, "shot")) { ok = sscanf(part, " %*s %39s", name) == 1; s.kind = S_SHOT; }
    if (!ok) rt_fatal("session: cannot read the step \"%s\"", part);
    s.a = (s32)a; s.b = (s32)b; s.us = 1000ull * (u64)ms; snprintf(s.name, sizeof s.name, "%s", name);
    steps = realloc(steps, (step_count + 1) * sizeof *steps); steps[step_count++] = s;
  }
  free(copy);
  if (step_count) rt_log("session: %u steps", step_count);
}
void session_frame(u32 frame) {
  if (!loaded) { load(); step_start = rt_now_us(); }   // the session's time starts with the first frame
  if (!step_count) return;
  u64 now = rt_now_us();
  for (;;) {
    if (step_at == step_count) { rt_log("session done: %u steps, %u frames", step_count, frame); rt_exit_process(0); }
    const Step *s = &steps[step_at];
    if (!started) { // what happens when a step begins
      started = 1; moved_x = moved_y = 0;
      switch (s->kind) {
      case S_CLICK: input_mouse_button((u32)s->a, 1); break;
      case S_BUTTON: input_mouse_button((u32)s->a, s->b); break;
      case S_KEY: case S_KEYDOWN: input_key((u32)s->a, 1); break;
      case S_KEYUP: input_key((u32)s->a, 0); break;
      case S_SHOT: rt_session_shot(s->name, frame); break;
      }
    }
    u64 passed = now - step_start; if (passed > s->us) passed = s->us;
    if (s->kind == S_MOVE) { // as much of the motion as the time that has passed
      s32 x = s->us ? (s32)((s64)s->a * (s64)passed / (s64)s->us) : s->a, y = s->us ? (s32)((s64)s->b * (s64)passed / (s64)s->us) : s->b;
      input_mouse_move(x - moved_x, y - moved_y, 0); moved_x = x; moved_y = y;
    }
    if (now - step_start < s->us) return;   // it is still going on
    if (s->kind == S_CLICK) input_mouse_button((u32)s->a, 0);
    if (s->kind == S_KEY) input_key((u32)s->a, 0);
    step_start += s->us; step_at++; started = 0;
  }
}

void (*rt_session_shot_hook)(const char *name, u32 frame);
__attribute__((weak)) void rt_session_shot(const char *name, u32 frame) { if (rt_session_shot_hook) rt_session_shot_hook(name, frame); else rt_log("checkpoint %s frame %u", name, frame); }

// ---- virtual keys: what user32 calls the keys the state holds by scan code (a US keyboard) ----
u32 input_vk_of_scan(u32 scan) {
  static const char rows[] = "1234567890-=\0\0QWERTYUIOP[]\0\0ASDFGHJKL;'`\0\\ZXCVBNM,./";   // scan codes 0x02..0x35
  if (scan >= 0x02 && scan <= 0x35) { char ch = rows[scan - 2]; if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) return (u32)ch; }
  if (scan >= 0x3B && scan <= 0x44) return 0x70 + (scan - 0x3B);   // F1..F10
  switch (scan) {
  case 0x01: return 0x1B; case 0x0E: return 0x08; case 0x0F: return 0x09; case 0x1C: return 0x0D; case 0x39: return 0x20;   // Esc, Backspace, Tab, Enter, Space
  case 0x2A: return 0xA0; case 0x36: return 0xA1; case 0x1D: return 0xA2; case 0x9D: return 0xA3; case 0x38: return 0xA4; case 0xB8: return 0xA5; // shifts, controls, alts (left / right)
  case 0x57: return 0x7A; case 0x58: return 0x7B;   // F11, F12
  case 0xC8: return 0x26; case 0xD0: return 0x28; case 0xCB: return 0x25; case 0xCD: return 0x27;   // the arrows
  case 0xC7: return 0x24; case 0xCF: return 0x23; case 0xC9: return 0x21; case 0xD1: return 0x22; case 0xD2: return 0x2D; case 0xD3: return 0x2E; // Home End PgUp PgDn Ins Del
  case 0x0C: return 0xBD; case 0x0D: return 0xBB; case 0x1A: return 0xDB; case 0x1B: return 0xDD; case 0x27: return 0xBA; case 0x28: return 0xDE;
  case 0x29: return 0xC0; case 0x2B: return 0xDC; case 0x33: return 0xBC; case 0x34: return 0xBE; case 0x35: return 0xBF;
  }
  return 0;
}
int input_scan_of_vk(u32 vk) {
  static s16 table[256]; static int ready;
  if (!ready) { for (int i = 0; i < 256; i++) table[i] = -1; for (u32 s = 255; s > 0; s--) { u32 v = input_vk_of_scan(s); if (v && v < 256) table[v] = (s16)s; } ready = 1; }
  if (vk >= 256) return -1;
  if (vk == 0x10) return input_keys()[0x2A] ? 0x2A : 0x36;   // VK_SHIFT, VK_CONTROL, VK_MENU: either side
  if (vk == 0x11) return input_keys()[0x1D] ? 0x1D : 0x9D;
  if (vk == 0x12) return input_keys()[0x38] ? 0x38 : 0xB8;
  return table[vk];
}
