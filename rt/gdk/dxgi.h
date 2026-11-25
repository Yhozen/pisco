// SPDX-License-Identifier: GPL-2.0-or-later
// DXGI's objects (rt/gdk/dxgi.c), for the Windows x64 runtime's factory (rt/windows-x64/dxgi_swapchain.c).
#pragma once
#include "rt.h"
typedef struct { u64 vtbl; u32 magic; s32 refs; u64 device; } DxgiObj;
#define OBJECT_SLOTS "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent"
DxgiObj *rt_dxgi_object(const char *prefix, const char *const *names, unsigned n, u64 device); // a COM object of dxgi.dll's methods
u64 rt_dxgi_adapter(void);   // the one adapter, with a reference added
