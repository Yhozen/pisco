// SPDX-License-Identifier: GPL-2.0-or-later
// tests/gdi/fonts.c: rt/win32/font.c built natively, asked what tools/oracle-wine/gdiprobe.c asks Wine's GDI, and
// printing its answers in the probe's format, so tools/oracle-wine/gdi-compare.py can set the two side by side.
//   cc -O1 -DFONT_NO_EMBED -Irt/win32 tests/gdi/fonts.c rt/win32/font.c -lm -o out/gdi/fonts && out/gdi/fonts rt/win32/fonts
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int height, weight, italic, charset, quality; } Want;
static const Want fonts[] = {{60, 0, 0, 0, 3}, {24, 400, 0, 1, 4}, {48, 400, 1, 1, 4}, {48, 400, 0, 1, 4}};
static const char *texts[] = {"Polishing Up", "HINT: Escape from the cage, then weigh down both floor buttons."};
static uint8_t *slurp(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) exit(1); fclose(f); return d;
}
static uint32_t glyphs_of(const FontSize *f, const char *s, uint32_t *g) { uint32_t n = (uint32_t)strlen(s); for (uint32_t i = 0; i < n; i++) g[i] = font_glyph(f, font_cp1252((uint8_t)s[i])); return n; }
int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "rt/win32/fonts"; char path[512];
  snprintf(path, sizeof path, "%s/LiberationSans-Regular.ttf", dir); font_set_data(FONT_SANS, slurp(path));
  snprintf(path, sizeof path, "%s/LiberationSans-Italic.ttf", dir); font_set_data(FONT_SANS_ITALIC, slurp(path));
  printf("# What rt/win32/font.c answers for Narbacular Drop's fonts. Made by tests/gdi/fonts.c.\n");
  for (unsigned n = 0; n < sizeof fonts / sizeof fonts[0]; n++) {
    const Want *w = &fonts[n]; FontSize f; font_select(&f, w->height, w->italic, w->quality);
    printf("font %u height %d weight %d italic %d charset %d quality %d face \"%s\"\n", n, w->height, w->weight, w->italic, w->charset, w->quality, font_face_name(&f));
    printf("metrics %u height %d ascent %d descent %d internal %d external %d average %d widest %d weight %d overhang 0 aspect 96 96 chars 32 65532 31 32 italic %u underlined 0 struck 0 pitch 0x27 charset 0\n",
      n, f.height, f.ascent, f.descent, f.internal, f.external, f.average, f.widest, f.weight, w->italic ? 255 : 0);
    printf("language %u 0x%x kerning_pairs %u\n", n, font_kerning_pairs(&f) ? 8 : 0, font_kerning_pairs(&f));
    { uint16_t first[512], count[512]; uint32_t glyphs, r = font_unicode_ranges(&f, first, count, 512, &glyphs);
      printf("ranges %u size %u glyphs %u count %u flags 0:", n, 16 + 4 * r, glyphs, r); for (uint32_t i = 0; i < r && i < 8; i++) printf(" %04x+%u", first[i], count[i]); printf("\n"); }
    printf("glyph_indices %u:", n); for (uint32_t c = 0; c < 256; c++) { uint32_t g = font_glyph(&f, c); printf(" %u", g ? g : 65535); } printf("\n");
    printf("abc_i %u:", n); for (uint32_t g = 0; g < 256; g++) { int32_t a, b, c; font_abc(&f, g, &a, &b, &c); printf(" %d,%d,%d", a, b, c); } printf("\n");
    printf("width_i %u:", n); for (uint32_t g = 0; g < 256; g++) printf(" %d", font_advance(&f, g)); printf("\n");
    printf("abc_w %u:", n); for (uint32_t ch = 0; ch < 256; ch++) { int32_t a, b, c; font_abc(&f, font_glyph(&f, ch), &a, &b, &c); printf(" %d,%d,%d", a, b, c); } printf("\n");
    for (unsigned t = 0; t < 2; t++) for (unsigned k = 0; k < 2; k++) {
      uint32_t g[128]; int32_t dx[128]; uint32_t m = glyphs_of(&f, texts[t], g); int32_t sum = font_place(&f, g, m, k, dx);
      printf("placement %u text %u flags 0x%x result 0x%08x glyphs %u fit 0:", n, t, k ? 8 : 0, (uint32_t)(sum & 0xFFFF) | (uint32_t)f.height << 16, m);
      int32_t caret = 0; for (uint32_t i = 0; i < m; i++) { printf(" %u/%d/%d", g[i], dx[i], caret); caret += dx[i]; } printf("\n");
    }
    int big = n == 0 ? 64 : 160, high = n == 0 ? 64 : f.height;
    FontTarget tg = {calloc((size_t)big * high, 4), big, high, (uint32_t)big * 4, 32, {0xFF0000, 0xFF00, 0xFF}, 1};
    if (n == 0) {
      for (int ch = 0; ch < 256; ch++) {
        FontRect all = {0, 0, 64, 64}; font_fill(&tg, &all, 0xFF00FF);
        uint32_t g = font_glyph(&f, font_cp1252((uint8_t)ch));
        font_text_out(&f, &tg, 32, 64, 6 | 8, 0, 0, &g, 1, 0, 0xFFFFFF, 0xFF00FF, 1, 0, 0);
        printf("char %d\n", ch);
        for (int y = 0; y < 64; y++) { printf("  "); for (int x = 0; x < 64; x++) { uint32_t p; memcpy(&p, tg.bits + 4 * (y * 64 + x), 4); p &= 0xFFFFFF; putchar(p == 0xFF00FF ? '.' : p == 0xFFFFFF ? '#' : '?'); } printf("\n"); }
      }
    } else {
      char seen[256] = {0};
      for (unsigned t = 0; t < 2; t++) {
        uint32_t g[128]; uint32_t m = glyphs_of(&f, texts[t], g);
        for (uint32_t i = 0; i < m; i++) {
          if (g[i] < 256 && seen[g[i]]) continue; if (g[i] < 256) seen[g[i]] = 1;
          memset(tg.bits, 0x40, (size_t)big * high * 4);
          font_text_out(&f, &tg, 0, 0, 0, 0x12, 0, &g[i], 1, 0, 0xFFFFFF, 0, 1, 0, 0);
          int32_t a, b, c; font_abc(&f, g[i], &a, &b, &c);
          printf("glyph %u %u abc %d %d %d grey 1 %dx%d drawn 1\n", n, g[i], a, b, c, big, high);
          for (int y = 0; y < high; y++) { printf("  "); for (int x = 0; x < big; x++) printf("%02x", tg.bits[4 * (y * big + x)]); printf("\n"); }
        }
      }
    }
    free(tg.bits);
  }
  return 0;
}
