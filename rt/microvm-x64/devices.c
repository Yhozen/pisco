// SPDX-License-Identifier: GPL-2.0-or-later
// The machine's port devices: a 16550 serial port at 0x3f8 (its output is the title's standard output; no input
// yet), an exit port at 0xf4 as QEMU's isa-debug-exit (a byte written there ends the machine with that status),
// and 8259s that answer as masked. A port nothing answers reads as ones and is said once with SW_TRACE.
#include <unistd.h>
#include "machine.h"

static char out[4096]; static unsigned out_n;
void uart_flush(void) { unsigned k = 0; while (k < out_n) { ssize_t w = write(1, out + k, out_n - k); if (w <= 0) break; k += (unsigned)w; } out_n = 0; }
static void uart_put(u8 ch) { out[out_n++] = (char)ch; if (ch == '\n' || out_n == sizeof out) uart_flush(); }

static u8 lcr, ier, mcr, scr, dll = 1, dlm;
static u32 uart_in(unsigned r) {
  int dlab = lcr & 0x80;
  switch (r) {
  case 0: return dlab ? dll : 0;
  case 1: return dlab ? dlm : ier;
  case 2: return 0xC1;                 // no interrupt pending; FIFOs on
  case 3: return lcr;
  case 4: return mcr;
  case 5: return 0x60;                 // the transmitter is empty, nothing received
  case 6: return mcr & 0x10 ? (u32)((mcr & 0xC) << 4 | (mcr & 2) << 3 | (mcr & 1) << 5) : 0xB0;   // loopback, or carrier, DSR, CTS
  default: return scr;
  }
}
static void uart_out(unsigned r, u8 v) {
  int dlab = lcr & 0x80;
  switch (r) {
  case 0: if (dlab) dll = v; else if (!(mcr & 0x10)) uart_put(v); break;
  case 1: if (dlab) dlm = v; else ier = v & 0xF; break;
  case 3: lcr = v; break;
  case 4: mcr = v & 0x1F; break;
  case 7: scr = v; break;
  }
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
  if (port == 0x21 || port == 0xA1) return 0xFF;
  unknown("a read of", port);
  return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
}
void rt_out(Cpu *c, u16 port, unsigned size, u32 v) {
  (void)c; (void)size;
  if (port >= 0x3F8 && port <= 0x3FF) { uart_out(port - 0x3F8, (u8)v); return; }
  if (port == 0xF4) { uart_flush(); rt_exit_process(v & 0xFF); }
  if (port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1 || port == 0x80) return;
  unknown("a write to", port);
}
