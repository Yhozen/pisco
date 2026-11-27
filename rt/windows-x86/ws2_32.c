// SPDX-License-Identifier: GPL-2.0-or-later
// Windows Sockets on a machine with no network: start-up succeeds and the local host has a name and a loopback
// address, sockets can be made, bound and configured, what is sent goes nowhere and nothing ever arrives (a
// non-blocking receive says it would block). Old titles import ws2_32 by ordinal (wsock32 forwards to it).
#include "../rt.h"

#define WSAEWOULDBLOCK 10035
#define WSAENOTSOCK 10038
enum { MAX_SOCKETS = 64 };
static struct { int used, nonblocking; u16 port; } sockets[MAX_SOCKETS];
static u32 sock_of(u64 s) { return (u32)s >= 0x100 && (u32)s < 0x100 + MAX_SOCKETS && sockets[(u32)s - 0x100].used ? (u32)s - 0x100 : ~0u; }
static void wsa_error(Cpu *c, u32 e) { SET_LAST_ERROR(e); }

IMP(ws2_32, WSAStartup) { // (version, WSADATA*): 2.2
  u64 d = ARG(1);
  if (d) { memset((void *)GP(d), 0, 400); M16(d) = (u16)ARG(0); M16(d + 2) = 0x0202; strcpy(GSTR(d + 4), "PISCO sockets (no network)"); strcpy(GSTR(d + 261), "Running"); M16(d + 390) = MAX_SOCKETS; }
  RET(0);
}
IMP(ws2_32, WSACleanup) { RET(0); }
IMP(ws2_32, WSAGetLastError) { RET(M32(THREAD(c)->teb + TEB_LAST_ERROR)); }
IMP(ws2_32, socket) { // (af, type, protocol)
  for (u32 i = 0; i < MAX_SOCKETS; i++) if (!sockets[i].used) { sockets[i].used = 1; sockets[i].nonblocking = 0; sockets[i].port = 0; RET(0x100 + i); return; }
  wsa_error(c, 10024); RET(0xFFFFFFFFu);   // WSAEMFILE; INVALID_SOCKET
}
IMP(ws2_32, closesocket) { u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; } sockets[i].used = 0; RET(0); }
IMP(ws2_32, bind) { u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; } if (ARG(1)) sockets[i].port = M16(ARG(1) + 2); RET(0); }
IMP(ws2_32, setsockopt) { RET(sock_of(ARG(0)) == ~0u ? (wsa_error(c, WSAENOTSOCK), 0xFFFFFFFFu) : 0); }
IMP(ws2_32, ioctlsocket) { // (socket, command, &argument): FIONBIO turns blocking off or on
  u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; }
  if ((u32)ARG(1) == 0x8004667Eu && ARG(2)) sockets[i].nonblocking = M32(ARG(2)) != 0;
  else if ((u32)ARG(1) == 0x4004667Fu && ARG(2)) M32(ARG(2)) = 0;   // FIONREAD: nothing waiting
  RET(0);
}
IMP(ws2_32, sendto) { u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; } RET((u32)ARG(2)); } // sent, to nowhere
IMP(ws2_32, recvfrom) { // nothing arrives: a non-blocking socket says so; a blocking one would wait for ever
  u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; }
  if (!sockets[i].nonblocking) rt_fatal("ws2_32: recvfrom on a blocking socket would wait for ever: there is no network");
  wsa_error(c, WSAEWOULDBLOCK); RET(0xFFFFFFFFu);
}
IMP(ws2_32, gethostname) { const char *n = "pisco"; if ((u32)ARG(1) < strlen(n) + 1) { wsa_error(c, 10014); RET(0xFFFFFFFFu); return; } strcpy(GSTR(ARG(0)), n); RET(0); }
// (name): every name is this machine, 127.0.0.1. HOSTENT: name, aliases, address type (2), length (4), addresses
IMP(ws2_32, gethostbyname) {
  static u64 he; if (!he) {
    he = (uptr)calloc(1, 64); u64 addr = he + 32, list = he + 40, aliases = he + 48, name = he + 52;
    M8(addr) = 127; M8(addr + 3) = 1; M32(list) = (u32)addr; M32(list + 4) = 0; M32(aliases) = 0; strcpy(GSTR(name), "pisco");
    M32(he) = (u32)name; M32(he + 4) = (u32)aliases; M16(he + 8) = 2; M16(he + 10) = 4; M32(he + 12) = (u32)list;
  }
  RET(he);
}
IMP(ws2_32, htons) { u16 v = (u16)ARG(0); RET((u16)(v << 8 | v >> 8)); }
IMP(ws2_32, ntohs) { u16 v = (u16)ARG(0); RET((u16)(v << 8 | v >> 8)); }
IMP(ws2_32, htonl) { RET(__builtin_bswap32((u32)ARG(0))); }
IMP(ws2_32, ntohl) { RET(__builtin_bswap32((u32)ARG(0))); }
IMP(ws2_32, inet_ntoa) { // (in_addr by value): a buffer of the thread's, as Windows has
  static __thread char buf[16]; u32 a = (u32)ARG(0);
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
  static __thread u64 g; if (!g) g = (uptr)calloc(1, 16);
  strcpy(GSTR(g), buf); RET(g);
}
IMP(ws2_32, connect) { u32 i = sock_of(ARG(0)); wsa_error(c, i == ~0u ? WSAENOTSOCK : 10061); RET(0xFFFFFFFFu); }   // WSAECONNREFUSED: no one is there
IMP(ws2_32, getsockname) { // (socket, sockaddr *, &length): 127.0.0.1 and the port it was bound to
  u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; }
  u64 a = ARG(1); if (!a || !ARG(2) || M32(ARG(2)) < 16) { wsa_error(c, 10014); RET(0xFFFFFFFFu); return; }
  memset((void *)GP(a), 0, 16); M16(a) = 2; M16(a + 2) = sockets[i].port; M8(a + 4) = 127; M8(a + 7) = 1; M32(ARG(2)) = 16; RET(0);
}
IMP(ws2_32, inet_addr) { // "a.b.c.d" -> the address in network order; INADDR_NONE for anything else
  unsigned a, b, d, e; char x;
  if (sscanf(GSTR(ARG(0)), "%u.%u.%u.%u%c", &a, &b, &d, &e, &x) != 4 || a > 255 || b > 255 || d > 255 || e > 255) { RET(0xFFFFFFFFu); return; }
  RET(a | b << 8 | d << 16 | e << 24);
}
IMP(ws2_32, send) { u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; } RET((u32)ARG(2)); }
IMP(ws2_32, recv) {
  u32 i = sock_of(ARG(0)); if (i == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; }
  if (!sockets[i].nonblocking) rt_fatal("ws2_32: recv on a blocking socket would wait for ever: there is no network");
  wsa_error(c, WSAEWOULDBLOCK); RET(0xFFFFFFFFu);
}
// select(n, read fd_set *, write, except, timeval *): nothing is ever readable; every socket can be written to. With
// nothing to report, it waits for its time-out (a virtual clock moves on by it). fd_set: count, then the sockets.
IMP(ws2_32, select) {
  u64 rd = ARG(1), wr = ARG(2), ex = ARG(3), tv = ARG(4); u32 ready = 0;
  if (rd) M32(rd) = 0;
  if (ex) M32(ex) = 0;
  if (wr) ready += M32(wr);
  if (!ready && tv) { u64 us = (u64)M32(tv) * 1000000 + M32(tv + 4); if (rt_clock_virtual()) rt_clock_advance(us); }
  RET(ready);
}
IMP(ws2_32, __WSAFDIsSet) { u64 set = ARG(1); u32 n = M32(set); for (u32 k = 0; k < n && k < 64; k++) if (M32(set + 4 + 4 * k) == (u32)ARG(0)) { RET(1); return; } RET(0); }
IMP(ws2_32, getsockopt) { // (socket, level, name, value *, &length): every option is 0
  if (sock_of(ARG(0)) == ~0u) { wsa_error(c, WSAENOTSOCK); RET(0xFFFFFFFFu); return; }
  if (ARG(3) && ARG(4) && M32(ARG(4)) >= 4) { M32(ARG(3)) = 0; M32(ARG(4)) = 4; }
  RET(0);
}
// ws2_32's ordinals (as its import library numbers them); wsock32's are the same functions but for three (rt/process.c)
ALIAS(ws2_32, "#2", bind, 12);
ALIAS(ws2_32, "#3", closesocket, 4);
ALIAS(ws2_32, "#8", htonl, 4);
ALIAS(ws2_32, "#9", htons, 4);
ALIAS(ws2_32, "#10", ioctlsocket, 12);
ALIAS(ws2_32, "#12", inet_ntoa, 4);
ALIAS(ws2_32, "#14", ntohl, 4);
ALIAS(ws2_32, "#15", ntohs, 4);
ALIAS(ws2_32, "#17", recvfrom, 24);
ALIAS(ws2_32, "#20", sendto, 24);
ALIAS(ws2_32, "#21", setsockopt, 20);
ALIAS(ws2_32, "#23", socket, 12);
ALIAS(ws2_32, "#4", connect, 12);
ALIAS(ws2_32, "#6", getsockname, 12);
ALIAS(ws2_32, "#7", getsockopt, 20);
ALIAS(ws2_32, "#11", inet_addr, 4);
ALIAS(ws2_32, "#16", recv, 16);
ALIAS(ws2_32, "#18", select, 20);
ALIAS(ws2_32, "#19", send, 16);
ALIAS(ws2_32, "#151", __WSAFDIsSet, 8);
ALIAS(ws2_32, "#52", gethostbyname, 4);
ALIAS(ws2_32, "#57", gethostname, 8);
ALIAS(ws2_32, "#111", WSAGetLastError, 0);
ALIAS(ws2_32, "#115", WSAStartup, 8);
ALIAS(ws2_32, "#116", WSACleanup, 0);
