// SPDX-License-Identifier: GPL-2.0-or-later
// What MSVC's static runtime would add: the type_info vtable (RTTI type descriptors point at it) and operator delete.
extern "C" const void *type_info_vftable[2] __asm__("??_7type_info@@6B@");
extern "C" const void *type_info_vftable[2] = {0, 0};
void operator delete(void *) noexcept {}
void operator delete(void *, unsigned long long) noexcept {}
extern "C" int _fltused = 0;
