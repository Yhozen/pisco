// SPDX-License-Identifier: GPL-2.0-or-later
// xg_xs.dll, the console's texture layout library.
#include "../gdk/d3d12.h"

// ---- xg_xs.dll: the console's texture layout library. Our layout is the one the GPU back end reads texels
// in app memory with: subresources one after another as GetCopyableFootprints places them (rows 256-byte
// aligned). It is not the console's tiling: textures the app lays out through XG at run time come out
// right; texels tiled ahead of time by Microsoft's tools would not. ----
typedef struct { u64 vtbl; u32 magic; s32 refs; u8 desc[56]; } XgComputer;
#define XGMAGIC 0x58474343u
IMP(xg_xs, XGCreateTextureComputer) { // (desc, &computer)
  static const char *const names[] = {"AddRef", "Release", 0, "GetResourceSizeBytes", "GetResourceSizeAlignment", "GetMipLevelOffsetBytes", 0, 0, "CopyIntoSubresource", "CopyFromSubresource", 0, 0};
  static u64 vt; if (!vt) vt = rt_com_vtable("xg_xs", "Computer_", names, 12);
  if (!ARG(0) || !ARG(1)) { RET(0x80004003u); return; }
  XgComputer *x = calloc(1, sizeof *x); x->vtbl = vt; x->magic = XGMAGIC; x->refs = 1; memcpy(x->desc, (void *)GP(ARG(0)), 56);
  M64(ARG(1)) = (uptr)x; RET(S_OK);
}
IMP(xg_xs, Computer_AddRef) { XgComputer *x = (XgComputer *)GP(ARG(0)); RET(++x->refs); }
IMP(xg_xs, Computer_Release) { XgComputer *x = (XgComputer *)GP(ARG(0)); s32 r = --x->refs; if (!r) { x->magic = 0; free(x); } RET(r < 0 ? 0 : r); }
IMP(xg_xs, Computer_GetResourceSizeBytes) { RET(d3d12_alloc_info(((XgComputer *)GP(ARG(0)))->desc)); }
IMP(xg_xs, Computer_GetResourceSizeAlignment) { RET(65536); }
static u32 subresources(const u8 *desc) { u16 mips = *(u16 *)(desc + 30); return (*(u32 *)desc == 4 ? 1 : *(u16 *)(desc + 28)) * (mips ? mips : 1); }
IMP(xg_xs, Computer_GetMipLevelOffsetBytes) { // (plane, mip): of the first slice
  XgComputer *x = (XgComputer *)GP(ARG(0)); u8 fp[32]; u32 mip = (u32)ARG(2);
  if (mip >= subresources(x->desc)) { RET(0); return; }
  d3d12_footprints(x->desc, mip, 1, 0, (uptr)fp, 0, 0, 0);
  u64 off; memcpy(&off, fp, 8); RET(off);
}
// (base address, plane, subresource, rows, row pitch, slice pitch): `rows` is the source (into) or destination (from)
static void xg_copy(Cpu *c, int into) {
  XgComputer *x = (XgComputer *)GP(ARG(0)); u64 base = ARG(1); u32 sub = (u32)ARG(3); u64 other = ARG(4); u32 pitch = (u32)ARG(5), slice = (u32)ARG(6);
  if (sub >= subresources(x->desc)) { RET(E_INVALIDARG); return; }
  u8 fp[32]; u32 rows; u64 row;
  // the subresource's place in the whole layout: footprints from 0 up to it
  u64 total = (u64)subresources(x->desc); u8 *all = malloc(32 * total); u32 *nrows = malloc(4 * total); u64 *sizes = malloc(8 * total);
  d3d12_footprints(x->desc, 0, (u32)total, 0, (uptr)all, (uptr)nrows, (uptr)sizes, 0);
  memcpy(fp, all + 32 * sub, 32); rows = nrows[sub]; row = sizes[sub]; free(all); free(nrows); free(sizes);
  u64 off; memcpy(&off, fp, 8); u32 depth, rp; memcpy(&depth, fp + 20, 4); memcpy(&rp, fp + 24, 4);
  for (u32 z = 0; z < depth; z++) for (u32 y = 0; y < rows; y++) {
    u8 *t = (u8 *)GP(base + off + (u64)rp * (rows * z + y)), *o = (u8 *)GP(other + (u64)slice * z + (u64)pitch * y);
    if (into) memcpy(t, o, (size_t)row); else memcpy(o, t, (size_t)row);
  }
  RET(S_OK);
}
IMP(xg_xs, Computer_CopyIntoSubresource) { xg_copy(c, 1); }
IMP(xg_xs, Computer_CopyFromSubresource) { xg_copy(c, 0); }
