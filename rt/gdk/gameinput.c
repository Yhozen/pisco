// SPDX-License-Identifier: GPL-2.0-or-later
// GameInput: the keyboard and the gamepad from the host's input block.
#include "gdk.h"

// ---- GameInput: an IGameInput whose methods are runtime functions. The host writes the keyboard's and the
// gamepad's state into an input block in shared memory (see input_open in the hosts); a reading is a snapshot
// of it. Slots follow GameInput's v0 interface order. ----
HOST("input_open") void host_input_open(u32 block);
// input block words: 0 sequence, 1 key count, 2..17 keys (virtual key | scan code << 8), 18 gamepad connected,
// 19 gamepad buttons, 20..25 floats (left trigger, right trigger, left x, left y, right x, right y)
static u32 *input_block;
static u64 gameinput, vt_reading;
#define KIND_KEYBOARD 0x10u
#define KIND_GAMEPAD 0x40000u
typedef struct { u64 vtbl; u32 magic, kind, seq, refs; u32 snap[32]; u64 time; } Reading;
static Reading *reading(u64 p) { Reading *r = (Reading *)GP(p); return p && r->magic == 0x52454144u ? r : 0; }
IMP(gameinput, IGameInput_QueryInterface) { M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(gameinput, IGameInput_AddRef) { RET(2); }
IMP(gameinput, IGameInput_Release) { RET(1); }
IMP(gameinput, IGameInput_GetCurrentTimestamp) { RET(now_us()); }
IMP(gameinput, IGameInput_GetCurrentReading) { // (kind, device, &reading)
  u32 kind = (u32)ARG(1); u64 out = ARG(3);
  if (out) M64(out) = 0;
  if (!input_block || !out) { RET(0x838A0003u); return; } // GAMEINPUT_E_READING_NOT_FOUND
  u32 snap[32]; memcpy(snap, input_block, sizeof snap);
  u32 k = kind & KIND_GAMEPAD ? KIND_GAMEPAD : kind & KIND_KEYBOARD ? KIND_KEYBOARD : 0;
  if (!k || (k == KIND_GAMEPAD && !snap[18])) { RET(0x838A0003u); return; }
  Reading *r = aligned_alloc(16, sizeof(Reading)); memset(r, 0, sizeof *r);
  r->vtbl = vt_reading; r->magic = 0x52454144u; r->kind = k; r->seq = snap[0]; r->refs = 1; r->time = now_us();
  memcpy(r->snap, snap, sizeof snap);
  M64(out) = (uptr)r; RET(S_OK);
}
IMP(gameinput, IGameInput_RegisterDeviceCallback) { // (device, kind, status filter, enumeration, context, callback, &token): no devices are announced
  u64 tok = ARG(7); if (tok) M64(tok) = 1; RET(S_OK);
}
IMP(gameinput, IGameInput_RegisterReadingCallback) { u64 tok = ARG(6); if (tok) M64(tok) = 2; RET(S_OK); }
IMP(gameinput, IGameInput_RegisterSystemButtonCallback) { u64 tok = ARG(5); if (tok) M64(tok) = 3; RET(S_OK); }
IMP(gameinput, IGameInput_RegisterKeyboardLayoutCallback) { u64 tok = ARG(4); if (tok) M64(tok) = 4; RET(S_OK); }
IMP(gameinput, IGameInput_StopCallback) {}
IMP(gameinput, IGameInput_UnregisterCallback) { RET(1); }
IMP(gameinput, IGameInput_SetFocusPolicy) {}
IMP(gameinput, IGameInput_Stub) { RET(E_NOTIMPL); }
IMP(gameinput, Reading_QueryInterface) { M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(gameinput, Reading_AddRef) { Reading *r = reading(ARG(0)); RET(r ? ++r->refs : 1); }
IMP(gameinput, Reading_Release) { Reading *r = reading(ARG(0)); if (!r) { RET(0); return; } u32 n = --r->refs; if (!n) { r->magic = 0; free(r); } RET(n); }
IMP(gameinput, Reading_GetInputKind) { Reading *r = reading(ARG(0)); RET(r ? r->kind : 0); }
IMP(gameinput, Reading_GetSequenceNumber) { Reading *r = reading(ARG(0)); RET(r ? r->seq : 0); }
IMP(gameinput, Reading_GetTimestamp) { Reading *r = reading(ARG(0)); RET(r ? r->time : 0); }
IMP(gameinput, Reading_GetDevice) { if (ARG(1)) M64(ARG(1)) = 0; }
IMP(gameinput, Reading_Zero) { RET(0); }
IMP(gameinput, Reading_GetKeyCount) { Reading *r = reading(ARG(0)); RET(r && r->kind == KIND_KEYBOARD ? (r->snap[1] > 16 ? 16 : r->snap[1]) : 0); }
IMP(gameinput, Reading_GetKeyState) { // (max count, GameInputKeyState[]) -> count; a key state: scan code, code point, virtual key, dead key
  Reading *r = reading(ARG(0)); u32 max = (u32)ARG(1), n = 0; u64 out = ARG(2);
  if (r && r->kind == KIND_KEYBOARD) for (u32 i = 0; i < r->snap[1] && i < 16 && n < max; i++, n++) {
    u32 k = r->snap[2 + i]; u64 e = out + 12ull * n;
    M32(e) = k >> 8 & 0xFFFF; M32(e + 4) = (k & 0xFF) >= 0x41 && (k & 0xFF) <= 0x5A ? (k & 0xFF) + 32 : 0; M8(e + 8) = (u8)k; M8(e + 9) = 0;
  }
  RET(n);
}
IMP(gameinput, Reading_GetGamepadState) { // (GameInputGamepadState*) -> bool
  Reading *r = reading(ARG(0)); u64 g = ARG(1);
  if (!r || r->kind != KIND_GAMEPAD || !g) { RET(0); return; }
  M32(g) = r->snap[19]; for (int i = 0; i < 6; i++) M32(g + 4 + 4 * i) = r->snap[20 + i];
  RET(1);
}
static u64 vt_of(const char *const *names, unsigned n, unsigned total, const char *rest) {
  u64 vt = (uptr)calloc(total, 8);
  for (unsigned i = 0; i < total; i++) M64(vt + 8 * i) = rt_dynamic_import("gameinput", i < n && names[i] ? names[i] : rest);
  return vt;
}
IMP(gameinput, GameInputCreate) {
  if (!gameinput) {
    static const char *const slots[] = {"IGameInput_QueryInterface", "IGameInput_AddRef", "IGameInput_Release", "IGameInput_GetCurrentTimestamp", "IGameInput_GetCurrentReading",
      0, 0, 0, "IGameInput_RegisterReadingCallback", "IGameInput_RegisterDeviceCallback", "IGameInput_RegisterSystemButtonCallback", "IGameInput_RegisterKeyboardLayoutCallback",
      "IGameInput_StopCallback", "IGameInput_UnregisterCallback", 0, 0, 0, 0, 0, 0, 0, "IGameInput_SetFocusPolicy"};
    static const char *const rslots[] = {"Reading_QueryInterface", "Reading_AddRef", "Reading_Release", "Reading_GetInputKind", "Reading_GetSequenceNumber", "Reading_GetTimestamp", "Reading_GetDevice",
      0, 0, 0, 0, 0, 0, 0, "Reading_GetKeyCount", "Reading_GetKeyState", 0, 0, 0, 0, 0, 0, "Reading_GetGamepadState"};
    u64 vt = vt_of(slots, sizeof slots / sizeof *slots, 64, "IGameInput_Stub");
    vt_reading = vt_of(rslots, sizeof rslots / sizeof *rslots, 32, "Reading_Zero");
    gameinput = (uptr)calloc(1, 16); M64(gameinput) = vt;
    input_block = aligned_alloc(64, 128); memset(input_block, 0, 128);
    host_input_open((u32)(uptr)input_block);
  }
  M64(ARG(0)) = gameinput; RET(S_OK);
}
