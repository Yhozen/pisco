// SPDX-License-Identifier: GPL-2.0-or-later
// ole32 for Windows desktop apps: COM starts and stops, and no class is registered (the DirectX SDK's check for
// XInput pads asks WMI, which is not there: the caller's fallback runs, as on a machine without WMI).
#include "../rt.h"

IMP(ole32, CoInitialize) { RET(0); }            // S_OK
IMP(ole32, CoInitializeEx) { RET(0); }
IMP(ole32, CoUninitialize) {}
IMP(ole32, CoCreateInstance) { if (ARG(4)) MP(ARG(4)) = 0; RET(0x80040154u); } // (clsid, outer, context, iid, &object): REGDB_E_CLASSNOTREG
