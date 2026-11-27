// SPDX-License-Identifier: GPL-2.0-or-later
// The machine's port devices: a 16550 serial port at 0x3f8 on the IOAPIC's pin 4: its output is the title's
// standard output, its input what the host writes in the console ring (keys typed in a terminal or the page, or
// a pipe), an exit port at 0xf4 as QEMU's isa-debug-exit (a byte written there ends the machine
// with that status), the CMOS clock (ports 0x70 and 0x71: the virtual clock's date, from 2005-01-01 12:00 UTC as
// rt/clock.c's, in BCD and 24 hours), the keyboard controller's and port 0xcf9's resets (the machine ends with 125, as the oracle
// says of a machine that did not end at the exit port). There are no 8259s: their ports read as ones, and a
// kernel that probes them finds none. A port nothing answers reads as ones and is said once with SW_TRACE.
#include <unistd.h>
#include "machine.h"

static char out[4096]; static unsigned out_n;
void uart_flush(void) { unsigned k = 0; while (k < out_n) { ssize_t w = write(1, out + k, out_n - k); if (w <= 0) break; k += (unsigned)w; } out_n = 0; }
static void uart_put(u8 ch) { out[out_n++] = (char)ch; if (ch == '\n' || out_n == sizeof out) uart_flush(); }

static u8 lcr, mcr, scr, dll = 1, dlm, thre_pending;
ConsoleRing *console;
HOST("console_open") void host_console_open(u32 ring);   // the host writes the console's input there
void console_init(void) { console = aligned_alloc(64, sizeof *console); memset(console, 0, sizeof *console); console->cap = sizeof console->data; host_console_open((u32)(uptr)console); }
// what the host wrote reaches the receiver while a driver asks for its interrupt: the reads a driver makes while
// it probes and starts (as Linux's 8250 does, its interrupts off) take nothing from a pipe written before it booted
static u8 ier;
int console_pending(void) { return console && (ier & 1) && __atomic_load_n(&console->w, __ATOMIC_ACQUIRE) != console->r; }
static int rx_byte(void) {
  if (!console_pending()) return -1;
  u8 b = console->data[console->r % console->cap]; __atomic_store_n(&console->r, console->r + 1, __ATOMIC_RELEASE); return b;
}
// the interrupt line: data received, or the transmitter empty (until IIR says so or THR is written), when IER asks
static void uart_line(void) { ioapic_line(4, (mcr & 8) && (((ier & 1) && console_pending()) || ((ier & 2) && thre_pending))); }
static u32 seen_w;
void console_poll(void) { if (console && console->w != seen_w) { seen_w = console->w; uart_line(); } }
static u32 uart_in(unsigned r) {
  int dlab = lcr & 0x80;
  switch (r) {
  case 0: if (dlab) return dll; { int b = rx_byte(); if (mc_trace > 2 && b >= 0) rt_log("uart: RX %02x read (ier %02x)", b, ier); uart_line(); return b < 0 ? 0 : (u32)b; }
  case 1: return dlab ? dlm : ier;
  case 2: if ((ier & 1) && console_pending()) return 0xC4;                                   // data received
    if ((ier & 2) && thre_pending) { thre_pending = 0; uart_line(); return 0xC2; }   // the transmitter is empty: said once
    return 0xC1;                       // no interrupt pending; FIFOs on
  case 3: return lcr;
  case 4: return mcr;
  case 5: return 0x60 | (console_pending() ? 1 : 0);   // the transmitter is empty; data received
  case 6: return mcr & 0x10 ? (u32)((mcr & 0xC) << 4 | (mcr & 2) << 3 | (mcr & 1) << 5) : 0xB0;   // loopback, or carrier, DSR, CTS
  default: return scr;
  }
}
static void uart_out(unsigned r, u8 v) {
  int dlab = lcr & 0x80;
  switch (r) {
  case 0: if (dlab) dll = v; else { if (!(mcr & 0x10)) uart_put(v); thre_pending = 1; uart_line(); } break;   // sent at once: empty again
  case 1: if (dlab) dlm = v; else { if ((v & 2) && !(ier & 2)) thre_pending = 1; ier = v & 0xF; uart_line(); } break;
  case 2: break;   // FCR: the FIFOs are always on (and reset: nothing is kept in them)
  case 3: lcr = v; break;
  case 4: mcr = v & 0x1F; uart_line(); break;
  case 7: scr = v; break;
  }
}

// ---- the CMOS clock ----
static u8 cmos_index, cmos_b = 0x02;
static u8 bcd(unsigned v) { return (u8)((v / 10) << 4 | (v % 10)); }
static u32 cmos_read(void) {
  u64 t = 1104580800ull + mc.now / 1000000000ull;   // seconds since 1970
  unsigned sec = t % 60, min = (t / 60) % 60, hour = (t / 3600) % 24; u64 days = t / 86400;
  // the civil date of a day count (Howard Hinnant's algorithm)
  u64 z = days + 719468, era = z / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned day = (unsigned)(doy - (153 * mp + 2) / 5 + 1), month = (unsigned)(mp < 10 ? mp + 3 : mp - 9), year = (unsigned)(yoe + era * 400 + (month <= 2));
  switch (cmos_index & 0x7F) {
  case 0x00: return bcd(sec); case 0x02: return bcd(min); case 0x04: return bcd(hour);
  case 0x06: return bcd((unsigned)((days + 4) % 7) + 1); case 0x07: return bcd(day); case 0x08: return bcd(month); case 0x09: return bcd(year % 100);
  case 0x32: return bcd(year / 100);
  case 0x0A: return 0x26;                 // no update in progress
  case 0x0B: return cmos_b; case 0x0C: return 0; case 0x0D: return 0x80;   // the battery is fine
  }
  return 0;
}

static void unknown(const char *what, u16 port) {
  static u16 told[64]; static unsigned k;
  if (!mc_trace) return;
  for (unsigned i = 0; i < k; i++) if (told[i] == port) return;
  if (k < 64) told[k++] = port;
  rt_log("machine: %s port 0x%x, which nothing answers", what, port);
}
u32 rt_in(Cpu *c, u16 port, unsigned size) {
  (void)c;
  if (port >= 0x3F8 && port <= 0x3FF) return uart_in(port - 0x3F8);
  if (port == 0x71) return cmos_read();
  if (port == 0x64) return 0x1C;   // the keyboard controller: nothing to read, ready for a command
  unknown("a read of", port);
  return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
}
void rt_out(Cpu *c, u16 port, unsigned size, u32 v) {
  (void)c; (void)size;
  if (port >= 0x3F8 && port <= 0x3FF) { uart_out(port - 0x3F8, (u8)v); return; }
  if (port == 0xF4) { uart_flush(); rt_exit_process(v & 0xFF); }
  if ((port == 0x64 && (v & 0xFF) == 0xFE) || (port == 0xCF9 && (v & 4))) { uart_flush(); rt_log("machine: reset"); rt_exit_process(125); }
  if (port == 0x70) { cmos_index = (u8)v; return; }
  if (port == 0x71) { if ((cmos_index & 0x7F) == 0x0B) cmos_b = (u8)((v & 0x7F) | 0x02); return; }
  if (port == 0x80 || port == 0x64) return;
  unknown("a write to", port);
}
