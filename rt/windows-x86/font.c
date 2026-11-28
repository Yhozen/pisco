// SPDX-License-Identifier: GPL-2.0-or-later
// The fonts behind gdi32: Liberation Sans, regular and italic (SIL Open Font License, rt/windows-x86/fonts/), built into
// the module, rasterised with stb_truetype (rt/windows-x86/third_party/, public domain / MIT).
//
// Why these: under Wine on the oracle's machine every font Narbacular Drop and its D3DX ask for (no face name,
// heights 60, 48 and 24) becomes Liberation Sans, the host's metric-compatible stand-in for Arial; the files here
// are the upstream 2.1.5 release, the same version the oracle's host has (they differ from its copies only in
// their checksum and time stamp). Every face name gets this face; italic gets the italic file. Measured against
// Wine with tools/oracle-wine/gdiprobe.c (docs/narbacular-drop.md).
//
// How Wine measures (and so this file): the em size for a positive height is the largest whose
// usWinAscent + usWinDescent fits in it; ascent and descent are those two scaled and rounded; advances are
// scaled and rounded; a glyph's black box runs from the floor of its left edge to the ceiling of its right one;
// kerning comes from the kern table, rounded as Windows does. Glyph pixels: Wine renders 256 levels of coverage
// and keeps 17 (GGO_GRAY4_BITMAP) for antialiased text, one bit for NONANTIALIASED_QUALITY. What differs: Wine's
// FreeType runs the font's TrueType hinting; stb_truetype does not hint. font_render stands in with vertical
// hinting only (the font's blue zones snapped to whole pixels), so a stem's edge can still land half a pixel
// away from Wine's.
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
// stb_truetype's memory comes from a pool of this file's, emptied before each glyph: the app's heap is the
// runtime's malloc, and an app that reads freed or uninitialised heap memory (Narbacular Drop does, in its first
// level) must not find the rasteriser's leftovers there
static unsigned char pool[1 << 20]; static size_t pool_used;
static void *pool_alloc(size_t n) { n = (n + 15) & ~(size_t)15; if (pool_used + n > sizeof pool) return 0; void *p = pool + pool_used; pool_used += n; return p; }
#define STBTT_malloc(x, u) ((void)(u), pool_alloc(x))
#define STBTT_free(x, u) ((void)(u), (void)(x))
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#ifndef FONT_NO_EMBED
static const unsigned char sans_regular[] = {
#embed "fonts/LiberationSans-Regular.ttf"
};
static const unsigned char sans_italic[] = {
#embed "fonts/LiberationSans-Italic.ttf"
};
static const unsigned char *data[FONT_FACES] = {sans_regular, sans_italic};
#else
static const unsigned char *data[FONT_FACES];
#endif
static const char *names[FONT_FACES] = {"Liberation Sans", "Liberation Sans"};

typedef struct {
  int ready; stbtt_fontinfo info;
  int upem, win_ascent, win_descent, hhea_ascent, hhea_descent, line_gap, average, weight, bbox_width;
  uint32_t ranges, range_glyphs; uint16_t range_first[512], range_count[512];
  int zones, zone_ref[8], zone_shoot[8];   // the heights glyphs line up on (font units) and how far round shapes pass them
} Face;
static Face faces[FONT_FACES];

void font_set_data(int face, const uint8_t *bytes) { if (face >= 0 && face < FONT_FACES) { data[face] = bytes; faces[face].ready = 0; } }
static int16_t s16(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static Face *face_of(int n) {
  Face *fc = &faces[n];
  if (fc->ready) return fc;
  const uint8_t *d = data[n];
  stbtt_InitFont(&fc->info, d, stbtt_GetFontOffsetForIndex(d, 0));
  uint32_t head = stbtt__find_table((uint8_t *)d, 0, "head"), hhea = stbtt__find_table((uint8_t *)d, 0, "hhea"), os2 = stbtt__find_table((uint8_t *)d, 0, "OS/2");
  fc->upem = u16(d + head + 18); fc->bbox_width = s16(d + head + 40) - s16(d + head + 36);
  fc->hhea_ascent = s16(d + hhea + 4); fc->hhea_descent = s16(d + hhea + 6); fc->line_gap = s16(d + hhea + 8);
  fc->average = s16(d + os2 + 2); fc->weight = u16(d + os2 + 4);
  fc->win_ascent = u16(d + os2 + 74); fc->win_descent = abs(s16(d + os2 + 76));
  // the characters the face has, as ranges (GetFontUnicodeRanges)
  uint32_t first = 0, have = 0;
  for (uint32_t ch = 0; ch <= 0x10000; ch++) {
    int g = ch < 0x10000 ? stbtt_FindGlyphIndex(&fc->info, (int)ch) : 0;
    if (g && !have) { first = ch; have = 1; }
    else if (!g && have) {
      if (fc->ranges < 512) { fc->range_first[fc->ranges] = (uint16_t)first; fc->range_count[fc->ranges] = (uint16_t)(ch - first); }
      fc->ranges++; fc->range_glyphs += ch - first; have = 0;
    }
  }
  // Blue zones, as an autohinter finds them: the flat height of a reference glyph and the overshoot of a round one
  static const char zone_chars[][2] = {{'H', 'O'}, {'x', 'o'}, {'h', 'h'}, {0, 'o'}, {'p', 'p'}};   // cap height, x-height, ascender, baseline, descender
  for (unsigned i = 0; i < sizeof zone_chars / sizeof zone_chars[0]; i++) {
    int x0, y0, x1, y1, ref = 0, shoot;
    int top = i < 3;
    if (zone_chars[i][0] && stbtt_GetGlyphBox(&fc->info, stbtt_FindGlyphIndex(&fc->info, zone_chars[i][0]), &x0, &y0, &x1, &y1)) ref = top ? y1 : y0;
    if (!stbtt_GetGlyphBox(&fc->info, stbtt_FindGlyphIndex(&fc->info, zone_chars[i][1]), &x0, &y0, &x1, &y1)) continue;
    shoot = top ? y1 : y0;
    if (!zone_chars[i][0]) ref = 0;
    fc->zone_ref[fc->zones] = ref; fc->zone_shoot[fc->zones] = shoot; fc->zones++;
  }
  fc->ready = 1;
  return fc;
}

// FreeType's fixed-point arithmetic, which Wine's numbers come out of
static int64_t div_fix(int64_t a, int64_t b) { int s = (a < 0) != (b < 0); a = llabs(a); b = llabs(b); int64_t q = (a * 65536 + b / 2) / b; return s ? -q : q; }
static int64_t mul_fix(int64_t a, int64_t b) { int s = (a < 0) != (b < 0); a = llabs(a); b = llabs(b); int64_t q = (a * b + 0x8000) >> 16; return s ? -q : q; }
static int32_t mul_div(int32_t a, int32_t b, int32_t c) { int64_t p = (int64_t)a * b; return (int32_t)(p >= 0 ? (p + c / 2) / c : -((-p + c / 2) / c)); }
static int32_t scale_x(const FontSize *f, int32_t units) { return (int32_t)((mul_fix(units, f->x_scale) + 32) >> 6); }
static int32_t scale_y(const FontSize *f, int32_t units) { return (int32_t)((mul_fix(units, f->y_scale) + 32) >> 6); }

void font_select(FontSize *f, int32_t lf_height, int italic, int quality) {
  memset(f, 0, sizeof *f);
  f->face = italic ? FONT_SANS_ITALIC : FONT_SANS;
  Face *fc = face_of(f->face);
  int32_t height = lf_height ? lf_height : 16, units = fc->win_ascent + fc->win_descent, ppem;
  if (height > 0) { ppem = mul_div(fc->upem, height, units); if (ppem > 1 && mul_div(units, ppem, fc->upem) > height) ppem--; }
  else ppem = -height;
  if (ppem < 1) ppem = 1;
  f->ppem = ppem;
  f->x_scale = f->y_scale = div_fix((int64_t)ppem << 6, fc->upem);
  f->scale = (float)ppem / (float)fc->upem;
  f->ascent = scale_y(f, fc->win_ascent); f->descent = scale_y(f, fc->win_descent); f->height = f->ascent + f->descent;
  f->internal = scale_y(f, units - fc->upem);
  int32_t el = scale_y(f, fc->line_gap - (units - (fc->hhea_ascent - fc->hhea_descent))); f->external = el > 0 ? el : 0;
  f->average = scale_x(f, fc->average); if (f->average < 1) f->average = 1;
  f->widest = scale_x(f, fc->bbox_width); f->weight = fc->weight ? fc->weight : 400;
  f->mono = quality == 3;   // NONANTIALIASED_QUALITY
}
const char *font_face_name(const FontSize *f) { return names[f->face]; }
uint32_t font_cp1252(uint8_t ch) {
  static const uint16_t high[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
  return ch >= 0x80 && ch < 0xA0 ? high[ch - 0x80] : ch;
}
uint32_t font_glyph(const FontSize *f, uint32_t ch) { return ch < 0x10000 ? (uint32_t)stbtt_FindGlyphIndex(&face_of(f->face)->info, (int)ch) : 0; }
uint32_t font_unicode_ranges(const FontSize *f, uint16_t *first, uint16_t *count, uint32_t max, uint32_t *glyphs) {
  Face *fc = face_of(f->face);
  for (uint32_t i = 0; i < fc->ranges && i < max && i < 512; i++) { first[i] = fc->range_first[i]; count[i] = fc->range_count[i]; }
  if (glyphs) *glyphs = fc->range_glyphs;
  return fc->ranges;
}
static int glyph_ok(Face *fc, uint32_t g) { return g < (uint32_t)fc->info.numGlyphs; }
int32_t font_advance(const FontSize *f, uint32_t g) {
  Face *fc = face_of(f->face); int adv = 0, lsb = 0;
  if (!glyph_ok(fc, g)) g = 0;
  stbtt_GetGlyphHMetrics(&fc->info, (int)g, &adv, &lsb);
  return scale_x(f, adv);
}
void font_abc(const FontSize *f, uint32_t g, int32_t *a, int32_t *b, int32_t *c) {
  Face *fc = face_of(f->face); int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!glyph_ok(fc, g)) g = 0;
  int32_t adv = font_advance(f, g), left = 0, right = 0;
  if (stbtt_GetGlyphBox(&fc->info, (int)g, &x0, &y0, &x1, &y1)) {   // in 26.6: floor of the left edge, ceiling of the right
    int64_t l = mul_fix(x0, f->x_scale), r = mul_fix(x1, f->x_scale);
    left = (int32_t)((l & ~63ll) >> 6); right = (int32_t)(((r + 63) & ~63ll) >> 6);
  }
  int32_t box = right - left; if (box < 1) box = 1;
  *a = left; *b = box; *c = adv - left - box;
}
int32_t font_kern(const FontSize *f, uint32_t l, uint32_t r) {
  Face *fc = face_of(f->face);
  if (!fc->info.kern) return 0;
  int32_t v = stbtt__GetGlyphKernInfoAdvance(&fc->info, (int)l, (int)r) * f->ppem;   // Wine's rounding, "closer to what Windows does"
  if (v < 0) v -= fc->upem / 2 + f->ppem; else if (v > 0) v += fc->upem / 2 + f->ppem;
  return v / fc->upem;
}
int32_t font_place(const FontSize *f, const uint32_t *glyphs, uint32_t count, int kerning, int32_t *dx) {
  int32_t sum = 0;
  for (uint32_t i = 0; i < count; i++) {
    int32_t d = font_advance(f, glyphs[i]) + (kerning && i + 1 < count ? font_kern(f, glyphs[i], glyphs[i + 1]) : 0);
    if (dx) dx[i] = d;
    sum += d;
  }
  return sum;
}
uint32_t font_kerning_pairs(const FontSize *f) { Face *fc = face_of(f->face); return fc->info.kern ? (uint32_t)stbtt_GetKerningTableLength(&fc->info) : 0; }

// Hinting, vertically only (what the TrueType hinting Wine's FreeType runs mostly comes to at these sizes): the
// heights of the blue zones go to whole pixels, a round glyph's overshoot onto the same pixel as the flat one's
// height, and every other height in between proportionally. Horizontal positions are left alone, as FreeType's
// default (v40) interpreter leaves them. Heights in font units times 4, for precision in stb_truetype's shorts.
enum { SUB = 4 };
static float snap_y(const Face *fc, const FontSize *f, float y) {
  float knot_from[16], knot_to[16]; int n = 0;
  for (int i = 0; i < fc->zones; i++) {
    float r = (float)fc->zone_ref[i], o = (float)fc->zone_shoot[i], to = floorf(r * f->scale + 0.5f) / f->scale;
    float shoot = f->mono ? to + floorf((o - r) * f->scale + 0.5f) / f->scale : to;   // a one-bit font keeps its overshoot, rounded
    knot_from[n] = r; knot_to[n++] = to; if (o != r) { knot_from[n] = o; knot_to[n++] = shoot; }
  }
  for (int i = 1; i < n; i++) for (int j = i; j > 0 && knot_from[j - 1] > knot_from[j]; j--) {   // in order of height
    float t = knot_from[j]; knot_from[j] = knot_from[j - 1]; knot_from[j - 1] = t; t = knot_to[j]; knot_to[j] = knot_to[j - 1]; knot_to[j - 1] = t; }
  if (!n) return y;
  if (y <= knot_from[0]) return y + knot_to[0] - knot_from[0];
  if (y >= knot_from[n - 1]) return y + knot_to[n - 1] - knot_from[n - 1];
  for (int i = 1; i < n; i++) if (y <= knot_from[i]) {
    float span = knot_from[i] - knot_from[i - 1];
    return span > 0 ? knot_to[i - 1] + (y - knot_from[i - 1]) * (knot_to[i] - knot_to[i - 1]) / span : knot_to[i];
  }
  return y;
}
uint8_t *font_render(const FontSize *f, uint32_t g, int32_t *width, int32_t *height, int32_t *left, int32_t *top) {
  Face *fc = face_of(f->face);
  *width = *height = *left = *top = 0;
  if (!glyph_ok(fc, g)) g = 0;
  pool_used = 0;
  stbtt_vertex *v = 0; int nv = stbtt_GetGlyphShape(&fc->info, (int)g, &v);
  if (nv <= 0) { STBTT_free(v, 0); return 0; }
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int i = 0; i < nv; i++) {
    v[i].x = (short)(v[i].x * SUB); v[i].cx = (short)(v[i].cx * SUB); v[i].cx1 = (short)(v[i].cx1 * SUB);
    v[i].y = (short)lrintf(snap_y(fc, f, v[i].y) * SUB); v[i].cy = (short)lrintf(snap_y(fc, f, v[i].cy) * SUB); v[i].cy1 = (short)lrintf(snap_y(fc, f, v[i].cy1) * SUB);
    if (v[i].type == STBTT_vmove || v[i].type == STBTT_vline || v[i].type == STBTT_vcurve || v[i].type == STBTT_vcubic) {
      if (v[i].x < minx) minx = v[i].x; if (v[i].x > maxx) maxx = v[i].x; if (v[i].y < miny) miny = v[i].y; if (v[i].y > maxy) maxy = v[i].y; }
  }
  float sc = f->scale / SUB;
  int x0 = (int)floorf(minx * sc), x1 = (int)ceilf(maxx * sc), y0 = (int)floorf(-maxy * sc), y1 = (int)ceilf(-miny * sc);
  int w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) { STBTT_free(v, 0); return 0; }
  uint8_t *px = pool_alloc((size_t)w * (size_t)h);
  if (!px) return 0;   // a glyph bigger than the pool: not drawn
  memset(px, 0, (size_t)w * (size_t)h);
  stbtt__bitmap bm = {w, h, w, px};
  stbtt_Rasterize(&bm, 0.35f, v, nv, sc, sc, 0, 0, x0, y0, 1, 0);
  STBTT_free(v, 0);
  for (int i = 0; i < w * h; i++) px[i] = f->mono ? (px[i] >= 128 ? 16 : 0) : (uint8_t)((px[i] * 16 + 128) / 256);
  *width = w; *height = h; *left = x0; *top = -y0;
  return px;
}

// ---- drawing ----
static uint8_t *pixel_at(FontTarget *t, int32_t x, int32_t y) { return t->bits + (size_t)(t->top_down ? y : t->height - 1 - y) * t->pitch; }
static uint32_t to_565_555(const FontTarget *t, uint32_t r, uint32_t g, uint32_t b) {
  return t->masks[1] == 0x7E0 ? (r >> 3) << 11 | (g >> 2) << 5 | b >> 3 : (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
}
static void put(FontTarget *t, int32_t x, int32_t y, uint32_t c) {   // c: a COLORREF
  uint8_t *row = pixel_at(t, x, y); uint32_t r = c & 0xFF, g = c >> 8 & 0xFF, b = c >> 16 & 0xFF;
  switch (t->bits_per_pixel) {
  case 32: row[4 * x] = (uint8_t)b; row[4 * x + 1] = (uint8_t)g; row[4 * x + 2] = (uint8_t)r; row[4 * x + 3] = 0; break;
  case 24: row[3 * x] = (uint8_t)b; row[3 * x + 1] = (uint8_t)g; row[3 * x + 2] = (uint8_t)r; break;
  case 16: { uint16_t v = (uint16_t)to_565_555(t, r, g, b); memcpy(row + 2 * x, &v, 2); break; }
  case 8: row[x] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8); break;   // no palette is kept: a grey level
  case 1: if (c & 0xFFFFFF) row[x / 8] |= (uint8_t)(0x80 >> (x & 7)); else row[x / 8] &= (uint8_t)~(0x80 >> (x & 7)); break;
  }
}
void font_fill(FontTarget *t, const FontRect *r, uint32_t color) {
  for (int32_t y = r->top < 0 ? 0 : r->top; y < r->bottom && y < t->height; y++)
    for (int32_t x = r->left < 0 ? 0 : r->left; x < r->right && x < t->width; x++) put(t, x, y, color);
}
// Wine's blend of a level of coverage (0..16) between the text's colour and what is there: the range a level
// reaches is bent by a fixed ramp (measured: white text on black gives exactly these values)
static const uint8_t ramp[17] = {0, 0x4d, 0x68, 0x7c, 0x8c, 0x9a, 0xa7, 0xb2, 0xbd, 0xc7, 0xd0, 0xd9, 0xe1, 0xe9, 0xf0, 0xf8, 0xff};
static uint8_t aa(uint32_t dst, uint32_t text, unsigned level) {
  uint32_t lo = ramp[level] * text / 0xFF, hi = ramp[16 - level] + (0xFF - ramp[16 - level]) * text / 0xFF;
  if (dst == text) return (uint8_t)dst;
  if (dst > text) return (uint8_t)(text + (dst - text) * (hi - text) / (0xFF - text));
  return (uint8_t)(text - (text - dst) * (text - lo) / text);
}
static void blend(FontTarget *t, int32_t x, int32_t y, uint32_t c, unsigned level) {
  if (level >= 16 || (t->bits_per_pixel != 32 && t->bits_per_pixel != 24)) { if (level >= 8) put(t, x, y, c); return; }
  uint8_t *p = pixel_at(t, x, y) + (t->bits_per_pixel / 8) * (uint32_t)x;
  p[0] = aa(p[0], c >> 16 & 0xFF, level); p[1] = aa(p[1], c >> 8 & 0xFF, level); p[2] = aa(p[2], c & 0xFF, level);
}
static int32_t floor_div2(int32_t v) { return v / 2; }   // C's truncation, as Wine's x -= width / 2
void font_text_out(const FontSize *f, FontTarget *t, int32_t x, int32_t y, uint32_t align, uint32_t options, const FontRect *rect,
                   const uint32_t *glyphs, uint32_t count, const int32_t *dx, uint32_t text_color, uint32_t bk_color, int opaque, int32_t *end_x, int32_t *end_y) {
  int pdy = (options & 0x2000) != 0;   // ETO_PDY
  int32_t width = 0, rise = 0;
  for (uint32_t i = 0; i < count; i++) { if (dx) { width += dx[pdy ? 2 * i : i]; if (pdy) rise += dx[2 * i + 1]; } else width += font_advance(f, glyphs[i]); }
  switch (align & 6) {   // TA_LEFT 0, TA_RIGHT 2, TA_CENTER 6
  case 6: x -= floor_div2(width); y -= floor_div2(rise); break;
  case 2: x -= width; y -= rise; break;
  }
  if (end_x) { *end_x = (align & 6) == 2 ? x : x + width; *end_y = (align & 6) == 2 ? y : y + rise; }
  switch (align & 24) { case 0: y += f->ascent; break; case 8: y -= f->descent; break; }   // TA_TOP 0, TA_BOTTOM 8, TA_BASELINE 24
  if ((options & 2) && rect) font_fill(t, rect, bk_color);   // ETO_OPAQUE
  if (opaque && !((options & 4) && (options & 2))) {
    if (!(options & 2) || !rect || x < rect->left || x + width >= rect->right || y - f->ascent < rect->top || y + f->descent >= rect->bottom) {
      FontRect box = {x, y - f->ascent, x + width, y + f->descent};
      if ((options & 4) && rect) { if (box.left < rect->left) box.left = rect->left; if (box.top < rect->top) box.top = rect->top; if (box.right > rect->right) box.right = rect->right; if (box.bottom > rect->bottom) box.bottom = rect->bottom; }
      font_fill(t, &box, bk_color);
    }
  }
  FontRect clip = {0, 0, t->width, t->height};
  if ((options & 4) && rect) { if (rect->left > clip.left) clip.left = rect->left; if (rect->top > clip.top) clip.top = rect->top; if (rect->right < clip.right) clip.right = rect->right; if (rect->bottom < clip.bottom) clip.bottom = rect->bottom; }
  for (uint32_t i = 0; i < count; i++) {
    int32_t w, h, left, top; uint8_t *px = font_render(f, glyphs[i], &w, &h, &left, &top);
    for (int32_t v = 0; px && v < h; v++) for (int32_t u = 0; u < w; u++) {
      int32_t X = x + left + u, Y = y - top + v; unsigned level = px[v * w + u];
      if (level && X >= clip.left && X < clip.right && Y >= clip.top && Y < clip.bottom) blend(t, X, Y, text_color, level);
    }
    if (dx) { x += dx[pdy ? 2 * i : i]; if (pdy) y += dx[2 * i + 1]; } else x += font_advance(f, glyphs[i]);
  }
}
