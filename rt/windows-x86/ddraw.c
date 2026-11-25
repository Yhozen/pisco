// SPDX-License-Identifier: GPL-2.0-or-later
// DirectDraw, as far as titles of the Direct3D 9 era ask it how much video memory there is (the DirectX SDK's
// video-memory code, which Tag uses, tries it first: docs/tag.md): DirectDrawCreate / DirectDrawCreateEx give an
// object whose GetAvailableVidMem answers for a 256 MB card, as the device's caps describe it (d3d9.c). The
// interfaces IDirectDraw2 to 7 share that method's slot (23); every other method is a named stub.
#include "../rt.h"

void rt_import_set_pop(u64 addr, int bytes);
static u64 dd_object(void) {
  static u64 obj;
  if (obj) return obj;
  static const char *const names[] = {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes",
    "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem"};
  u64 vt = rt_com_vtable("ddraw", "DirectDraw_", names, sizeof names / sizeof *names);
  static const int pop[] = {12, 4, 4};   // QueryInterface, AddRef, Release
  for (int i = 0; i < 3; i++) rt_import_set_pop(MP(vt + PTR_SIZE * i), pop[i]);
  rt_import_set_pop(MP(vt + PTR_SIZE * 23), 16);   // GetAvailableVidMem(this, caps, &total, &free)
  obj = (uptr)calloc(1, 16); MP(obj) = vt;
  return obj;
}
IMP(ddraw, DirectDrawCreate) { if (!ARG(1)) { RET(0x80070057u); return; } MP(ARG(1)) = dd_object(); RET(0); }     // (guid, IDirectDraw **, outer)
IMP(ddraw, DirectDrawCreateEx) { if (!ARG(1)) { RET(0x80070057u); return; } MP(ARG(1)) = dd_object(); RET(0); }   // (guid, void **, iid, outer)
IMP(ddraw, DirectDraw_QueryInterface) { if (ARG(2)) MP(ARG(2)) = ARG(0); RET(0); }   // every DirectDraw interface: the same object
IMP(ddraw, DirectDraw_AddRef) { RET(2); }
IMP(ddraw, DirectDraw_Release) { RET(1); }
IMP(ddraw, DirectDraw_GetAvailableVidMem) { // (this, DDSCAPS / DDSCAPS2 *, &total, &free)
  if (ARG(2)) M32(ARG(2)) = 256u << 20;
  if (ARG(3)) M32(ARG(3)) = 240u << 20;
  RET(0);
}
