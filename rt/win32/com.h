// SPDX-License-Identifier: GPL-2.0-or-later
// COM objects whose methods are runtime functions, for a 32-bit guest: every method is stdcall with `this` as
// its first argument, so each vtable slot has to know how many bytes of arguments its caller pushed.
// The layouts (*_layout.gen.h) come from tools/gen-com-layout.py.
#pragma once
#include "../rt.h"

typedef struct { const char *name; unsigned char bytes; } ComMethod;
typedef struct { const char *name, *iid; const ComMethod *methods; unsigned count; } ComInterface;

// The vtable of `iface` served by `dll` ("d3d9"), made once: slot i calls IMP(dll, <Interface>_<Method>) where the
// runtime has it, and otherwise a stub named "dll.dll!<Interface>::<Method>" that logs (or stops, in strict
// mode). Either way the caller's arguments are removed.
u32 com_vtable(const char *dll, const ComInterface *iface);
// A new object: `size` zeroed bytes of guest memory whose first word is the vtable; the reference count is the
// caller's to keep (objects usually put it second). Returns its address.
u32 com_new(const char *dll, const ComInterface *iface, u32 size);
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" against the 16 bytes of a GUID in guest memory
int com_is_iid(u64 guid, const char *text);
// SW_STRICT=1: a question the runtime cannot answer for certain stops the run instead of being guessed at
int com_strict(void);
// For such a question: stops in strict mode with the message, otherwise logs it (the caller then gives its fallback answer)
void com_unsure(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
