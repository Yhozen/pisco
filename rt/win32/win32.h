// SPDX-License-Identifier: GPL-2.0-or-later
// What the parts of the Windows desktop layer (rt/win32/) ask of one another.
#pragma once
#include "../rt.h"

// user32.c: the client area of a window; 0 if there is no such window
int win32_client_size(u32 hwnd, s32 *width, s32 *height);
// gdi32.c: FillRect's work (user32 exports the function)
int gdi_fill_rect(u32 hdc, const s32 *rect, u32 brush);

// ---- input.c: the one input state DirectInput (and later user32) reads ----
// Keys are DirectInput scan codes (DIK_*); mouse buttons 0 left, 1 right, 2 middle, ...
// Whoever has input to give (the session player, the page's keyboard and mouse) calls the first three.
void input_key(u32 scan, int down);
void input_mouse_move(s32 dx, s32 dy, s32 wheel);       // relative motion
void input_mouse_button(u32 button, int down);
// What the page sent since the last call (its keyboard, the mouse under Pointer Lock: web/browser/main.js), into
// the state: DirectInput calls it before it reads. The first call opens the page's event ring.
void input_poll(void);
// The state: 256 keys and 8 buttons, 0x80 where one is down; the mouse's position as the sum of all motion
const u8 *input_keys(void);
const u8 *input_mouse_buttons(void);
void input_mouse_position(s32 *x, s32 *y, s32 *wheel);
// The same as events, numbered from 0 in the order they happened; the last 1024 are kept. Events made by one
// act (the x and y of one motion) share a sequence number.
enum { INPUT_KEY = 1, INPUT_AXIS, INPUT_BUTTON };
typedef struct { u8 kind, code; s32 value, position; u32 time_ms, sequence; } InputEvent;   // code: the scan code, the axis (0 x, 1 y, 2 wheel) or the button; value: 1 / 0 down / up, or the motion (position: where the axis is after it)
u32 input_event_count(void);
const InputEvent *input_event(u32 number);              // null when it is too old to have been kept

// ---- the session player (input.c; the host's SW_SESSION) ----
// Plays the steps of a session script that are due on the title's clock: called once a frame, after the frame was
// presented. When the script ends it logs "session done" and the process exits with code 0.
void session_frame(u32 frame);
