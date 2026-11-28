// SPDX-License-Identifier: GPL-2.0-or-later
// gdiprobe: asks the oracle's GDI about the fonts Narbacular Drop asks for, and what their text looks like, one
// fact a line, so that rt/windows-x86/gdi32.c can be checked against it (tools/oracle-wine/gdi-compare.py):
//   for each font (the LOGFONTs the game and its D3DX create, measured in our runtime with SW_TRACE=1): the face
//   GDI chose, TEXTMETRICW, GetFontLanguageInfo, the kerning pairs, GetFontUnicodeRanges, glyph indices, ABC
//   widths, GetCharacterPlacementA of the session's two texts; then the pixels: every glyph D3DX draws, drawn as
//   D3DX draws it (ExtTextOutW with ETO_GLYPH_INDEX | ETO_OPAQUE at 0,0, white on black, background mode OPAQUE,
//   into a 32-bit top-down DIB section first filled with grey 0x40), and the game's 256 characters drawn as the
//   game draws them (TextOutA at 32,64 aligned TA_CENTER | TA_BOTTOM, white on magenta, into a 64x64 DIB section
//   filled with magenta).
// Build and run (32-bit, in the app's prefix):
//   $MINGW/bin/i686-w64-mingw32-clang tools/oracle-wine/gdiprobe.c -o out/caps32/gdiprobe.exe -lgdi32 -luser32
//   tools/oracle-wine/oracle.sh run narbacular-drop out/caps32/gdiprobe.exe | tr -d '\r' > out/oracle/narbacular-drop/gdiprobe.txt
// The output holds glyph pixels of the fonts Wine picked (the host's): it stays under out/.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { int height, weight, italic, charset, quality; } Want;
static const Want fonts[] = {{60, 0, 0, 0, 3}, {24, 400, 0, 1, 4}, {48, 400, 1, 1, 4}, {48, 400, 0, 1, 4}};
static const char *texts[] = {"Polishing Up", "HINT: Escape from the cage, then weigh down both floor buttons."};

static void dump(const unsigned char *bits, int w, int pitch, int y0, int y1, int x1, int channel) {
  for (int y = y0; y < y1; y++) { printf("  "); for (int x = 0; x < x1; x++) printf("%02x", bits[y * pitch + 4 * x + channel]); printf("\n"); }
}
int main(void) {
  HDC dc = CreateCompatibleDC(0);
  printf("# What the oracle's GDI answers for Narbacular Drop's fonts. Made by tools/oracle-wine/gdiprobe.c.\n");
  for (unsigned f = 0; f < sizeof fonts / sizeof fonts[0]; f++) {
    const Want *w = &fonts[f];
    HFONT font = CreateFontA(w->height, 0, 0, 0, w->weight, w->italic, 0, 0, w->charset, 0, 0, w->quality, 0, f == 0 ? NULL : "");
    HGDIOBJ old = SelectObject(dc, font);
    char face[64] = {0}; GetTextFaceA(dc, sizeof face, face);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    printf("font %u height %d weight %d italic %d charset %d quality %d face \"%s\"\n", f, w->height, w->weight, w->italic, w->charset, w->quality, face);
    printf("metrics %u height %ld ascent %ld descent %ld internal %ld external %ld average %ld widest %ld weight %ld overhang %ld aspect %ld %ld chars %u %u %u %u italic %u underlined %u struck %u pitch 0x%02x charset %u\n",
      f, tm.tmHeight, tm.tmAscent, tm.tmDescent, tm.tmInternalLeading, tm.tmExternalLeading, tm.tmAveCharWidth, tm.tmMaxCharWidth, tm.tmWeight, tm.tmOverhang,
      tm.tmDigitizedAspectX, tm.tmDigitizedAspectY, tm.tmFirstChar, tm.tmLastChar, tm.tmDefaultChar, tm.tmBreakChar, tm.tmItalic, tm.tmUnderlined, tm.tmStruckOut, tm.tmPitchAndFamily, tm.tmCharSet);
    printf("language %u 0x%lx kerning_pairs %lu\n", f, GetFontLanguageInfo(dc), GetKerningPairsW(dc, 0, 0));
    { DWORD n = GetFontUnicodeRanges(dc, 0); GLYPHSET *g = malloc(n); GetFontUnicodeRanges(dc, g);
      printf("ranges %u size %lu glyphs %lu count %lu flags %lu:", f, n, g->cGlyphsSupported, g->cRanges, g->flAccel);
      for (DWORD i = 0; i < g->cRanges && i < 8; i++) printf(" %04x+%u", g->ranges[i].wcLow, g->ranges[i].cGlyphs); printf("\n"); free(g); }
    { WCHAR ch[256]; WORD gi[256]; for (int i = 0; i < 256; i++) ch[i] = (WCHAR)i; GetGlyphIndicesW(dc, ch, 256, gi, GGI_MARK_NONEXISTING_GLYPHS);
      printf("glyph_indices %u:", f); for (int i = 0; i < 256; i++) printf(" %u", gi[i]); printf("\n"); }
    { ABC abc[256]; INT wi[256]; GetCharABCWidthsI(dc, 0, 256, 0, abc); GetCharWidthI(dc, 0, 256, 0, wi);
      printf("abc_i %u:", f); for (int i = 0; i < 256; i++) printf(" %d,%u,%d", abc[i].abcA, abc[i].abcB, abc[i].abcC); printf("\n");
      printf("width_i %u:", f); for (int i = 0; i < 256; i++) printf(" %d", wi[i]); printf("\n");
      GetCharABCWidthsW(dc, 0, 255, abc); printf("abc_w %u:", f); for (int i = 0; i < 256; i++) printf(" %d,%u,%d", abc[i].abcA, abc[i].abcB, abc[i].abcC); printf("\n"); }
    for (unsigned t = 0; t < 2; t++) for (unsigned k = 0; k < 2; k++) {
      const char *s = texts[t]; int n = (int)strlen(s); GCP_RESULTSA r = {sizeof r}; WCHAR glyphs[128]; INT dx[128], caret[128]; UINT order[128]; char cls[128] = {0}, out[128];
      r.lpOutString = out; r.lpOrder = order; r.lpDx = dx; r.lpCaretPos = caret; r.lpClass = cls; r.lpGlyphs = glyphs; r.nGlyphs = 128;
      DWORD v = GetCharacterPlacementA(dc, s, n, 0, &r, k ? GCP_USEKERNING : 0);
      printf("placement %u text %u flags 0x%x result 0x%08lx glyphs %u fit %d:", f, t, k ? GCP_USEKERNING : 0, v, r.nGlyphs, r.nMaxFit);
      for (UINT i = 0; i < r.nGlyphs; i++) printf(" %u/%d/%d", glyphs[i], dx[i], caret[i]); printf("\n");
    }
    // the pixels
    int big = f == 0 ? 64 : 160, high = f == 0 ? 64 : tm.tmHeight;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), big, -high, 1, 32, BI_RGB}}; void *bits; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ oldbm = SelectObject(dc, bm);
    if (f == 0) {
      SetTextColor(dc, 0xFFFFFF); SetBkColor(dc, 0xFF00FF); SetTextAlign(dc, TA_CENTER | TA_BOTTOM); HBRUSH magenta = CreateSolidBrush(0xFF00FF); RECT all = {0, 0, 64, 64};
      for (int ch = 0; ch < 256; ch++) {
        FillRect(dc, &all, magenta); char c = (char)ch; TextOutA(dc, 32, 64, &c, 1); GdiFlush();
        printf("char %d\n", ch);   // a row: one character a pixel, '.' magenta, '#' white, '?' anything else
        for (int y = 0; y < 64; y++) { printf("  "); for (int x = 0; x < 64; x++) { unsigned p = ((unsigned *)bits)[y * 64 + x] & 0xFFFFFF; putchar(p == 0xFF00FF ? '.' : p == 0xFFFFFF ? '#' : '?'); } printf("\n"); }
      }
      DeleteObject(magenta);
    } else {
      SetBkMode(dc, OPAQUE); SetBkColor(dc, 0); SetTextColor(dc, 0xFFFFFF); SetTextAlign(dc, TA_LEFT | TA_TOP);
      char seen[256] = {0};
      for (unsigned t = 0; t < 2; t++) {
        const char *s = texts[t]; int n = (int)strlen(s); GCP_RESULTSA r = {sizeof r}; WCHAR glyphs[128]; r.lpGlyphs = glyphs; r.nGlyphs = 128;
        GetCharacterPlacementA(dc, s, n, 0, &r, 0);
        for (UINT i = 0; i < r.nGlyphs; i++) {
          WCHAR g = glyphs[i]; if (g < 256 && seen[g]) continue; if (g < 256) seen[g] = 1;
          memset(bits, 0x40, (size_t)big * high * 4); BOOL ok = ExtTextOutW(dc, 0, 0, ETO_GLYPH_INDEX | ETO_OPAQUE, 0, &g, 1, 0); GdiFlush();
          ABC abc; GetCharABCWidthsI(dc, g, 1, 0, &abc);
          int same = 1; for (int p = 0; p < big * high; p++) { unsigned char *q = (unsigned char *)bits + 4 * p; if (q[0] != q[1] || q[1] != q[2]) same = 0; }
          printf("glyph %u %u abc %d %u %d grey %d %dx%d drawn %d\n", f, g, abc.abcA, abc.abcB, abc.abcC, same, big, high, ok);   // rows of the blue channel, hex
          dump(bits, big, big * 4, 0, high, big, 0);
        }
      }
    }
    SelectObject(dc, oldbm); DeleteObject(bm);
    SelectObject(dc, old); DeleteObject(font);
  }
  DeleteDC(dc);
  fflush(stdout);
  return 0;
}
