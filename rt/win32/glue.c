// SPDX-License-Identifier: GPL-2.0-or-later
// What the core runtime calls in the console's libraries, for the profile that does not link them.
#include "../rt.h"

int rt_gfx_free(u64 addr) { (void)addr; return 0; } // VirtualFree asks whether an address is graphics memory: a desktop title has none
