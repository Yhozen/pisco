// SPDX-License-Identifier: GPL-2.0-or-later
// The fonts behind gdi32 (font.c): what a LOGFONT becomes, its measurements and its glyphs, the way Wine's GDI
// (FreeType underneath) computes them on the oracle's machine. No Cpu, no guest memory: tests build it natively.
#pragma once
#include <stdint.h>

typedef struct {
  int face;                       // FONT_SANS, FONT_SANS_ITALIC
  int ppem;                       // pixels an em
  int64_t x_scale, y_scale;       // font units to 26.6 pixels, 16.16 fixed point (FreeType's)
  float scale;                    // the same as a float, for the rasteriser
  int32_t height, ascent, descent, internal, external, average, widest, weight;   // TEXTMETRIC's
  int mono;                       // NONANTIALIASED_QUALITY: one bit a pixel; else 17 levels of grey
} FontSize;
enum { FONT_SANS, FONT_SANS_ITALIC, FONT_FACES };

// The face a LOGFONT gets and its size. lf_height as in LOGFONT (cell height if positive, character height if
// negative, 0 for the default); quality is LOGFONT's lfQuality.
void font_select(FontSize *f, int32_t lf_height, int italic, int quality);
const char *font_face_name(const FontSize *f);
// A character of code page 1252 (the A functions' text) as Unicode
uint32_t font_cp1252(uint8_t ch);
// A character's glyph (0 if the face has none); GetFontUnicodeRanges' ranges, into first/count (up to max of
// them): the number of ranges, and in *glyphs the number of characters covered
uint32_t font_glyph(const FontSize *f, uint32_t codepoint);
uint32_t font_unicode_ranges(const FontSize *f, uint16_t *first, uint16_t *count, uint32_t max, uint32_t *glyphs);
// A glyph's advance and its ABC widths (GetCharABCWidthsI: the space before its black box, the box, the space after)
int32_t font_advance(const FontSize *f, uint32_t glyph);
void font_abc(const FontSize *f, uint32_t glyph, int32_t *a, int32_t *b, int32_t *c);
// Kerning: the adjustment between two glyphs, in pixels; the number of pairs the face's kern table has
int32_t font_kern(const FontSize *f, uint32_t left, uint32_t right);
uint32_t font_kerning_pairs(const FontSize *f);
// GetCharacterPlacement's advances: each glyph's, with GCP_USEKERNING adjusted by the pair it starts; the sum
int32_t font_place(const FontSize *f, const uint32_t *glyphs, uint32_t count, int kerning, int32_t *dx);
// A glyph's pixels: width x height values, 0..16 (or 0 / 16 for a mono font), the top left one at (left, -top)
// from the pen on the baseline (y grows downwards). Returns font.c's own buffer, good until the next call; 0 for a
// glyph with no pixels.
uint8_t *font_render(const FontSize *f, uint32_t glyph, int32_t *width, int32_t *height, int32_t *left, int32_t *top);

// ---- drawing text into a DIB section ----
typedef struct { uint8_t *bits; int32_t width, height; uint32_t pitch, bits_per_pixel, masks[3]; int top_down; } FontTarget;
typedef struct { int32_t left, top, right, bottom; } FontRect;
// ExtTextOutW's work, as Wine's GDI does it: glyphs (already indices) from (x, y) aligned by align (TA_*), each
// advanced by its own advance or by dx[i] (two values a glyph with ETO_PDY); with ETO_OPAQUE and a rectangle the
// rectangle is filled with bk_color, and with an opaque background mode the text's box too; ETO_CLIPPED keeps the
// pixels inside the rectangle. Colours are COLORREFs. *end gets the pen's position after the text (TA_UPDATECP).
void font_text_out(const FontSize *f, FontTarget *t, int32_t x, int32_t y, uint32_t align, uint32_t options, const FontRect *rect,
                   const uint32_t *glyphs, uint32_t count, const int32_t *dx, uint32_t text_color, uint32_t bk_color, int opaque, int32_t *end_x, int32_t *end_y);
void font_fill(FontTarget *t, const FontRect *r, uint32_t color);

// For a native test: use these bytes for a face instead of the ones built in
void font_set_data(int face, const uint8_t *data);
