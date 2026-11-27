// SPDX-License-Identifier: GPL-2.0-or-later
// gl-ffp <out folder> [keys.txt]: writes the WGSL of OpenGL's fixed-function stages (rt/windows-x86/gl_ffp.c) for
// every texture-unit set-up (each unit off, or on with each environment and base format), and for every alpha
// test, fog mode and the clip plane with one of them; then for each key in keys.txt (hex, as a run's log prints
// them). One file a shader; a line each on stdout: "<file> <key> <what it is>". tests/gfx/gl-ffp.sh validates them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_ffp.h"

static char text[16384]; static int written, failed;
static void emit(const char *dir, const GlFragmentKey *k) {
  char hex[2 * sizeof *k + 1], what[256], path[512];
  for (size_t i = 0; i < sizeof *k; i++) sprintf(hex + 2 * i, "%02x", ((const unsigned char *)k)[i]);
  gl_fragment_describe(k, what, sizeof what);
  size_t n = gl_fragment_wgsl(k, text, sizeof text);
  if (!n) { printf("%s FAILED %s: %s\n", hex, what, text); failed++; return; }
  snprintf(path, sizeof path, "%s/fs-%s.wgsl", dir, hex);
  FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
  fwrite(text, 1, n, f); fclose(f); written++;
  printf("fs-%s.wgsl %s %s\n", hex, hex, what);
}
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: gl-ffp <out folder> [keys.txt]\n"); return 2; }
  size_t n = gl_vertex_wgsl(text, sizeof text);
  if (!n) { printf("vs FAILED: %s\n", text); return 1; }
  char path[512]; snprintf(path, sizeof path, "%s/vs.wgsl", argv[1]);
  FILE *f = fopen(path, "w"); fwrite(text, 1, n, f); fclose(f); written++;
  printf("vs.wgsl - the vertex stage\n");
  for (int a = 0; a < 16; a++) for (int b = 0; b < 16; b++) {   // a unit: 0 off, else 1 + environment * 3 + base format
    GlFragmentKey k = {0};
    if (a) { k.unit_on[0] = 1; k.env[0] = (unsigned char)((a - 1) / 3); k.base[0] = (unsigned char)((a - 1) % 3); }
    if (b) { k.unit_on[1] = 1; k.env[1] = (unsigned char)((b - 1) / 3); k.base[1] = (unsigned char)((b - 1) % 3); }
    emit(argv[1], &k);
  }
  for (int t = 0; t <= 8; t++) for (int fog = 0; fog <= 3; fog++) for (int clip = 0; clip <= 1; clip++) {
    GlFragmentKey k = {{1, 1}, {GL_ENV_MODULATE, GL_ENV_MODULATE}, {GL_BASE_RGBA, GL_BASE_RGB}, (unsigned char)t, (unsigned char)fog, (unsigned char)clip, {0}};
    emit(argv[1], &k);
  }
  if (argc > 2) {
    FILE *in = fopen(argv[2], "r"); char line[128];
    while (in && fgets(line, sizeof line, in)) {
      GlFragmentKey k; unsigned char *p = (unsigned char *)&k; unsigned v;
      if (strlen(line) < 2 * sizeof k) continue;
      for (size_t i = 0; i < sizeof k; i++) { sscanf(line + 2 * i, "%2x", &v); p[i] = (unsigned char)v; }
      emit(argv[1], &k);
    }
    if (in) fclose(in);
  }
  fprintf(stderr, "%d shaders written, %d failures\n", written, failed);
  return failed != 0;
}
