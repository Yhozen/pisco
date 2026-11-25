// SPDX-License-Identifier: GPL-2.0-or-later
// d3d9caps: prints the D3DCAPS9 of the default adapter's HAL device, one field a line, so our Direct3D 9 can
// answer GetDeviceCaps as the oracle does. Build and run (32-bit, as the titles are):
//   $MINGW/bin/i686-w64-mingw32-clang tools/oracle-wine/d3d9caps.c -o out/d3d9caps.exe -ld3d9
//   tools/oracle-wine/oracle.sh start <title> 'Z:\...\out\d3d9caps.exe'      (its output is in the run folder's wine.log)
#include <d3d9.h>
#include <stdio.h>
#define X(f) printf("%-36s 0x%08lx\n", #f, (unsigned long)c.f)
#define F(f) printf("%-36s %g\n", #f, (double)c.f)
int main(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION); D3DCAPS9 c; D3DADAPTER_IDENTIFIER9 id; D3DDISPLAYMODE m;
  if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
  if (IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id) == D3D_OK) printf("# adapter: %s (%s)\n", id.Description, id.Driver);
  if (IDirect3D9_GetAdapterDisplayMode(d3d, 0, &m) == D3D_OK) printf("# display mode: %ux%u format %u\n", m.Width, m.Height, (unsigned)m.Format);
  HRESULT hr = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &c);
  if (hr != D3D_OK) { printf("GetDeviceCaps failed: 0x%08lx\n", (unsigned long)hr); return 1; }
  X(DeviceType); X(AdapterOrdinal); X(Caps); X(Caps2); X(Caps3); X(PresentationIntervals); X(CursorCaps); X(DevCaps);
  X(PrimitiveMiscCaps); X(RasterCaps); X(ZCmpCaps); X(SrcBlendCaps); X(DestBlendCaps); X(AlphaCmpCaps); X(ShadeCaps);
  X(TextureCaps); X(TextureFilterCaps); X(CubeTextureFilterCaps); X(VolumeTextureFilterCaps); X(TextureAddressCaps);
  X(VolumeTextureAddressCaps); X(LineCaps); X(MaxTextureWidth); X(MaxTextureHeight); X(MaxVolumeExtent); X(MaxTextureRepeat);
  X(MaxTextureAspectRatio); X(MaxAnisotropy); F(MaxVertexW); F(GuardBandLeft); F(GuardBandTop); F(GuardBandRight);
  F(GuardBandBottom); F(ExtentsAdjust); X(StencilCaps); X(FVFCaps); X(TextureOpCaps); X(MaxTextureBlendStages);
  X(MaxSimultaneousTextures); X(VertexProcessingCaps); X(MaxActiveLights); X(MaxUserClipPlanes); X(MaxVertexBlendMatrices);
  X(MaxVertexBlendMatrixIndex); F(MaxPointSize); X(MaxPrimitiveCount); X(MaxVertexIndex); X(MaxStreams); X(MaxStreamStride);
  X(VertexShaderVersion); X(MaxVertexShaderConst); X(PixelShaderVersion); F(PixelShader1xMaxValue); X(DevCaps2);
  F(MaxNpatchTessellationLevel); X(Reserved5); X(MasterAdapterOrdinal); X(AdapterOrdinalInGroup); X(NumberOfAdaptersInGroup); X(DeclTypes);
  X(NumSimultaneousRTs); X(StretchRectFilterCaps); X(VS20Caps.Caps); X(VS20Caps.DynamicFlowControlDepth); X(VS20Caps.NumTemps);
  X(VS20Caps.StaticFlowControlDepth); X(PS20Caps.Caps); X(PS20Caps.DynamicFlowControlDepth); X(PS20Caps.NumTemps);
  X(PS20Caps.StaticFlowControlDepth); X(PS20Caps.NumInstructionSlots); X(VertexTextureFilterCaps);
  X(MaxVShaderInstructionsExecuted); X(MaxPShaderInstructionsExecuted); X(MaxVertexShader30InstructionSlots);
  X(MaxPixelShader30InstructionSlots);
  IDirect3D9_Release(d3d); return 0;
}
