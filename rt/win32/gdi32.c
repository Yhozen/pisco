// SPDX-License-Identifier: GPL-2.0-or-later
// gdi32 for Windows desktop titles: memory device contexts, DIB sections with real pixel memory, fonts, brushes
// and the stock objects, the text state of a device context, and the text functions a title and D3DX's font
// code call. Rectangles are filled for real. Text is not drawn yet and its measurements are estimates: a font's
// metrics are derived from the height asked for with the proportions of a common sans-serif face, every
// character has the average width, and there are no kerning pairs. Real glyphs come with the pixels (the next
// milestone needs a font to draw from). Uniscribe (usp10.dll) is absent, so D3DX takes its plain-GDI path.
#include "win32.h"

enum { T_DC = 1, T_BITMAP, T_FONT, T_BRUSH, T_PEN };
typedef struct {
  u8 type, stock; u32 handle;
  // a bitmap: a DIB section's pixels, or the 1x1 one a new memory DC holds
  s32 width, height; u32 bits_per_pixel, pitch, compression, masks[3]; u8 *bits; int top_down;
  // a font: LOGFONTA, and what it measures
  u8 logfont[60]; s32 cell, ascent, descent, internal, external, average, widest, weight;
  // a brush or a pen
  u32 color, style;
  // a device context
  u32 bitmap, font, brush, pen, text_color, bk_color, bk_mode, text_align, map_mode; s32 x, y;
} Gdi;
enum { OBJECTS = 1024, FIRST = 0x00100000u, STOCK = 0x00080000u };
static Gdi *objects[OBJECTS]; static Gdi stock[32];
static void init_stock(void);
static Gdi *get(u64 h, int type) {
  Gdi *o = 0; init_stock();
  if (h >= STOCK && h < STOCK + 32) o = stock[h - STOCK].type ? &stock[h - STOCK] : 0;
  else if (h >= FIRST && h < FIRST + 4 * OBJECTS && !(h & 3)) o = objects[(h - FIRST) / 4];
  return o && (!type || o->type == type) ? o : 0;
}
static Gdi *make(int type) {
  for (unsigned i = 1; i < OBJECTS; i++) if (!objects[i]) { Gdi *o = objects[i] = calloc(1, sizeof *o); o->type = (u8)type; o->handle = FIRST + 4 * i; return o; }
  rt_fatal("gdi32: out of object handles");
}
// What a font measures. The height asked for is the cell's (positive), the characters' (negative: the cell is
// taller by the internal leading), or the default (0). Proportions: a common sans-serif face at 2048 units an em
// (ascent 1854, descent 434, average character 904).
static void measure(Gdi *f) {
  s32 height; memcpy(&height, f->logfont, 4); s32 weight; memcpy(&weight, f->logfont + 16, 4);
  s32 em = height < 0 ? -height : height > 0 ? (height * 2048 + 1144) / 2288 : 12;
  f->ascent = (em * 1854 + 1024) / 2048; f->descent = (em * 434 + 1024) / 2048; f->cell = f->ascent + f->descent;
  if (height > 0) { f->cell = height; f->descent = height - f->ascent; }
  f->internal = f->cell - em; f->external = (em * 67 + 1024) / 2048; f->average = (em * 904 + 1024) / 2048; if (f->average < 1) f->average = 1;
  f->widest = 2 * em; f->weight = weight ? weight : 400;
}
static void init_stock(void) { // GetStockObject's: brushes 0..5, pens 6..8, fonts 10..17, and (ours, 20 and 21) what a new DC holds
  if (stock[0].type) return;
  static const u32 gray[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
  for (unsigned i = 0; i < 6; i++) { stock[i].type = T_BRUSH; stock[i].color = i < 5 ? gray[i] : 0; stock[i].style = i == 5; }   // BS_SOLID 0, BS_NULL 1
  for (unsigned i = 6; i < 9; i++) { stock[i].type = T_PEN; stock[i].color = i == 6 ? 0xFFFFFF : 0; stock[i].style = i == 8 ? 5 : 0; } // PS_NULL 5
  for (unsigned i = 10; i < 18; i++) if (i != 15) { stock[i].type = T_FONT; s32 h = i == 17 ? -11 : 16; memcpy(stock[i].logfont, &h, 4); strcpy((char *)stock[i].logfont + 28, i == 17 ? "Tahoma" : "System"); measure(&stock[i]); } // 17: DEFAULT_GUI_FONT
  stock[20].type = T_BITMAP; stock[20].width = stock[20].height = 1; stock[20].bits_per_pixel = 1; stock[20].pitch = 4; stock[20].bits = calloc(1, 4);
  for (unsigned i = 0; i < 32; i++) { stock[i].stock = 1; stock[i].handle = STOCK + i; }
}
IMP(gdi32, GetStockObject) { init_stock(); u32 n = (u32)ARG(0); RET(n < 20 && stock[n].type ? STOCK + n : 0); }
IMP(gdi32, CreateSolidBrush) { Gdi *o = make(T_BRUSH); o->color = (u32)ARG(0) & 0xFFFFFF; RET(o->handle); }
IMP(gdi32, DeleteObject) {
  Gdi *o = get(ARG(0), 0);
  if (!o || o->type == T_DC) { RET(0); return; }
  if (o->stock) { RET(1); return; }
  free(o->bits); objects[(o->handle - FIRST) / 4] = 0; free(o); RET(1);
}

// ---- device contexts: only memory ones (there is no screen to draw on with GDI) ----
IMP(gdi32, CreateCompatibleDC) {
  init_stock(); Gdi *d = make(T_DC);
  d->bitmap = STOCK + 20; d->font = STOCK + 13; d->brush = STOCK + 0; d->pen = STOCK + 7;   // the 1x1 bitmap, SYSTEM_FONT, WHITE_BRUSH, BLACK_PEN
  d->text_color = 0; d->bk_color = 0xFFFFFF; d->bk_mode = 2; d->map_mode = 1;                // black on white, OPAQUE, MM_TEXT
  RET(d->handle);
}
IMP(gdi32, DeleteDC) { Gdi *d = get(ARG(0), T_DC); if (!d) { RET(0); return; } objects[(d->handle - FIRST) / 4] = 0; free(d); RET(1); }
// SelectObject(dc, object): the object of that kind the DC held before, or 0
IMP(gdi32, SelectObject) {
  Gdi *d = get(ARG(0), T_DC), *o = get(ARG(1), 0);
  if (!d || !o) { RET(0); return; }
  u32 *slot = o->type == T_BITMAP ? &d->bitmap : o->type == T_FONT ? &d->font : o->type == T_BRUSH ? &d->brush : o->type == T_PEN ? &d->pen : 0;
  if (!slot) { RET(0); return; }
  u32 old = *slot; *slot = o->handle; RET(old);
}
// The setters return what was set before (or their own value for "no such DC")
#define SET(FIELD, INVALID) do { Gdi *d = get(ARG(0), T_DC); if (!d) { RET(INVALID); return; } u32 old = d->FIELD; d->FIELD = (u32)ARG(1); RET(old); } while (0)
IMP(gdi32, SetTextColor) { SET(text_color, 0xFFFFFFFFu); }
IMP(gdi32, SetBkColor) { SET(bk_color, 0xFFFFFFFFu); }
IMP(gdi32, SetBkMode) { SET(bk_mode, 0); }
IMP(gdi32, SetTextAlign) { SET(text_align, 0xFFFFFFFFu); }
IMP(gdi32, SetMapMode) { SET(map_mode, 0); }
IMP(gdi32, MoveToEx) { // (dc, x, y, POINT * for the old position)
  Gdi *d = get(ARG(0), T_DC); if (!d) { RET(0); return; }
  if (ARG(3)) { M32(ARG(3)) = (u32)d->x; M32(ARG(3) + 4) = (u32)d->y; }
  d->x = (s32)ARG(1); d->y = (s32)ARG(2); RET(1);
}

// ---- DIB sections ----
// CreateDIBSection(dc, BITMAPINFO *, usage, void **bits, section, offset): 1, 4, 8, 16, 24 or 32 bits a pixel,
// uncompressed (BI_RGB 0) or with colour masks (BI_BITFIELDS 3). Rows are padded to 4 bytes; a negative height
// puts the top row first. The pixels start as zero.
IMP(gdi32, CreateDIBSection) {
  u64 bi = ARG(1), out = ARG(3);
  if (out) M32(out) = 0;
  if (!bi || M32(bi) < 40 || ARG(4)) { RET(0); return; }
  s32 w = (s32)M32(bi + 4), h = (s32)M32(bi + 8); u32 bpp = M16(bi + 14), comp = M32(bi + 16);
  if (w <= 0 || !h || M16(bi + 12) != 1 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) { RET(0); return; }
  Gdi *b = make(T_BITMAP);
  b->width = w; b->height = h < 0 ? -h : h; b->top_down = h < 0; b->bits_per_pixel = bpp; b->compression = comp;
  b->pitch = (((u32)w * bpp + 31) / 32) * 4; b->bits = calloc(b->pitch, (size_t)b->height);
  if (comp == 3) memcpy(b->masks, (void *)GP(bi + M32(bi)), 12);
  else if (bpp == 16) { b->masks[0] = 0x7C00; b->masks[1] = 0x3E0; b->masks[2] = 0x1F; }
  else if (bpp >= 24) { b->masks[0] = 0xFF0000; b->masks[1] = 0xFF00; b->masks[2] = 0xFF; }
  if (out) M32(out) = (u32)(uptr)b->bits;
  RET(b->handle);
}
// A solid rectangle in the bitmap a DC holds (FillRect). COLORREF is 0x00bbggrr; a 32- or 24-bit DIB pixel is b, g, r.
int gdi_fill_rect(u32 hdc, const s32 *r, u32 hbrush) {
  init_stock();
  Gdi *d = get(hdc, T_DC), *br = get(hbrush, T_BRUSH);
  if (!br && hbrush >= 1 && hbrush <= 31) { static Gdi sys; sys.type = T_BRUSH; sys.color = 0xC0C0C0; br = &sys; }   // a system colour + 1: all the same grey here
  Gdi *b = d ? get(d->bitmap, T_BITMAP) : 0;
  if (!d || !br || !b) return 0;
  if (br->style == 1) return 1;
  u32 c = br->color, bgr = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
  for (s32 y = r[1] < 0 ? 0 : r[1]; y < r[3] && y < b->height; y++) {
    u8 *row = b->bits + (size_t)(b->top_down ? y : b->height - 1 - y) * b->pitch;
    for (s32 x = r[0] < 0 ? 0 : r[0]; x < r[2] && x < b->width; x++) {
      if (b->bits_per_pixel == 32) memcpy(row + 4 * x, &bgr, 4);
      else if (b->bits_per_pixel == 24) memcpy(row + 3 * x, &bgr, 3);
      else if (b->bits_per_pixel == 16) { u16 v = (u16)((bgr >> 19 & 0x1F) << (b->masks[1] == 0x7E0 ? 11 : 10) | (b->masks[1] == 0x7E0 ? (bgr >> 10 & 0x3F) << 5 : (bgr >> 11 & 0x1F) << 5) | (bgr >> 3 & 0x1F)); memcpy(row + 2 * x, &v, 2); }
      else if (b->bits_per_pixel == 8) row[x] = (u8)((77 * (c & 0xFF) + 150 * (c >> 8 & 0xFF) + 29 * (c >> 16 & 0xFF)) >> 8);
      else if (b->bits_per_pixel == 1) { if (c) row[x / 8] |= (u8)(0x80 >> (x & 7)); else row[x / 8] &= (u8)~(0x80 >> (x & 7)); }
    }
  }
  return 1;
}
// GetObject(object, size, buffer): BITMAP (24 bytes) or DIBSECTION (84) for a DIB section, LOGFONT for a font,
// LOGBRUSH (12) for a brush. With no buffer: the size the object's description has.
static u32 get_object(Cpu *c, int wide) {
  init_stock();
  Gdi *o = get(ARG(0), 0); u32 size = (u32)ARG(1); u64 out = ARG(2);
  if (!o) return 0;
  if (o->type == T_BITMAP) {
    if (!out) return 24;
    u8 ds[84] = {0}; u32 v;   // BITMAP: type, width, height, row bytes, planes (16 bit), bits a pixel (16 bit), bits; then BITMAPINFOHEADER, the masks, section, offset
    #define PUT(off, val) (v = (u32)(val), memcpy(ds + (off), &v, 4))
    PUT(4, o->width); PUT(8, o->height); PUT(12, o->pitch); ds[16] = 1; ds[18] = (u8)o->bits_per_pixel; PUT(20, (uptr)o->bits);
    PUT(24, 40); PUT(28, o->width); PUT(32, o->top_down ? -o->height : o->height); ds[36] = 1; ds[38] = (u8)o->bits_per_pixel; PUT(40, o->compression); PUT(44, o->pitch * (u32)o->height);
    memcpy(ds + 64, o->masks, 12);
    #undef PUT
    u32 n = size >= 84 && !o->stock ? 84 : size >= 24 ? 24 : 0;
    memcpy((void *)GP(out), ds, n); return n;
  }
  if (o->type == T_FONT) {
    u32 full = wide ? 92 : 60;
    if (!out) return full;
    if (size > full) size = full;
    if (!wide) memcpy((void *)GP(out), o->logfont, size);
    else { u8 w[92] = {0}; memcpy(w, o->logfont, 28); for (unsigned i = 0; i < 32; i++) w[28 + 2 * i] = o->logfont[28 + i]; memcpy((void *)GP(out), w, size); }
    return size;
  }
  if (o->type == T_BRUSH) { if (!out) return 12; if (size < 12) return 0; M32(out) = o->style; M32(out + 4) = o->color; M32(out + 8) = 0; return 12; }
  return 0;
}
IMP(gdi32, GetObjectA) { RET(get_object(c, 0)); }
IMP(gdi32, GetObjectW) { RET(get_object(c, 1)); }

// ---- fonts ----
// LOGFONTA: height, width, escapement, orientation, weight (5 longs), italic, underline, strike-out, character
// set, output precision, clip precision, quality, pitch and family (8 bytes), the face name (32 bytes)
static u32 font_new(const u8 *lf) { Gdi *f = make(T_FONT); memcpy(f->logfont, lf, 60); f->logfont[59] = 0; measure(f); return f->handle; }
IMP(gdi32, CreateFontIndirectA) { if (!ARG(0)) { RET(0); return; } RET(font_new((const u8 *)GP(ARG(0)))); }
IMP(gdi32, CreateFontIndirectW) {
  if (!ARG(0)) { RET(0); return; }
  u8 lf[60] = {0}; memcpy(lf, (void *)GP(ARG(0)), 28); const wchar16 *face = GWSTR(ARG(0) + 28);
  for (unsigned i = 0; i < 31 && face[i]; i++) lf[28 + i] = face[i] < 0x100 ? (u8)face[i] : '?';
  RET(font_new(lf));
}
IMP(gdi32, CreateFontA) { // the fields of a LOGFONTA, one argument each
  u8 lf[60] = {0}; u32 v;
  for (unsigned i = 0; i < 5; i++) { v = (u32)ARG(i); memcpy(lf + 4 * i, &v, 4); }
  for (unsigned i = 0; i < 8; i++) lf[20 + i] = (u8)ARG(5 + i);
  if (ARG(13)) strncpy((char *)lf + 28, GSTR(ARG(13)), 31);
  RET(font_new(lf));
}
static Gdi *font_of(u64 hdc) { init_stock(); Gdi *d = get(hdc, T_DC); return d ? get(d->font, T_FONT) : 0; }
// TEXTMETRIC: eleven longs, then the first, last, default and break characters (bytes, or 16-bit in the W form),
// italic, underlined, struck out, pitch and family, character set
static u32 text_metrics(Cpu *c, int wide) {
  Gdi *f = font_of(ARG(0)); u64 p = ARG(1);
  if (!f || !p) return 0;
  memset((void *)GP(p), 0, wide ? 60 : 56);
  s32 v[11] = {f->cell, f->ascent, f->descent, f->internal, f->external, f->average, f->widest, f->weight, 0, 96, 96};
  memcpy((void *)GP(p), v, 44);
  u64 q = p + 44;
  if (wide) { M16(q) = 0x20; M16(q + 2) = 0xFF; M16(q + 4) = 0x1F; M16(q + 6) = 0x20; q += 8; } else { M8(q) = 0x20; M8(q + 1) = 0xFF; M8(q + 2) = 0x1F; M8(q + 3) = 0x20; q += 4; }
  M8(q) = f->logfont[20]; M8(q + 1) = f->logfont[21]; M8(q + 2) = f->logfont[22];
  M8(q + 3) = 0x27;   // a variable-pitch TrueType vector font of the Swiss family
  M8(q + 4) = f->logfont[23] == 1 ? 0 : f->logfont[23];   // DEFAULT_CHARSET resolves to ANSI
  return 1;
}
IMP(gdi32, GetTextMetricsA) { RET(text_metrics(c, 0)); }
IMP(gdi32, GetTextMetricsW) { RET(text_metrics(c, 1)); }
// What the font can do beyond plain left-to-right placement (kerning, glyph shaping, reordering): nothing
IMP(gdi32, GetFontLanguageInfo) { RET(font_of(ARG(0)) ? 0 : 0xFFFFFFFFu); }

// ---- text: measured with the estimates above, not drawn yet ----
static void not_drawn(const char *fn) { static int told; if (!told++ && rt_trace) rt_log("gdi32: %s and the other text functions draw nothing yet", fn); }
IMP(gdi32, TextOutA) { not_drawn("TextOutA"); RET(get(ARG(0), T_DC) != 0); }       // (dc, x, y, text, count)
IMP(gdi32, ExtTextOutA) { not_drawn("ExtTextOutA"); RET(get(ARG(0), T_DC) != 0); } // (dc, x, y, options, RECT *, text, count, advances)
IMP(gdi32, ExtTextOutW) { not_drawn("ExtTextOutW"); RET(get(ARG(0), T_DC) != 0); }
// GetCharacterPlacement(dc, text, count, widest extent, GCP_RESULTS *, flags): the text in order, a glyph a
// character (the glyph's index is the character's code), each as wide as the average. GCP_RESULTS: size, output
// string, order, advances, caret positions, classes, glyphs, glyph count, characters that fit.
// Returns the text's width and the font's height.
static u32 placement(Cpu *c, int wide) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 r = ARG(4);
  if (!f) return 0;
  if (r) {
    u32 room = M32(r + 28); if (M32(r + 24) && room < n) n = room;
    for (u32 i = 0; i < n; i++) {
      u32 ch = wide ? GWSTR(ARG(1))[i] : (u8)GSTR(ARG(1))[i];
      if (M32(r + 4)) { if (wide) M16(M32(r + 4) + 2 * i) = (u16)ch; else M8(M32(r + 4) + i) = (u8)ch; }
      if (M32(r + 8)) M32(M32(r + 8) + 4 * i) = i;
      if (M32(r + 12)) M32(M32(r + 12) + 4 * i) = (u32)f->average;
      if (M32(r + 16)) M32(M32(r + 16) + 4 * i) = i * (u32)f->average;
      if (M32(r + 24)) M16(M32(r + 24) + 2 * i) = (u16)ch;
    }
    M32(r + 28) = n; M32(r + 32) = n;
  }
  return (n * (u32)f->average & 0xFFFF) | (u32)f->cell << 16;
}
IMP(gdi32, GetCharacterPlacementA) { RET(placement(c, 0)); }
IMP(gdi32, GetCharacterPlacementW) { RET(placement(c, 1)); }
// Glyphs by index (the index is the character's code here). GetCharABCWidthsI(dc, first, count, indices or
// null, ABC *): the space before the glyph, its width, the space after; GetCharWidthI the same as one advance.
// D3DX's font needs both to exist on Windows 2000 and later, with GetFontUnicodeRanges.
IMP(gdi32, GetCharABCWidthsI) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 out = ARG(4);
  if (!f || !out) { RET(0); return; }
  for (u32 i = 0; i < n; i++) { M32(out + 12 * i) = 0; M32(out + 12 * i + 4) = (u32)f->average; M32(out + 12 * i + 8) = 0; }
  RET(1);
}
IMP(gdi32, GetCharWidthI) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 out = ARG(4);
  if (!f || !out) { RET(0); return; }
  for (u32 i = 0; i < n; i++) M32(out + 4 * i) = (u32)f->average;
  RET(1);
}
// GetFontUnicodeRanges(dc, GLYPHSET * or null): size, flags, glyphs supported, ranges, then each range's first
// character and length (16 bits each). The characters of code page 1252's Latin-1 part: 0x20..0x7E and 0xA0..0xFF.
IMP(gdi32, GetFontUnicodeRanges) {
  u64 p = ARG(1);
  if (!font_of(ARG(0))) { RET(0); return; }
  if (p) { M32(p) = 24; M32(p + 4) = 0; M32(p + 8) = 95 + 96; M32(p + 12) = 2; M16(p + 16) = 0x20; M16(p + 18) = 95; M16(p + 20) = 0xA0; M16(p + 22) = 96; }
  RET(24);
}
