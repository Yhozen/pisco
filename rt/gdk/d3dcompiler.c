// SPDX-License-Identifier: GPL-2.0-or-later
// d3dcompiler_47.dll: shaders compiled ahead of time, looked up by their source.
#include "d3d12.h"

// ---- d3dcompiler_47.dll: no compiler runs here. D3DCompile looks the source up among shaders compiled ahead
// of time by tools/hlsl-prep.mjs (<title folder>/shaders/hlsl/<key>.cso, DXIL from dxc), keyed by a hash of
// the source, entry point and target. A miss is logged with the source, for hlsl-prep to pick up. ----
static u64 fnv64(u64 h, const void *p, size_t n) { const u8 *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static void b64(const u8 *p, size_t n, char *out) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63]; out[o++] = i + 1 < n ? t[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  out[o] = 0;
}
IMP(d3dcompiler_47, D3DCompile) { // (source, size, name, defines, include, entry, target, flags1, flags2, &code, &errors)
  u64 src = ARG(0), n = ARG(1); const char *entry = ARG(5) ? GSTR(ARG(5)) : "main", *target = ARG(6) ? GSTR(ARG(6)) : "";
  u64 h = fnv64(0xcbf29ce484222325ull, (void *)GP(src), (size_t)n); h = fnv64(h, "", 1); h = fnv64(h, entry, strlen(entry) + 1); h = fnv64(h, target, strlen(target));
  char name[64]; snprintf(name, sizeof name, "shaders/hlsl/%016llx.cso", (unsigned long long)h);
  u8 *buf = malloc(1 << 20); u32 got = host_read_file(name, (u32)strlen(name), 0, 1 << 20, (uptr)buf);
  u64 code = ARG(9), errors = ARG(10);
  if (errors) M64(errors) = 0;
  if (got) { if (code) M64(code) = rt_blob_new(buf, got); free(buf); RET(S_OK); return; }
  free(buf);
  char *enc = malloc((size_t)n * 4 / 3 + 8); b64((u8 *)GP(src), (size_t)n, enc);
  rt_log("d3dcompile: no precompiled shader %016llx for %s %s: run tools/hlsl-prep.mjs on this log", (unsigned long long)h, target, entry);
  rt_log("d3dcompile-source %016llx %s %s %s", (unsigned long long)h, target, entry, enc);
  free(enc);
  if (code) M64(code) = 0;
  if (errors) { const char *m = "pisco: this shader was not compiled ahead of time (tools/hlsl-prep.mjs)"; M64(errors) = rt_blob_new(m, strlen(m) + 1); }
  RET(0x80004005u);
}
