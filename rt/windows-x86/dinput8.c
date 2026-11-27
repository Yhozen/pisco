// SPDX-License-Identifier: GPL-2.0-or-later
// DirectInput 8 for Windows desktop titles: the system mouse and the system keyboard, read from the runtime's
// input state (input.c) as immediate data (GetDeviceState: DIMOUSESTATE, DIMOUSESTATE2, the 256-byte keyboard)
// and as buffered data (GetDeviceData, once the title has set a buffer size: the events since its last read). In the
// browser the page's keyboard and the mouse under Pointer Lock feed that state (input_poll, before each read).
// The title's window always has the focus, so a device is never lost. No game controller is attached.
// The data formats understood are the three standard ones (by their size); others are refused.
#include "com.h"
#include "win32.h"
#include "dinput8_layout.gen.h"

#define DI_OK 0u
#define S_FALSE 1u                        // DI_NOEFFECT, DI_BUFFEROVERFLOW, DI_POLLEDDEVICE
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_HANDLE 0x80070006u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_NOTBUFFERED 0x80040207u
#define DIERR_ACQUIRED 0x800700AAu
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_OLDDIRECTINPUTVERSION 0x8007047Eu
#define IID_IUnknown_text "00000000-0000-0000-c000-000000000046"
#define IID_IDirectInput8A_text "bf798030-483a-4da2-aa99-5d64ed369700"
#define IID_IDirectInput8W_text "bf798031-483a-4da2-aa99-5d64ed369700"
#define IID_IDirectInputDevice8A_text "54d41080-dc15-4833-a41b-748f73a38179"
#define GUID_SysMouse_text "6f1d2b60-d5a0-11cf-bfc7-444553540000"
#define GUID_SysKeyboard_text "6f1d2b61-d5a0-11cf-bfc7-444553540000"

typedef struct { u32 vtable; s32 refs; } DirectInput;
enum { MOUSE = 1, KEYBOARD = 2 };
typedef struct {
  u32 vtable; s32 refs; int kind;
  u32 data_size;                          // of the data format set: 16 or 20 (mouse), 256 (keyboard); 0 before SetDataFormat
  int relative;                           // axes as motion since the last read (the mouse's default) or as a position
  u32 cooperative, window;
  u32 buffer;                             // DIPROP_BUFFERSIZE: events kept for GetDeviceData
  int acquired;
  s32 x, y, wheel;                        // the mouse's position at the last GetDeviceState, for relative data
  u32 next_event;                         // the first event GetDeviceData has not returned yet
} Device;

// DirectInput8Create(instance, version, riid, void **, outer)
IMP(dinput8, DirectInput8Create) {
  u64 out = ARG(3);
  if (!out) { RET(E_POINTER); return; }
  M32(out) = 0;
  if ((u32)ARG(1) != 0x0800) { com_unsure("DirectInput8Create: version 0x%x is not DirectInput 8's", (u32)ARG(1)); RET(DIERR_OLDDIRECTINPUTVERSION); return; }
  if (com_is_iid(ARG(2), IID_IDirectInput8W_text)) { com_unsure("DirectInput: the Unicode interface (IDirectInput8W) is not implemented"); RET(E_NOINTERFACE); return; }
  if (!com_is_iid(ARG(2), IID_IDirectInput8A_text) && !com_is_iid(ARG(2), IID_IUnknown_text)) { RET(E_NOINTERFACE); return; }
  DirectInput *d = (DirectInput *)GP(com_new("dinput8", &IDirectInput8A_interface, sizeof(DirectInput)));
  d->refs = 1; M32(out) = (u32)(uptr)d; RET(DI_OK);
}
#define DI ((DirectInput *)GP(ARG(0)))
IMP(dinput8, IDirectInput8A_QueryInterface) {
  if (!ARG(2)) { RET(E_POINTER); return; }
  if (com_is_iid(ARG(1), IID_IDirectInput8A_text) || com_is_iid(ARG(1), IID_IUnknown_text)) { DI->refs++; M32(ARG(2)) = (u32)ARG(0); RET(DI_OK); return; }
  M32(ARG(2)) = 0; RET(E_NOINTERFACE);
}
IMP(dinput8, IDirectInput8A_AddRef) { RET(++DI->refs); }
IMP(dinput8, IDirectInput8A_Release) { DirectInput *d = DI; u32 n = (u32)--d->refs; if (!n) { d->vtable = 0; free(d); } RET(n); }
IMP(dinput8, IDirectInput8A_Initialize) { RET(DI_OK); }
// CreateDevice(GUID, IDirectInputDevice8A **, outer)
IMP(dinput8, IDirectInput8A_CreateDevice) {
  u64 out = ARG(2);
  if (!out || !ARG(1)) { RET(E_POINTER); return; }
  M32(out) = 0;
  int kind = com_is_iid(ARG(1), GUID_SysMouse_text) ? MOUSE : com_is_iid(ARG(1), GUID_SysKeyboard_text) ? KEYBOARD : 0;
  if (!kind) { RET(DIERR_DEVICENOTREG); return; }   // no other device exists
  Device *d = (Device *)GP(com_new("dinput8", &IDirectInputDevice8A_interface, sizeof(Device)));
  d->refs = 1; d->kind = kind; d->relative = kind == MOUSE;
  M32(out) = (u32)(uptr)d; RET(DI_OK);
}
// EnumDevices(type, callback, context, flags): there are no game controllers (DI8DEVCLASS_GAMECTRL 4, or the
// joystick types 0x14..0x1C), so nothing is reported for them. Listing the mouse and the keyboard is not implemented.
IMP(dinput8, IDirectInput8A_EnumDevices) {
  u32 type = (u32)ARG(1);
  if (type == 4 || (type >= 0x14 && type <= 0x1C)) { RET(DI_OK); return; }
  com_unsure("DirectInput: EnumDevices of device type %u is not implemented: reporting no device", type); RET(DI_OK);
}
IMP(dinput8, IDirectInput8A_GetDeviceStatus) { RET(com_is_iid(ARG(1), GUID_SysMouse_text) || com_is_iid(ARG(1), GUID_SysKeyboard_text) ? DI_OK : S_FALSE); } // attached, or DI_NOTATTACHED

#define DEV ((Device *)GP(ARG(0)))
IMP(dinput8, IDirectInputDevice8A_QueryInterface) {
  if (!ARG(2)) { RET(E_POINTER); return; }
  if (com_is_iid(ARG(1), IID_IDirectInputDevice8A_text) || com_is_iid(ARG(1), IID_IUnknown_text)) { DEV->refs++; M32(ARG(2)) = (u32)ARG(0); RET(DI_OK); return; }
  M32(ARG(2)) = 0; RET(E_NOINTERFACE);
}
IMP(dinput8, IDirectInputDevice8A_AddRef) { RET(++DEV->refs); }
IMP(dinput8, IDirectInputDevice8A_Release) { Device *d = DEV; u32 n = (u32)--d->refs; if (!n) { d->vtable = 0; free(d); } RET(n); }
// SetDataFormat(DIDATAFORMAT *): size 24, object size 16, flags (DIDF_ABSAXIS 1, DIDF_RELAXIS 2), data size, objects, their list
IMP(dinput8, IDirectInputDevice8A_SetDataFormat) {
  Device *d = DEV; u64 f = ARG(1);
  if (!f) { RET(E_POINTER); return; }
  if (d->acquired) { RET(DIERR_ACQUIRED); return; }
  if (M32(f) != 24 || M32(f + 4) != 16) { RET(DIERR_INVALIDPARAM); return; }
  u32 size = M32(f + 12);
  if (d->kind == MOUSE ? (size != 16 && size != 20) : size != 256) {
    com_unsure("DirectInput: a data format of %u bytes for the %s is not one of the standard ones", size, d->kind == MOUSE ? "mouse" : "keyboard"); RET(DIERR_INVALIDPARAM); return;
  }
  d->data_size = size; if (d->kind == MOUSE) d->relative = !(M32(f + 8) & 1);
  RET(DI_OK);
}
// SetCooperativeLevel(window, flags): EXCLUSIVE 1 or NONEXCLUSIVE 2, with FOREGROUND 4 or BACKGROUND 8; NOWINKEY 0x10
IMP(dinput8, IDirectInputDevice8A_SetCooperativeLevel) {
  Device *d = DEV; u32 flags = (u32)ARG(2); s32 w, h;
  if (d->acquired) { RET(DIERR_ACQUIRED); return; }
  if (!!(flags & 1) == !!(flags & 2) || !!(flags & 4) == !!(flags & 8)) { RET(DIERR_INVALIDPARAM); return; }
  if (!win32_client_size((u32)ARG(1), &w, &h)) { RET(E_HANDLE); return; }
  d->cooperative = flags; d->window = (u32)ARG(1); RET(DI_OK);
}
// SetProperty(property, DIPROPHEADER *). The property is a small number in place of a GUID's address:
// DIPROP_BUFFERSIZE 1 and DIPROP_AXISMODE 2 carry a number after the 16-byte header (a DIPROPDWORD, 20 bytes).
IMP(dinput8, IDirectInputDevice8A_SetProperty) {
  Device *d = DEV; u32 prop = (u32)ARG(1); u64 h = ARG(2);
  if (!h || M32(h + 4) != 16) { RET(DIERR_INVALIDPARAM); return; }
  if (prop == 1 || prop == 2) {
    if (M32(h) != 20) { RET(DIERR_INVALIDPARAM); return; }
    if (d->acquired) { RET(DIERR_ACQUIRED); return; }
    if (prop == 1) d->buffer = M32(h + 16); else d->relative = M32(h + 16) == 1;   // DIPROPAXISMODE_ABS 0, _REL 1
    RET(DI_OK); return;
  }
  com_unsure("DirectInput: SetProperty of property %u is not implemented", prop); RET(DIERR_UNSUPPORTED);
}
IMP(dinput8, IDirectInputDevice8A_GetProperty) {
  Device *d = DEV; u32 prop = (u32)ARG(1); u64 h = ARG(2);
  if (!h || M32(h + 4) != 16) { RET(DIERR_INVALIDPARAM); return; }
  if ((prop == 1 || prop == 2) && M32(h) == 20) { M32(h + 16) = prop == 1 ? d->buffer : (u32)d->relative; RET(DI_OK); return; }
  com_unsure("DirectInput: GetProperty of property %u is not implemented", prop); RET(DIERR_UNSUPPORTED);
}
// Acquire: needs a data format; a second Acquire has no effect. What moved before does not count afterwards.
IMP(dinput8, IDirectInputDevice8A_Acquire) {
  Device *d = DEV;
  input_poll();   // what the page sent since
  if (!d->data_size) { RET(DIERR_INVALIDPARAM); return; }
  if (d->acquired) { RET(S_FALSE); return; }
  d->acquired = 1; d->next_event = input_event_count(); input_mouse_position(&d->x, &d->y, &d->wheel);
  if (d->kind == MOUSE && (d->cooperative & 1)) input_mouse_exclusive++;   // DISCL_EXCLUSIVE: the windows get no mouse messages
  RET(DI_OK);
}
IMP(dinput8, IDirectInputDevice8A_Unacquire) {
  Device *d = DEV; u32 was = (u32)d->acquired; d->acquired = 0;
  if (was && d->kind == MOUSE && (d->cooperative & 1) && input_mouse_exclusive > 0) input_mouse_exclusive--;
  RET(was ? DI_OK : S_FALSE);
}
IMP(dinput8, IDirectInputDevice8A_Poll) { RET(DEV->acquired ? S_FALSE : DIERR_NOTACQUIRED); }   // neither device needs polling
// GetDeviceState(size, data): the state now
IMP(dinput8, IDirectInputDevice8A_GetDeviceState) {
  Device *d = DEV; u64 out = ARG(2);
  input_poll();   // what the page sent since
  if (!out) { RET(E_POINTER); return; }
  if (!d->acquired) { RET(DIERR_NOTACQUIRED); return; }
  if ((u32)ARG(1) != d->data_size) { RET(DIERR_INVALIDPARAM); return; }
  if (d->kind == KEYBOARD) { memcpy((void *)GP(out), input_keys(), 256); RET(DI_OK); return; }
  s32 x, y, wheel; input_mouse_position(&x, &y, &wheel);   // DIMOUSESTATE: x, y, wheel, then 4 buttons (DIMOUSESTATE2: 8)
  M32(out) = (u32)(d->relative ? x - d->x : x); M32(out + 4) = (u32)(d->relative ? y - d->y : y); M32(out + 8) = (u32)(d->relative ? wheel - d->wheel : wheel);
  d->x = x; d->y = y; d->wheel = wheel;
  memcpy((void *)GP(out + 12), input_mouse_buttons(), d->data_size - 12);
  RET(DI_OK);
}
// GetDeviceData(size of an item, items or null, count in / out, flags): the events since the last read, as
// DIDEVICEOBJECTDATA (offset in the data format, data, time stamp, sequence, application data: 20 bytes; 16
// before DirectInput 8). With no room for items they are only counted; DIGDD_PEEK (flag 1) leaves them unread.
// A device keeps `buffer` events: if more happened since the last read the oldest are lost and it says so.
IMP(dinput8, IDirectInputDevice8A_GetDeviceData) {
  Device *d = DEV; u32 item = (u32)ARG(1); u64 out = ARG(2), count = ARG(3);
  input_poll();   // what the page sent since
  if (!count) { RET(E_POINTER); return; }
  if (item != 20 && item != 16) { RET(DIERR_INVALIDPARAM); return; }
  if (!d->acquired) { RET(DIERR_NOTACQUIRED); return; }
  if (!d->buffer) { RET(DIERR_NOTBUFFERED); return; }
  u32 room = M32(count), given = 0, end = input_event_count(), at = d->next_event, mine = 0, lost = 0;
  for (u32 i = at; i < end; i++) { const InputEvent *e = input_event(i); if (e && (e->kind == INPUT_KEY) == (d->kind == KEYBOARD)) mine++; }
  for (; at < end && (!out || given < room); at++) {
    const InputEvent *e = input_event(at);
    if (!e || (e->kind == INPUT_KEY) != (d->kind == KEYBOARD)) continue;
    if (mine-- > d->buffer) { lost = 1; continue; }   // older than the buffer holds
    if (out) {
      u64 p = out + (u64)item * given;
      M32(p) = e->kind == INPUT_KEY ? e->code : e->kind == INPUT_AXIS ? 4u * e->code : 12u + e->code;
      M32(p + 4) = e->kind == INPUT_AXIS ? (u32)(d->relative ? e->value : e->position) : e->value ? 0x80u : 0;
      M32(p + 8) = e->time_ms; M32(p + 12) = e->sequence; if (item == 20) M32(p + 16) = 0xFFFFFFFFu;
    }
    given++;
  }
  M32(count) = given;
  if (!(ARG(4) & 1)) d->next_event = at;
  RET(lost ? S_FALSE : DI_OK);
}
