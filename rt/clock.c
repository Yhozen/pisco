// SPDX-License-Identifier: GPL-2.0-or-later
// The title's clock (rt.h).
#include <time.h>
#include "rt.h"

// The title's clock (rt.h): the host's, or with SW_CLOCK=<microseconds a frame> a virtual one that starts at a
// fixed date (2005-01-01 12:00 UTC) and moves only when the title presents a frame or sleeps.
static u64 clock_step, clock_virtual; static int clock_mode; // 0 not asked yet, 1 the host's, 2 virtual
static void clock_init(void) { const char *e = getenv("SW_CLOCK"); clock_step = e ? strtoull(e, 0, 10) : 0; clock_mode = clock_step ? 2 : 1; }
int rt_clock_virtual(void) { if (!clock_mode) clock_init(); return clock_mode == 2; }
u64 rt_now_us(void) {
  if (rt_clock_virtual()) return 1104580800000000ull + __atomic_load_n(&clock_virtual, __ATOMIC_SEQ_CST);
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (u64)t.tv_sec * 1000000 + (u64)t.tv_nsec / 1000;
}
__attribute__((weak)) void mixer_clock_moved(void);   // mixer.c, in the bases that have sound: under a virtual clock the sound is mixed up to the clock as it moves
static u32 clock_polls;   // reads of the clock since it last moved
void rt_clock_advance(u64 us) { if (rt_clock_virtual()) { clock_polls = 0; __atomic_add_fetch(&clock_virtual, us, __ATOMIC_SEQ_CST); if (mixer_clock_moved) mixer_clock_moved(); } }
// A title that waits for time by reading the clock in a loop (Tag's frame limiter: PeekMessage, QueryPerformanceCounter)
// would wait for ever on a virtual clock, which moves only when a frame is presented or the title sleeps: after 64
// reads in a row each read moves it on by a millisecond. A title that reads it a few times a frame is not affected.
void rt_clock_polled(void) { if (rt_clock_virtual() && ++clock_polls > 64) rt_clock_advance(1000); }
u64 rt_clock_virtual_us(void) { return __atomic_load_n(&clock_virtual, __ATOMIC_SEQ_CST); }   // the virtual clock since it started
void rt_clock_advance_frame(void) { rt_clock_advance(clock_step); }
// What the system has been up for: it started a minute before the first time anyone asked
u64 rt_uptime_us(void) { static u64 boot; u64 now = rt_now_us(); if (!boot) boot = now - 60000000; return now - boot; }
