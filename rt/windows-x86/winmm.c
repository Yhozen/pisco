// SPDX-License-Identifier: GPL-2.0-or-later
// winmm for Windows desktop titles: the multimedia timer's clock, which is the title's clock (rt.h).
#include "../rt.h"

IMP(winmm, timeGetTime) { RET((u32)(rt_uptime_us() / 1000)); }   // milliseconds since the system started
