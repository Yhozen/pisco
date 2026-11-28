// SPDX-License-Identifier: GPL-2.0-or-later
// gdi32 for Windows desktop apps: memory device contexts, DIB sections with real pixel memory, fonts, brushes
// and the stock objects, the text state of a device context, and the text functions an app and D3DX's font
// code call. Rectangles are filled and text is drawn for real, into the DIB section a DC holds. Fonts, their
// measurements and their glyphs are font.c's: Liberation Sans for every face name, measured and drawn as Wine's
// GDI does on the oracle's machine. Uniscribe (usp10.dll) is absent, so D3DX takes its plain-GDI path
// (GetCharacterPlacement, then ExtTextOutW with glyph indices): docs/narbacular-drop.md says why.
#include "win32.h"
#include "font.h"

enum { T_DC = 1, T_BITMAP, T_FONT, T_BRUSH, T_PEN };
typedef struct {
  u8 type, stock; u32 handle;
  // a bitmap: a DIB section's pixels, or the 1x1 one a new memory DC holds
  s32 width, height; u32 bits_per_pixel, pitch, compression, masks[3]; u8 *bits; int top_down;
  // a font: LOGFONTA, and the face and size it gets
  u8 logfont[60]; FontSize fs;
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
// The face and size a LOGFONT gets: its height (the cell's if positive, the characters' if negative, the default
// if 0), italic, quality (font.c)
static void measure(Gdi *f) { s32 height; memcpy(&height, f->logfont, 4); font_select(&f->fs, height, f->logfont[20] != 0, f->logfont[26]); }
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
static u32 font_new(const u8 *lf) {
  Gdi *f = make(T_FONT); memcpy(f->logfont, lf, 60); f->logfont[59] = 0; measure(f);
  if (rt_trace) { s32 h, w; memcpy(&h, lf, 4); memcpy(&w, lf + 16, 4); rt_log("gdi32: font \"%s\" height %d weight %d%s quality %u: %s, %d pixels an em", (const char *)lf + 28, h, w, lf[20] ? " italic" : "", lf[26], font_face_name(&f->fs), f->fs.ppem); }
  return f->handle;
}
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
  const FontSize *s = &f->fs;
  s32 v[11] = {s->height, s->ascent, s->descent, s->internal, s->external, s->average, s->widest, s->weight, 0, 96, 96};
  memcpy((void *)GP(p), v, 44);
  u64 q = p + 44;   // the characters: Wine's answer for the W form (the face goes up to U+FFFC); the A form stops at 255
  if (wide) { M16(q) = 0x20; M16(q + 2) = 0xFFFC; M16(q + 4) = 0x1F; M16(q + 6) = 0x20; q += 8; } else { M8(q) = 0x20; M8(q + 1) = 0xFF; M8(q + 2) = 0x1F; M8(q + 3) = 0x20; q += 4; }
  M8(q) = f->logfont[20] ? 0xFF : 0; M8(q + 1) = f->logfont[21]; M8(q + 2) = f->logfont[22];
  M8(q + 3) = 0x27;   // a variable-pitch TrueType vector font of the Swiss family
  M8(q + 4) = f->logfont[23] == 1 ? 0 : f->logfont[23];   // DEFAULT_CHARSET resolves to ANSI
  return 1;
}
IMP(gdi32, GetTextMetricsA) { RET(text_metrics(c, 0)); }
IMP(gdi32, GetTextMetricsW) { RET(text_metrics(c, 1)); }
// What the font can do beyond plain left-to-right placement. Wine's GDI answers GCP_USEKERNING (8) for Liberation
// Sans, which has kerning pairs, and D3DX's GDI path then asks GetCharacterPlacement to kern. But D3DX takes that
// path only because usp10.dll is missing here: on Windows and under Wine it places glyphs with Uniscribe, which
// leaves the kern table's pairs out (measured: Wine's "HINT:" has the T and the colon 2 pixels further apart than
// a kerned placement). Nothing here, so D3DX places its glyphs as it does there.
IMP(gdi32, GetFontLanguageInfo) { RET(font_of(ARG(0)) ? 0 : 0xFFFFFFFFu); }

// ---- text ----
// The bitmap a DC holds, as font.c draws into it
static int target_of(Gdi *d, FontTarget *t) {
  Gdi *b = get(d->bitmap, T_BITMAP);
  if (!b || !b->bits) return 0;
  t->bits = b->bits; t->width = b->width; t->height = b->height; t->pitch = b->pitch; t->bits_per_pixel = b->bits_per_pixel; t->top_down = b->top_down;
  memcpy(t->masks, b->masks, sizeof t->masks);
  return 1;
}
enum { TEXT_MAX = 4096 };
// ExtTextOut's work: glyphs from (x, y) in the DC's font and colours, aligned as the DC says; with TA_UPDATECP from
// and to the DC's current position. options: ETO_OPAQUE 2, ETO_CLIPPED 4, ETO_GLYPH_INDEX 0x10, ETO_PDY 0x2000.
static int text_out(Cpu *c, u64 hdc, s32 x, s32 y, u32 options, u64 rect, const u32 *glyphs, u32 n, u64 dx) {
  Gdi *d = get(hdc, T_DC), *f = d ? get(d->font, T_FONT) : 0;
  if (!d || !f) return 0;
  FontTarget t; FontRect r, *rp = 0; s32 *adv = 0;
  if (rect) { r.left = (s32)M32(rect); r.top = (s32)M32(rect + 4); r.right = (s32)M32(rect + 8); r.bottom = (s32)M32(rect + 12); rp = &r; }
  static s32 advances[2 * TEXT_MAX];   // (not from the heap, which is the app's too)
  if (dx) { u32 k = (options & 0x2000) ? 2 * n : n; adv = advances; for (u32 i = 0; i < k; i++) adv[i] = (s32)M32(dx + 4 * i); }
  if (d->text_align & 1) { x = d->x; y = d->y; }   // TA_UPDATECP
  s32 ex, ey;
  if (target_of(d, &t)) font_text_out(&f->fs, &t, x, y, d->text_align, options, rp, glyphs, n, adv, d->text_color, d->bk_color, d->bk_mode == 2, &ex, &ey);
  else { FontTarget none = {0}; font_text_out(&f->fs, &none, x, y, d->text_align, options & ~6u, 0, glyphs, n, adv, 0, 0, 0, &ex, &ey); }   // no pixels: only where the pen ends
  if (d->text_align & 1) { d->x = ex; d->y = ey; }
  return 1;
}
// A string's glyphs: code page 1252 (A) or UTF-16 (W) characters through the face's character map, or glyph indices as they are
static u32 glyphs_of(Gdi *f, u64 str, u32 n, int wide, int indices, u32 *out) {
  if (n > TEXT_MAX) n = TEXT_MAX;
  for (u32 i = 0; i < n; i++) {
    u32 ch = wide ? GWSTR(str)[i] : font_cp1252((u8)GSTR(str)[i]);
    out[i] = indices ? ch : font_glyph(&f->fs, ch);
  }
  return n;
}
static u32 text_call(Cpu *c, int wide, int ext) {   // TextOutA(dc, x, y, text, count); ExtTextOut(dc, x, y, options, RECT *, text, count, advances)
  Gdi *f = font_of(ARG(0));
  if (!f) return 0;
  u32 options = ext ? (u32)ARG(3) : 0; u64 str = ext ? ARG(5) : ARG(3); u32 n = (u32)(ext ? ARG(6) : ARG(4));
  static u32 glyphs[TEXT_MAX];
  n = str ? glyphs_of(f, str, n, wide, wide && (options & 0x10), glyphs) : 0;
  return (u32)text_out(c, ARG(0), (s32)ARG(1), (s32)ARG(2), options, ext ? ARG(4) : 0, glyphs, n, ext ? ARG(7) : 0);
}
IMP(gdi32, TextOutA) { RET(text_call(c, 0, 0)); }
IMP(gdi32, ExtTextOutA) { RET(text_call(c, 0, 1)); }
IMP(gdi32, ExtTextOutW) { RET(text_call(c, 1, 1)); }
// GetCharacterPlacement(dc, text, count, widest extent, GCP_RESULTS *, flags), as Wine does it without
// GCP_REORDER: the text in order, the face's glyphs, each glyph's advance (adjusted by the kerning pair it starts
// with GCP_USEKERNING, 8), the caret positions. GCP_RESULTS: size, output string, order, advances, caret
// positions, classes, glyphs, glyph count, characters that fit. Returns the text's width and the font's height.
static u32 placement(Cpu *c, int wide) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 r = ARG(4);
  if (!f || !n) return 0;
  if (n > TEXT_MAX) n = TEXT_MAX;
  if (r && M32(r + 28) < n) n = M32(r + 28);
  static u32 glyphs[TEXT_MAX]; static s32 dx[TEXT_MAX];
  glyphs_of(f, ARG(1), n, wide, 0, glyphs);
  s32 width = font_place(&f->fs, glyphs, n, (ARG(5) & 8) != 0, dx);
  if (r) {
    s32 caret = 0;
    for (u32 i = 0; i < n; i++) {
      if (M32(r + 4)) { if (wide) M16(M32(r + 4) + 2 * i) = GWSTR(ARG(1))[i]; else M8(M32(r + 4) + i) = (u8)GSTR(ARG(1))[i]; }
      if (M32(r + 8)) M32(M32(r + 8) + 4 * i) = i;
      if (M32(r + 12)) M32(M32(r + 12) + 4 * i) = (u32)dx[i];
      if (M32(r + 16)) M32(M32(r + 16) + 4 * i) = (u32)caret;
      if (M32(r + 24)) M16(M32(r + 24) + 2 * i) = (u16)glyphs[i];
      caret += dx[i];
    }
    M32(r + 28) = n; M32(r + 32) = n;
  }
  return ((u32)width & 0xFFFF) | (u32)f->fs.height << 16;
}
IMP(gdi32, GetCharacterPlacementA) { RET(placement(c, 0)); }
IMP(gdi32, GetCharacterPlacementW) { RET(placement(c, 1)); }
// Glyphs by index. GetCharABCWidthsI(dc, first, count, indices (16-bit) or null for first..first+count-1, ABC *):
// the space before the glyph's black box, the box, the space after; GetCharWidthI the same as one advance. D3DX's
// font needs both to exist on Windows 2000 and later, with GetFontUnicodeRanges.
static u32 index_at(Cpu *c, u32 i) { return ARG(3) ? M16(ARG(3) + 2 * i) : (u32)ARG(1) + i; }
IMP(gdi32, GetCharABCWidthsI) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 out = ARG(4);
  if (!f || !out) { RET(0); return; }
  for (u32 i = 0; i < n; i++) { s32 a, b, cc; font_abc(&f->fs, index_at(c, i), &a, &b, &cc); M32(out + 12 * i) = (u32)a; M32(out + 12 * i + 4) = (u32)b; M32(out + 12 * i + 8) = (u32)cc; }
  RET(1);
}
IMP(gdi32, GetCharWidthI) {
  Gdi *f = font_of(ARG(0)); u32 n = (u32)ARG(2); u64 out = ARG(4);
  if (!f || !out) { RET(0); return; }
  for (u32 i = 0; i < n; i++) M32(out + 4 * i) = (u32)font_advance(&f->fs, index_at(c, i));
  RET(1);
}
// GetFontUnicodeRanges(dc, GLYPHSET * or null): size, flags, characters covered, ranges, then each range's first
// character and length (16 bits each): the face's character map
IMP(gdi32, GetFontUnicodeRanges) {
  Gdi *f = font_of(ARG(0)); u64 p = ARG(1);
  if (!f) { RET(0); return; }
  static u16 first[512], count[512]; u32 glyphs, n = font_unicode_ranges(&f->fs, first, count, 512, &glyphs); if (n > 512) n = 512;
  if (p) { M32(p) = 16 + 4 * n; M32(p + 4) = 0; M32(p + 8) = glyphs; M32(p + 12) = n; for (u32 i = 0; i < n; i++) { M16(p + 16 + 4 * i) = first[i]; M16(p + 18 + 4 * i) = count[i]; } }
  RET(16 + 4 * n);
}

// ---- a window's DC as OpenGL draws into it: pixel formats, the screen's numbers, SwapBuffers ----
// The formats offered, as DescribePixelFormat numbers them from 1: a 32-bit colour buffer with 24 bits of depth and
// 8 of stencil, double-buffered, drawn by the "hardware" (not PFD_GENERIC_FORMAT); then the same without stencil,
// and a 16-bit one. Which one a window has is kept, but every one draws into the same buffers (opengl32.c).
static const u8 formats[][4] = {{32, 24, 8, 8}, {32, 24, 0, 8}, {16, 16, 0, 0}};   // colour, depth, stencil, alpha bits
enum { FORMATS = sizeof formats / sizeof formats[0] };
static u32 window_format;
static void describe(u32 i, u64 p) {   // PIXELFORMATDESCRIPTOR, 40 bytes
  const u8 *f = formats[i - 1];
  memset((void *)GP(p), 0, 40);
  M16(p) = 40; M16(p + 2) = 1; M32(p + 4) = 0x4 | 0x20 | 0x1;   // PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
  M8(p + 8) = 0; M8(p + 9) = f[0];                              // PFD_TYPE_RGBA, colour bits
  if (f[0] == 32) { M8(p + 10) = 8; M8(p + 11) = 16; M8(p + 12) = 8; M8(p + 13) = 8; M8(p + 14) = 8; M8(p + 15) = 0; M8(p + 16) = f[3]; M8(p + 17) = 24; }
  else { M8(p + 10) = 5; M8(p + 11) = 11; M8(p + 12) = 6; M8(p + 13) = 5; M8(p + 14) = 5; M8(p + 15) = 0; }
  M8(p + 23) = f[1]; M8(p + 24) = f[2];
}
IMP(gdi32, DescribePixelFormat) { // (dc, index, size, PIXELFORMATDESCRIPTOR *): the number of formats
  u32 i = (u32)ARG(1);
  if (ARG(3) && i >= 1 && i <= FORMATS && (u32)ARG(2) >= 40) describe(i, ARG(3));
  RET(FORMATS);
}
IMP(gdi32, ChoosePixelFormat) { // the first format with at least the depth and colour asked for
  u64 p = ARG(1); u32 color = M8(p + 9), depth = M8(p + 23), stencil = M8(p + 24);
  for (u32 i = 1; i <= FORMATS; i++) if (formats[i - 1][0] >= color && formats[i - 1][1] >= depth && formats[i - 1][2] >= stencil) { RET(i); return; }
  RET(1);
}
IMP(gdi32, SetPixelFormat) { u32 i = (u32)ARG(1); if (i < 1 || i > FORMATS) { SET_LAST_ERROR(2000); RET(0); return; } window_format = i; RET(1); }   // ERROR_INVALID_PIXEL_FORMAT
IMP(gdi32, GetPixelFormat) { RET(window_format); }
// what OpenGL does at the end of a frame (opengl32.c, when an app links it)
__attribute__((weak)) void gl_swap_buffers(u32 hdc) { (void)hdc; }
IMP(gdi32, SwapBuffers) { gl_swap_buffers((u32)ARG(0)); RET(1); }
// GetDeviceCaps(dc, index): the screen's (user32's display mode)
IMP(gdi32, GetDeviceCaps) {
  s32 w = 0, h = 0; win32_screen_size(&w, &h);
  switch ((s32)ARG(1)) {
  case 8: RET(w); return; case 10: RET(h); return;                 // HORZRES, VERTRES
  case 12: RET(32); return; case 14: RET(1); return;               // BITSPIXEL, PLANES
  case 4: RET(w * 254 / 960); return; case 6: RET(h * 254 / 960); return;   // HORZSIZE, VERTSIZE in mm at 96 dpi
  case 88: case 90: RET(96); return;                               // LOGPIXELSX, LOGPIXELSY
  case 116: RET(60); return;                                       // VREFRESH
  case 2: RET(1); return;                                          // TECHNOLOGY: DT_RASDISPLAY
  case 24: RET(-1); return;                                        // NUMCOLORS: more than 256
  case 40: RET(0); return;                                         // RASTERCAPS
  default: RET(0); return;
  }
}
// No gamma ramp to set (as under Wine without a hardware one): apps fall back to their own
IMP(gdi32, GetDeviceGammaRamp) { RET(0); }
IMP(gdi32, SetDeviceGammaRamp) { RET(0); }
IMP(gdi32, CreateDCA) { RET(0x01000000u); }   // the screen's DC ("DISPLAY")
IMP(gdi32, StretchBlt) { RET(1); }            // a DC that is not a memory DC's bitmap: nothing kept to stretch
IMP(gdi32, Rectangle) { RET(1); }
IMP(gdi32, SetROP2) { RET(13); }              // the old mix mode: R2_COPYPEN
