// SPDX-License-Identifier: GPL-2.0-or-later
// libScePad: the DualSense, from the host's input block (the page writes the first gamepad and the keys held into
// it: web/browser/main.js; rt/gdk/gameinput.c reads the same block). A standard gamepad maps button for button; on
// the keyboard: WASD the left stick, the arrow keys the right stick, Space R2, Enter Circle, C Cross, X Square,
// T Triangle, Q / E L1 / R1, Z / V the d-pad left / right, H L3, M Options, P the touchpad.
#include "ps5.h"

static u32 *block;
// DualSense buttons (ScePadButtonDataOffset)
enum { L3 = 0x2, R3 = 0x4, OPTIONS = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80, L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800,
       TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000, TOUCHPAD = 0x100000 };
IMP(libscepad, scePadInit) {
  if (!block) { block = aligned_alloc(64, 128); memset(block, 0, 128); host_input_open((u32)(uptr)block); }
  RET(0);
}
IMP(libscepad, scePadOpen) { RET(1); }
IMP(libscepad, scePadClose) { RET(0); }
IMP(libscepad, scePadSetMotionSensorState) { RET(0); }
IMP(libscepad, scePadSetVibration) { RET(0); }
IMP(libscepad, scePadSetLightBar) { RET(0); }
static u8 axis(float v) { int x = (int)(128.0f + v * 127.0f); return (u8)(x < 0 ? 0 : x > 255 ? 255 : x); }
// (handle, ScePadData*): buttons (4 bytes), sticks lx ly rx ry, triggers l2 r2, orientation (quaternion), acceleration,
// angular velocity (from byte 40), touch data, connected
IMP(libscepad, scePadReadState) {
  u64 d = A1; memset((void *)GP(d), 0, 0x78);
  u32 snap[32]; if (block) memcpy(snap, block, sizeof snap); else memset(snap, 0, sizeof snap);
  float lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0; u32 b = 0;
  if (snap[18]) {   // GameInput's gamepad bits: menu 1, view 2, a 4, b 8, x 0x10, y 0x20, d-pad 0x40.., shoulders 0x400 0x800, sticks 0x1000 0x2000
    u32 g = snap[19]; float f[6]; memcpy(f, &snap[20], sizeof f);
    static const u32 map[][2] = { {1, OPTIONS}, {2, TOUCHPAD}, {4, CROSS}, {8, CIRCLE}, {0x10, SQUARE}, {0x20, TRIANGLE}, {0x40, UP}, {0x80, DOWN},
      {0x100, LEFT}, {0x200, RIGHT}, {0x400, L1}, {0x800, R1}, {0x1000, L3}, {0x2000, R3} };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (g & map[i][0]) b |= map[i][1];
    l2 = f[0]; r2 = f[1]; lx = f[2]; ly = -f[3]; rx = f[4]; ry = -f[5];
  }
  u32 n = snap[1] > 16 ? 16 : snap[1];
  for (u32 i = 0; i < n; i++) switch (snap[2 + i] & 0xFF) {   // virtual key codes
    case 'W': ly = -1; break; case 'S': ly = 1; break; case 'A': lx = -1; break; case 'D': lx = 1; break;
    case 0x26: ry = -1; break; case 0x28: ry = 1; break; case 0x25: rx = -1; break; case 0x27: rx = 1; break;
    case ' ': r2 = 1; break; case 0x0D: b |= CIRCLE; break; case 'C': b |= CROSS; break; case 'X': b |= SQUARE; break;
    case 'T': b |= TRIANGLE; break; case 'Q': b |= L1; break; case 'E': b |= R1; break; case 'Z': b |= LEFT; break;
    case 'V': b |= RIGHT; break; case 'H': b |= L3; break; case 'M': b |= OPTIONS; break; case 'P': b |= TOUCHPAD; break;
  }
  if (l2 > 0.1f) b |= L2;
  if (r2 > 0.1f) b |= R2;
  M32(d) = b;
  M8(d + 4) = axis(lx); M8(d + 5) = axis(ly); M8(d + 6) = axis(rx); M8(d + 7) = axis(ry);
  M8(d + 8) = (u8)(l2 * 255.0f); M8(d + 9) = (u8)(r2 * 255.0f);
  MF32(d + 24) = 1.0f;   // orientation w: at rest
  MF32(d + 32) = -1.0f;  // acceleration: gravity, the pad lying flat (y)
  M8(d + 0x4C) = 1;      // connected
  RET(0);
}
IMP(libscepad, scePadRead) { RET(0); }
