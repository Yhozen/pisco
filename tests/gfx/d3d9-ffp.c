// SPDX-License-Identifier: GPL-2.0-or-later
// d3d9-ffp <out dir> [keys.txt]: writes the fixed-function shaders (rt/win32/d3d9_ffp.c) of a representative set
// of state keys as <out dir>/vs-<n>.wgsl and fs-<n>.wgsl, with one line each on stdout ("vs-3.wgsl <key> <what>").
// With a keys file (lines "vs <hex>" or "fs <hex>", as a run logs them under SW_D3D9_STATS=1) those keys are
// written too. Keys the generator must refuse are checked here. tests/gfx/d3d9-ffp.sh validates the files with naga.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_ffp.h"

static const char *dir; static int count[2], failed;
static char text[65536];
static void emit(const char *stage, int n, const char *hex, const char *what, size_t len) {
  char path[512]; snprintf(path, sizeof path, "%s/%s-%d.wgsl", dir, stage, n);
  FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
  fwrite(text, 1, len, f); fclose(f);
  printf("%s-%d.wgsl %s %s\n", stage, n, hex, what);
}
static void vs(FfpVertexKey k) {
  char hex[2 * sizeof k + 1], what[256]; ffp_key_hex(&k, sizeof k, hex); ffp_vertex_describe(&k, what, sizeof what);
  size_t len = ffp_vertex_wgsl(&k, text, sizeof text);
  if (!len) { printf("FAILED vs %s (%s): %s\n", hex, what, text); failed++; return; }
  emit("vs", count[0]++, hex, what, len);
}
static void fs(FfpFragmentKey k) {
  char hex[2 * sizeof k + 1], what[256]; ffp_key_hex(&k, sizeof k, hex); ffp_fragment_describe(&k, what, sizeof what);
  size_t len = ffp_fragment_wgsl(&k, text, sizeof text);
  if (!len) { printf("FAILED fs %s (%s): %s\n", hex, what, text); failed++; return; }
  emit("fs", count[1]++, hex, what, len);
}
static void vs_refused(FfpVertexKey k, const char *why) { if (ffp_vertex_wgsl(&k, text, sizeof text)) { printf("FAILED: a vertex key that should be refused was not (%s)\n", why); failed++; } }
static void fs_refused(FfpFragmentKey k, const char *why) { if (ffp_fragment_wgsl(&k, text, sizeof text)) { printf("FAILED: a fragment key that should be refused was not (%s)\n", why); failed++; } }

// A vertex key from an FVF code, with the device's defaults around it (one world matrix, lighting on, a local
// viewer, the diffuse colour from the vertex when it has one)
static FfpVertexKey fvf(unsigned code) {
  FfpVertexKey k = {0};
  unsigned position = code & 0x400E;
  k.pretransformed = position == 4;
  if (position >= 6 && position <= 0xE) k.weights = (uint8_t)((position - 4) / 2);
  k.matrices = 1;
  k.normal = (code & 0x10) != 0; k.psize = (code & 0x20) != 0; k.diffuse = (code & 0x40) != 0; k.specular = (code & 0x80) != 0;
  if ((code >> 8) & 0xF) { static const uint8_t n[4] = {2, 3, 4, 1}; k.tex = n[(code >> 16) & 3]; }
  k.lighting = 1; k.local_viewer = 1;
  k.diffuse_source = k.diffuse ? FFP_SOURCE_DIFFUSE : FFP_SOURCE_MATERIAL;
  k.specular_source = k.specular ? FFP_SOURCE_SPECULAR : FFP_SOURCE_MATERIAL;
  return k;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: d3d9-ffp <out dir> [keys.txt]\n"); return 2; }
  dir = argv[1];
  FfpVertexKey k;
  // ---- vertex formats by FVF: what the game draws with, each lit by 0 to 8 lights where it has a normal ----
  static const unsigned codes[] = {0x112 /* XYZ|NORMAL|TEX1 */, 0x144 /* XYZRHW|DIFFUSE|TEX1 */, 0x104 /* XYZRHW|TEX1 */, 0x44 /* XYZRHW|DIFFUSE */, 0x42 /* XYZ|DIFFUSE */,
    0x62 /* XYZ|PSIZE|DIFFUSE */, 0x116 /* XYZB1|NORMAL|TEX1 */, 0x118 /* XYZB2 */, 0x11A /* XYZB3 */, 0x10102 /* XYZ|TEX1, three coordinates */, 0x30102 /* one */, 0x20102 /* four */,
    0x1C4 /* XYZRHW|DIFFUSE|SPECULAR|TEX1 */, 0x1D2 /* XYZ|NORMAL|DIFFUSE|SPECULAR|TEX1 */, 0x002 /* XYZ */, 0x212 /* XYZ|NORMAL|TEX2: the second set is not read */};
  for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
    k = fvf(codes[i]);
    if (k.weights) k.matrices = (uint8_t)(k.weights + 1);
    for (unsigned lights = 0; lights <= 8; lights += lights < 4 ? 1 : 4) {
      k.lights = (uint8_t)lights; vs(k);
      if (!k.normal || k.pretransformed) break;
    }
    k.lights = 2; k.fog = FFP_FOG_LINEAR; k.normalize = 1; vs(k);
    k.lighting = 0; vs(k);
  }
  // ---- vertex blending: every count of weights against every count of matrices ----
  for (unsigned w = 0; w <= 3; w++) for (unsigned m = 1; m <= 4; m++) {
    k = fvf(0x112); k.weights = (uint8_t)w; k.matrices = (uint8_t)m; k.lights = 3; k.normalize = (m & 1) != 0; vs(k);
    k.normal = 0; k.lighting = 0; vs(k);
  }
  // ---- lighting: sources, specular, the viewer ----
  for (unsigned s = 0; s < 81; s += 7) {
    k = fvf(0x1D2); k.lights = 4; k.specular_on = 1; k.local_viewer = s & 1;
    k.diffuse_source = (uint8_t)(s % 3); k.ambient_source = (uint8_t)(s / 3 % 3); k.specular_source = (uint8_t)(s / 9 % 3); k.emissive_source = (uint8_t)(s / 27 % 3); vs(k);
  }
  k = fvf(0x112); k.lights = 8; k.specular_on = 1; k.normalize = 1; vs(k);          // every light, with specular
  k = fvf(0x42); k.lights = 1; vs(k);                                                // lit without a normal: ambient only
  // ---- fog ----
  for (unsigned mode = FFP_FOG_EXP; mode <= FFP_FOG_LINEAR; mode++) for (unsigned range = 0; range < 2; range++) {
    k = fvf(0x112); k.lights = 1; k.fog = (uint8_t)mode; k.range_fog = (uint8_t)range; vs(k);
    k = fvf(0x144); k.fog = (uint8_t)mode; vs(k);
    k = fvf(0x1C4); k.fog = (uint8_t)mode; vs(k);
  }
  // ---- texture coordinates: generated ones, a texture matrix ----
  for (unsigned gen = 0; gen <= FFP_TEXGEN_REFLECTION; gen++) for (unsigned tr = 0; tr <= 4; tr++) for (unsigned tex = 0; tex <= 4; tex += 2) {
    k = fvf(0x112); k.tex = (uint8_t)tex; k.tex_gen = (uint8_t)gen; k.tex_transform = (uint8_t)tr; vs(k);
  }
  for (unsigned tex = 1; tex <= 4; tex++) { k = fvf(0x144); k.tex = (uint8_t)tex; k.tex_transform = 2; vs(k); }
  // ---- points: with and without a size in the vertex, scaled, as sprites, pretransformed ----
  for (unsigned v = 0; v < 16; v++) {
    k = fvf((v & 1) ? 0x62 : 0x42); k.lighting = 0; k.point = 1; k.point_scale = (v & 2) != 0; k.point_sprite = (v & 4) != 0; k.fog = (v & 8) ? FFP_FOG_LINEAR : 0; vs(k);
  }
  k = fvf(0x144); k.point = 1; k.point_sprite = 1; vs(k);
  k = fvf(0x64); k.point = 1; k.point_scale = 1; vs(k);
  k = fvf(0x112); k.point = 1; k.lights = 2; vs(k);
  // ---- what must be refused ----
  k = fvf(0x112); k.matrices = 0; vs_refused(k, "no world matrix");
  k = fvf(0x112); k.matrices = 5; vs_refused(k, "five world matrices");
  k = fvf(0x112); k.weights = 4; vs_refused(k, "four weights");
  k = fvf(0x112); k.lights = 9; vs_refused(k, "nine lights");
  k = fvf(0x112); k.tex = 5; vs_refused(k, "five texture coordinates");
  k = fvf(0x112); k.fog = 4; vs_refused(k, "a fog mode that is none");

  // ---- the fragment stage: one texture stage ----
  FfpFragmentKey f;
  // the game's: MODULATE, SELECTARG1 or DISABLE on colour and alpha, of the texture (2) or the diffuse colour (0)
  static const uint8_t ops[] = {1, 2, 4}, textures[] = {FFP_TEXTURE_NONE, FFP_TEXTURE_2D, FFP_TEXTURE_3D};
  for (unsigned c = 0; c < 3; c++) for (unsigned a = 0; a < 3; a++) for (unsigned a1 = 0; a1 <= 2; a1 += 2) for (unsigned tx = 0; tx < 3; tx++) {
    f = (FfpFragmentKey){ops[c], (uint8_t)a1, 0, ops[a], (uint8_t)a1, 0, textures[tx]};
    fs(f);
    f.alpha_test = 5; f.fog = 1; f.texture_opaque = tx == 1; fs(f);
  }
  // every operation, on both channels
  for (unsigned op = 1; op <= 16; op++) if (op != 15) for (unsigned tx = 0; tx < 2; tx++) {
    f = (FfpFragmentKey){(uint8_t)op, 2, 0, (uint8_t)op, 2, 0, tx ? FFP_TEXTURE_2D : FFP_TEXTURE_NONE}; fs(f);
    f.color_arg1 = 0; f.color_arg2 = 2; f.alpha_arg1 = 3; f.alpha_arg2 = 2; fs(f);
  }
  // every argument, with its modifiers
  for (unsigned a = 0; a <= 4; a++) for (unsigned mod = 0; mod < 0x40; mod += 0x10) {
    f = (FfpFragmentKey){4, (uint8_t)(a | mod), 1, 4, (uint8_t)(a | (mod & 0x10)), 0, FFP_TEXTURE_2D}; fs(f);
    f = (FfpFragmentKey){7, 2, (uint8_t)(a | mod), 10, 2, (uint8_t)(a | (mod & 0x10)), FFP_TEXTURE_3D, .specular_add = 1}; fs(f);
  }
  // the alpha test's eight comparisons; a texture without alpha; projection; specular; fog
  for (unsigned cmp = 1; cmp <= 8; cmp++) { f = (FfpFragmentKey){4, 2, 0, 4, 2, 0, FFP_TEXTURE_2D, .alpha_test = (uint8_t)cmp, .fog = cmp & 1, .texture_opaque = (cmp & 2) != 0}; fs(f); }
  for (unsigned p = 2; p <= 4; p++) { f = (FfpFragmentKey){2, 2, 0, 2, 2, 0, p == 4 ? FFP_TEXTURE_3D : FFP_TEXTURE_2D, .projected = (uint8_t)p}; fs(f); }
  f = (FfpFragmentKey){4, 2, 0, 1, 2, 0, FFP_TEXTURE_2D, .specular_add = 1, .fog = 1, .alpha_test = 7}; fs(f);
  f = (FfpFragmentKey){1, 2, 0, 4, 2, 0, FFP_TEXTURE_2D, .fog = 1, .alpha_test = 5}; fs(f);   // the stage disabled with a texture bound
  // what must be refused
  fs_refused((FfpFragmentKey){15, 2, 0, 2, 2, 0, FFP_TEXTURE_2D}, "premultiplied texture alpha blend");
  fs_refused((FfpFragmentKey){22, 2, 0, 2, 2, 0, FFP_TEXTURE_2D}, "bump mapping");
  fs_refused((FfpFragmentKey){4, 2, 0, 26, 2, 0, FFP_TEXTURE_2D}, "an operation of three arguments");
  fs_refused((FfpFragmentKey){4, 5, 0, 2, 2, 0, FFP_TEXTURE_2D}, "the temporary register");
  fs_refused((FfpFragmentKey){4, 2, 0, 2, 2, 0, 1}, "a texture kind that is none");
  fs_refused((FfpFragmentKey){4, 2, 0, 2, 2, 0, FFP_TEXTURE_2D, .alpha_test = 9}, "a comparison that is none");

  // ---- the keys of a run ----
  if (argc > 2) {
    FILE *in = fopen(argv[2], "r"); if (!in) { perror(argv[2]); return 2; }
    char stage[8], hex[256];
    while (fscanf(in, "%7s %255s", stage, hex) == 2) {
      FfpVertexKey vk; FfpFragmentKey fk;
      if (!strcmp(stage, "vs") && ffp_key_parse(hex, &vk, sizeof vk)) vs(vk);
      else if (!strcmp(stage, "fs") && ffp_key_parse(hex, &fk, sizeof fk)) fs(fk);
      else { printf("FAILED: cannot read the key \"%s %s\"\n", stage, hex); failed++; }
    }
    fclose(in);
  }
  fprintf(stderr, "%d vertex shaders, %d fragment shaders, %d failures\n", count[0], count[1], failed);
  return failed ? 1 : 0;
}
