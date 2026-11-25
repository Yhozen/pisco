// SPDX-License-Identifier: GPL-2.0-or-later
// The reference for fixtures/doom: doomgeneric on Linux with no window, the same virtual time and the same frame
// hash as doom_xbox.c's -hashframes. ./doom -iwad freedoom1.wad -timedemo demo1 prints one line per frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomgeneric.h"

char *dg_save_dir = ".";
static uint64_t virtual_ms;
static unsigned frames_drawn;

// -dumpframe N: the rows of frame N (a hash each, and the pixels of the rows the caller asks for with -dumprows a-b)
static int dump_frame = -1, dump_lo = -1, dump_hi = -1;
static void dump(unsigned n, const uint32_t *px) {
  if ((int)n != dump_frame) return;
  for (int y = 0; y < DOOMGENERIC_RESY; y++) {
    uint32_t h = 2166136261u;
    for (int x = 0; x < DOOMGENERIC_RESX; x++) { h = (h ^ (px[y * DOOMGENERIC_RESX + x] & 0xFFFFFF)) * 16777619u; }
    printf("row %d %08x\n", y, h);
    if (y >= dump_lo && y <= dump_hi) { printf("px %d", y); for (int x = 0; x < DOOMGENERIC_RESX; x += 2) printf(" %06x", px[y * DOOMGENERIC_RESX + x] & 0xFFFFFF); printf("\n"); }
  }
}
static void dump_args(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; i++) { if (!strcmp(argv[i], "-dumpframe")) dump_frame = atoi(argv[i + 1]); if (!strcmp(argv[i], "-dumprows")) sscanf(argv[i + 1], "%d-%d", &dump_lo, &dump_hi); }
}
void DG_Init(void) {}
void DG_DrawFrame(void) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) { uint32_t p = DG_ScreenBuffer[i]; h = (h ^ (p & 0xFF)) * 16777619u; h = (h ^ (p >> 8 & 0xFF)) * 16777619u; h = (h ^ (p >> 16 & 0xFF)) * 16777619u; }
  dump(frames_drawn, DG_ScreenBuffer);
  printf("frame %u %08x\n", frames_drawn++, h);
}
void DG_SleepMs(uint32_t ms) { virtual_ms += ms; }
uint32_t DG_GetTicksMs(void) { return (uint32_t)virtual_ms; }
int DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title) { (void)title; }
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  dump_args(argc, argv);
  doomgeneric_Create(argc, argv);
  for (;;) doomgeneric_Tick();
}
