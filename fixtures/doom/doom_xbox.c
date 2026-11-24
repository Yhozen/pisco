// Doom (doomgeneric, GPL-2.0) as a console title: the platform layer of doomgeneric written against the console's
// interfaces — D3D12.X (D3D12XboxCreateDevice, display back buffers, the frame clock, PresentX), GameInput
// (keyboard and gamepad), XAudio2 (sound effects) and XGameRuntime (a user, the save folder).
// The structure (device, queue, a texture the frame is copied into, a fullscreen pass, PresentX) follows
// OpenScarlett's tests/scarlett_game/scarlett_platform.cpp (MIT, github.com/californiaAlmonds/OpenScarlett);
// no code is taken from it.
//
// -hashframes: time is virtual (it advances only by DG_SleepMs), so -timedemo runs the same frames on any
// machine, and every frame's FNV-1a hash (B, G, R of each pixel) is printed; tools/doom-native.sh builds the
// same doomgeneric for Linux with the same rule to compare against.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "vs_blit.h"
#include "ps_blit.h"

__declspec(dllimport) void __stdcall Sleep(uint32_t ms);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(int64_t *v);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(int64_t *v);
__declspec(dllimport) void *__stdcall CreateEventW(void *attrs, int manual, int initial, const uint16_t *name);
__declspec(dllimport) uint32_t __stdcall WaitForSingleObject(void *h, uint32_t ms);

#define W DOOMGENERIC_RESX
#define H DOOMGENERIC_RESY
#define FMT_BGRA8 87
static int hash_frames;
static uint64_t virtual_ms;

static void check(HRESULT hr, const char *what) { if (FAILED(hr)) { printf("doom: %s failed (0x%08x)\n", what, (unsigned)hr); exit(1); } }

// ---- D3D12.X ----
static void *dev, *queue, *alloc[2], *list, *fence, *fence_event, *rtv_heap, *srv_heap, *root_sig, *pso, *backbuf[2], *fbtex, *upload[2];
static uint64_t rtv_start, rtv_step, srv_gpu, fence_value, frame_fence[2];
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
static int frame;

static void *committed(uint32_t heap_type, uint32_t heap_flags, const D3D12_RESOURCE_DESC *d, uint32_t state) {
  D3D12_HEAP_PROPERTIES hp = {heap_type, 0, 0, 1, 1}; void *r = NULL;
  check(VCALL(dev, DEV_CreateCommittedResource, HRESULT (*)(void *, const D3D12_HEAP_PROPERTIES *, uint32_t, const D3D12_RESOURCE_DESC *, uint32_t, const void *, const GUID *, void **), &hp, heap_flags, d, state, NULL, &IID_ID3D12Resource, &r), "CreateCommittedResource");
  return r;
}
static void gfx_init(void) {
  D3D12XBOX_CREATE_DEVICE_PARAMETERS p = {0}; p.Version = 0x20004; p.GraphicsCommandQueueRingSizeBytes = 1 << 20;
  check(D3D12XboxCreateDevice(NULL, &p, &IID_ID3D12Device, &dev), "D3D12XboxCreateDevice");
  check(VCALL(dev, DEV_SetFrameIntervalX, HRESULT (*)(void *, void *, uint32_t, uint32_t, uint32_t), NULL, 16667, 2, 0), "SetFrameIntervalX");
  check(VCALL(dev, DEV_ScheduleFrameEventX, HRESULT (*)(void *, uint32_t, uint32_t, void *, uint32_t), 0, 0, NULL, 0), "ScheduleFrameEventX");
  D3D12_COMMAND_QUEUE_DESC qd = {0, 0, 0, 0};
  check(VCALL(dev, DEV_CreateCommandQueue, HRESULT (*)(void *, const D3D12_COMMAND_QUEUE_DESC *, const GUID *, void **), &qd, &IID_ID3D12CommandQueue, &queue), "CreateCommandQueue");
  for (int i = 0; i < 2; i++) check(VCALL(dev, DEV_CreateCommandAllocator, HRESULT (*)(void *, uint32_t, const GUID *, void **), 0, &IID_ID3D12CommandAllocator, &alloc[i]), "CreateCommandAllocator");
  check(VCALL(dev, DEV_CreateCommandList, HRESULT (*)(void *, uint32_t, uint32_t, void *, void *, const GUID *, void **), 0, 0, alloc[0], NULL, &IID_ID3D12GraphicsCommandList, &list), "CreateCommandList");
  VCALL(list, CL_Close, HRESULT (*)(void *));
  check(VCALL(dev, DEV_CreateFence, HRESULT (*)(void *, uint64_t, uint32_t, const GUID *, void **), 0, 0, &IID_ID3D12Fence, &fence), "CreateFence");
  fence_event = CreateEventW(NULL, 0, 0, NULL);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {2, 2, 0, 0}; // RTV
  check(VCALL(dev, DEV_CreateDescriptorHeap, HRESULT (*)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, const GUID *, void **), &hd, &IID_ID3D12DescriptorHeap, &rtv_heap), "CreateDescriptorHeap");
  D3D12_DESCRIPTOR_HEAP_DESC sd = {0, 1, 1, 0}; // CBV/SRV/UAV, shader visible
  check(VCALL(dev, DEV_CreateDescriptorHeap, HRESULT (*)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, const GUID *, void **), &sd, &IID_ID3D12DescriptorHeap, &srv_heap), "CreateDescriptorHeap");
  rtv_start = VCALL(rtv_heap, HEAP_GetCPUStart, uint64_t (*)(void *)); // the console returns handles in RAX
  rtv_step = VCALL(dev, DEV_GetDescriptorHandleIncrementSize, uint32_t (*)(void *, uint32_t), 2);
  srv_gpu = VCALL(srv_heap, HEAP_GetGPUStart, uint64_t (*)(void *));
  uint64_t srv_cpu = VCALL(srv_heap, HEAP_GetCPUStart, uint64_t (*)(void *));
  D3D12_RESOURCE_DESC bb = {3, 0, W, H, 1, 1, FMT_BGRA8, 1, 0, 0, 1 /* ALLOW_RENDER_TARGET */};
  for (int i = 0; i < 2; i++) {
    backbuf[i] = committed(1, 0x8 /* ALLOW_DISPLAY */, &bb, 0);
    VCALL(dev, DEV_CreateRenderTargetView, void (*)(void *, void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE), backbuf[i], NULL, (D3D12_CPU_DESCRIPTOR_HANDLE){rtv_start + i * rtv_step});
  }
  D3D12_RESOURCE_DESC td = {3, 0, W, H, 1, 1, FMT_BGRA8, 1, 0, 0, 0};
  fbtex = committed(1, 0, &td, 0x400 /* COPY_DEST */);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {FMT_BGRA8, 4 /* TEXTURE2D */, 0x1688 /* default mapping */, 0, 0, 1, 0, 0, {0, 0}};
  VCALL(dev, DEV_CreateShaderResourceView, void (*)(void *, void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE), fbtex, &sv, (D3D12_CPU_DESCRIPTOR_HANDLE){srv_cpu});
  uint32_t rows; uint64_t row_size, total;
  VCALL(dev, DEV_GetCopyableFootprints, void (*)(void *, const D3D12_RESOURCE_DESC *, uint32_t, uint32_t, uint64_t, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *, uint32_t *, uint64_t *, uint64_t *), &td, 0, 1, 0, &footprint, &rows, &row_size, &total);
  D3D12_RESOURCE_DESC ud = {1, 0, total, 1, 1, 1, 0, 1, 0, 1 /* ROW_MAJOR */, 0};
  for (int i = 0; i < 2; i++) upload[i] = committed(2 /* UPLOAD */, 0, &ud, 0xAC3 /* GENERIC_READ */);
  // the root signature is the one the shaders carry ([RootSignature] in blit.hlsl)
  check(VCALL(dev, DEV_CreateRootSignature, HRESULT (*)(void *, uint32_t, const void *, size_t, const GUID *, void **), 0, g_vs_main, sizeof g_vs_main, &IID_ID3D12RootSignature, &root_sig), "CreateRootSignature");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
  pd.pRootSignature = root_sig;
  pd.VS = (D3D12_SHADER_BYTECODE){g_vs_main, sizeof g_vs_main}; pd.PS = (D3D12_SHADER_BYTECODE){g_ps_main, sizeof g_ps_main};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
  pd.SampleMask = 0xFFFFFFFF;
  pd.RasterizerState.FillMode = 3; pd.RasterizerState.CullMode = 1; pd.RasterizerState.DepthClipEnable = 1;
  pd.PrimitiveTopologyType = 3; pd.NumRenderTargets = 1; pd.RTVFormats[0] = FMT_BGRA8; pd.SampleCount = 1;
  check(VCALL(dev, DEV_CreateGraphicsPipelineState, HRESULT (*)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, const GUID *, void **), &pd, &IID_ID3D12PipelineState, &pso), "CreateGraphicsPipelineState");
}
static void wait_fence(uint64_t v) {
  if (VCALL(fence, FENCE_GetCompletedValue, uint64_t (*)(void *)) >= v) return;
  VCALL(fence, FENCE_SetEventOnCompletion, HRESULT (*)(void *, uint64_t, void *), v, fence_event);
  WaitForSingleObject(fence_event, 0xFFFFFFFF);
}
static void barrier(void *res, uint32_t before, uint32_t after) {
  D3D12_RESOURCE_BARRIER b = {0, 0, res, 0xFFFFFFFF, before, after, 0};
  VCALL(list, CL_ResourceBarrier, void (*)(void *, uint32_t, const D3D12_RESOURCE_BARRIER *), 1, &b);
}
static uint32_t frame_hash(const uint32_t *px) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < W * H; i++) { uint32_t p = px[i]; h = (h ^ (p & 0xFF)) * 16777619u; h = (h ^ (p >> 8 & 0xFF)) * 16777619u; h = (h ^ (p >> 16 & 0xFF)) * 16777619u; }
  return h;
}
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
void DG_DrawFrame(void) {
  int f = frame;
  if (hash_frames) printf("frame %u %08x\n", frames_drawn, frame_hash(DG_ScreenBuffer));
  dump(frames_drawn, DG_ScreenBuffer);
  frames_drawn++;
  wait_fence(frame_fence[f]);
  uint64_t token = 0;
  check(VCALL(dev, DEV_WaitFrameEventX, HRESULT (*)(void *, uint32_t, uint32_t, void *, uint32_t, uint64_t *), 0, 1000, NULL, 0, &token), "WaitFrameEventX");
  uint8_t *map = NULL;
  check(VCALL(upload[f], RES_Map, HRESULT (*)(void *, uint32_t, const void *, void **), 0, NULL, (void **)&map), "Map");
  for (int y = 0; y < H; y++) memcpy(map + footprint.Offset + (size_t)y * footprint.RowPitch, DG_ScreenBuffer + (size_t)y * W, W * 4);
  VCALL(alloc[f], ALLOC_Reset, HRESULT (*)(void *));
  VCALL(list, CL_Reset, HRESULT (*)(void *, void *, void *), alloc[f], pso);
  barrier(fbtex, 0x80 /* PIXEL_SHADER_RESOURCE */, 0x400 /* COPY_DEST */);
  D3D12_TEXTURE_COPY_LOCATION dst = {fbtex, 0, 0, {.SubresourceIndex = 0}}, src = {upload[f], 1, 0, {.PlacedFootprint = footprint}};
  VCALL(list, CL_CopyTextureRegion, void (*)(void *, const D3D12_TEXTURE_COPY_LOCATION *, uint32_t, uint32_t, uint32_t, const D3D12_TEXTURE_COPY_LOCATION *, const void *), &dst, 0, 0, 0, &src, NULL);
  barrier(fbtex, 0x400, 0x80);
  barrier(backbuf[f], 0, 4 /* RENDER_TARGET */);
  VCALL(list, CL_SetGraphicsRootSignature, void (*)(void *, void *), root_sig);
  VCALL(list, CL_SetDescriptorHeaps, void (*)(void *, uint32_t, void *const *), 1, &srv_heap);
  VCALL(list, CL_SetGraphicsRootDescriptorTable, void (*)(void *, uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE), 0, (D3D12_GPU_DESCRIPTOR_HANDLE){srv_gpu});
  D3D12_VIEWPORT vp = {0, 0, W, H, 0, 1}; D3D12_RECT sc = {0, 0, W, H};
  VCALL(list, CL_RSSetViewports, void (*)(void *, uint32_t, const D3D12_VIEWPORT *), 1, &vp);
  VCALL(list, CL_RSSetScissorRects, void (*)(void *, uint32_t, const D3D12_RECT *), 1, &sc);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {rtv_start + f * rtv_step};
  VCALL(list, CL_OMSetRenderTargets, void (*)(void *, uint32_t, const D3D12_CPU_DESCRIPTOR_HANDLE *, int, const void *), 1, &rtv, 0, NULL);
  static const float black[4] = {0, 0, 0, 1};
  VCALL(list, CL_ClearRenderTargetView, void (*)(void *, D3D12_CPU_DESCRIPTOR_HANDLE, const float *, uint32_t, const void *), rtv, black, 0, NULL);
  VCALL(list, CL_IASetPrimitiveTopology, void (*)(void *, uint32_t), 4 /* TRIANGLELIST */);
  VCALL(list, CL_DrawInstanced, void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t), 3, 1, 0, 0);
  barrier(backbuf[f], 4, 0);
  check(VCALL(list, CL_Close, HRESULT (*)(void *)), "Close");
  VCALL(queue, Q_ExecuteCommandLists, void (*)(void *, uint32_t, void *const *), 1, &list);
  D3D12XBOX_PRESENT_PLANE_PARAMETERS plane; memset(&plane, 0, sizeof plane);
  plane.Token = token; plane.ResourceCount = 1; plane.ppResources = &backbuf[f];
  D3D12XBOX_PRESENT_PARAMETERS pp; memset(&pp, 0, sizeof pp);
  check(VCALL(queue, Q_PresentX, HRESULT (*)(void *, uint32_t, const D3D12XBOX_PRESENT_PLANE_PARAMETERS *, const D3D12XBOX_PRESENT_PARAMETERS *), 1, &plane, &pp), "PresentX");
  check(VCALL(queue, Q_Signal, HRESULT (*)(void *, void *, uint64_t), fence, ++fence_value), "Signal");
  frame_fence[f] = fence_value;
  frame ^= 1;
}

// ---- time ----
static int64_t qpf, qpc0;
uint32_t DG_GetTicksMs(void) {
  if (hash_frames) return (uint32_t)virtual_ms;
  int64_t t; QueryPerformanceCounter(&t);
  return (uint32_t)((t - qpc0) * 1000 / qpf);
}
void DG_SleepMs(uint32_t ms) { if (hash_frames) virtual_ms += ms; else Sleep(ms); }
void DG_SetWindowTitle(const char *title) { (void)title; }
void DG_Init(void) {}

// ---- input: GameInput's keyboard and gamepad, turned into Doom key events ----
static void *gameinput;
static unsigned char events[256][2]; static unsigned ev_head, ev_tail;
static void push_key(int pressed, unsigned char key) { if (ev_head - ev_tail < 256) { events[ev_head % 256][0] = (unsigned char)pressed; events[ev_head % 256][1] = key; ev_head++; } }
static unsigned char doom_key(uint8_t vk) {
  switch (vk) {
  case 0x25: return KEY_LEFTARROW; case 0x26: return KEY_UPARROW; case 0x27: return KEY_RIGHTARROW; case 0x28: return KEY_DOWNARROW;
  case 0x0D: return KEY_ENTER; case 0x1B: return KEY_ESCAPE; case 0x09: return KEY_TAB; case 0x20: return KEY_USE; case 0x08: return KEY_BACKSPACE;
  case 0x11: case 0xA2: case 0xA3: return KEY_FIRE; case 0x10: case 0xA0: case 0xA1: return KEY_RSHIFT; case 0x12: case 0xA4: case 0xA5: return KEY_RALT;
  case 0xBC: return KEY_STRAFE_L; case 0xBE: return KEY_STRAFE_R; case 0xBB: return KEY_EQUALS; case 0xBD: return KEY_MINUS;
  default:
    if (vk >= 0x70 && vk <= 0x79) return (unsigned char)(KEY_F1 + (vk - 0x70));
    if (vk == 0x7A) return KEY_F11;
    if (vk == 0x7B) return KEY_F12;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return (unsigned char)(vk >= 'A' ? vk + 32 : vk);
    return 0;
  }
}
static unsigned char held[256], held_pad[16];
static void poll_input(void) {
  if (!gameinput) return;
  unsigned char now[256] = {0};
  void *r = NULL;
  if (VCALL(gameinput, GI_GetCurrentReading, HRESULT (*)(void *, uint32_t, void *, void **), GameInputKindKeyboard, NULL, &r) == 0 && r) {
    GameInputKeyState keys[16];
    uint32_t n = VCALL(r, GIR_GetKeyState, uint32_t (*)(void *, uint32_t, GameInputKeyState *), 16, keys);
    for (uint32_t i = 0; i < n && i < 16; i++) { unsigned char k = doom_key(keys[i].virtualKey); if (k) now[k] = 1; }
    VCALL(r, GIR_Release, uint32_t (*)(void *));
  }
  r = NULL;
  if (VCALL(gameinput, GI_GetCurrentReading, HRESULT (*)(void *, uint32_t, void *, void **), GameInputKindGamepad, NULL, &r) == 0 && r) {
    GameInputGamepadState g; memset(&g, 0, sizeof g);
    if (VCALL(r, GIR_GetGamepadState, int (*)(void *, GameInputGamepadState *), &g)) {
      uint32_t b = g.buttons;
      if (b & GP_Up || g.leftThumbstickY > 0.4f) now[KEY_UPARROW] = 1;
      if (b & GP_Down || g.leftThumbstickY < -0.4f) now[KEY_DOWNARROW] = 1;
      if (b & GP_Left || g.rightThumbstickX < -0.4f) now[KEY_LEFTARROW] = 1;
      if (b & GP_Right || g.rightThumbstickX > 0.4f) now[KEY_RIGHTARROW] = 1;
      if (g.leftThumbstickX < -0.4f || b & GP_LB) now[KEY_STRAFE_L] = 1;
      if (g.leftThumbstickX > 0.4f || b & GP_RB) now[KEY_STRAFE_R] = 1;
      if (b & GP_A) { now[KEY_USE] = 1; now[KEY_ENTER] = 1; }
      if (b & GP_B || g.rightTrigger > 0.3f) now[KEY_FIRE] = 1;
      if (b & GP_X) now[KEY_RSHIFT] = 1;
      if (b & GP_Y) now['y'] = 1;
      if (b & GP_Menu) now[KEY_ESCAPE] = 1;
      if (b & GP_View) now[KEY_TAB] = 1;
    }
    VCALL(r, GIR_Release, uint32_t (*)(void *));
  }
  for (int k = 1; k < 256; k++) if (now[k] != held[k]) { push_key(now[k], (unsigned char)k); held[k] = now[k]; }
  (void)held_pad;
}
int DG_GetKey(int *pressed, unsigned char *key) {
  if (ev_head == ev_tail) poll_input();
  if (ev_head == ev_tail) return 0;
  *pressed = events[ev_tail % 256][0]; *key = events[ev_tail % 256][1]; ev_tail++;
  return 1;
}

// ---- sound effects: one XAudio2 source voice per Doom channel, the lumps' 8-bit PCM as they are ----
static void *xaudio, *master, *voices[16];
static uint32_t voice_rate[16];
static snddevice_t sound_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};
static boolean snd_prefix;
static boolean Snd_Init(boolean use_prefix) {
  snd_prefix = use_prefix;
  if (FAILED(XAudio2Create(&xaudio, 0, 1)) || !xaudio) return false;
  if (FAILED(VCALL(xaudio, XA_CreateMasteringVoice, HRESULT (*)(void *, void **, uint32_t, uint32_t, uint32_t, const void *, const void *, uint32_t), &master, 2, 48000, 0, NULL, NULL, 6))) return false;
  WAVEFORMATEX wf = {1, 1, 11025, 11025, 1, 8, 0};
  for (int i = 0; i < 16; i++) {
    if (FAILED(VCALL(xaudio, XA_CreateSourceVoice, HRESULT (*)(void *, void **, const WAVEFORMATEX *, uint32_t, float, void *, const void *, const void *), &voices[i], &wf, 0, 4.0f, NULL, NULL, NULL))) return false;
    voice_rate[i] = 11025;
  }
  printf("doom: XAudio2 sound effects\n");
  return true;
}
static void Snd_Shutdown(void) {}
static int Snd_GetSfxLumpNum(sfxinfo_t *s) { char n[16]; if (snd_prefix) snprintf(n, sizeof n, "ds%s", s->name); else snprintf(n, sizeof n, "%s", s->name); return W_GetNumForName(n); }
static void Snd_Update(void) {}
static void set_params(int ch, int vol, int sep) {
  float v = vol / 127.0f, right = sep / 254.0f, levels[2] = {v * (1 - right) * 1.4f, v * right * 1.4f};
  VCALL(voices[ch], V_SetOutputMatrix, HRESULT (*)(void *, void *, uint32_t, uint32_t, const float *, uint32_t), master, 1, 2, levels, 0);
}
static void Snd_UpdateSoundParams(int ch, int vol, int sep) { if (ch >= 0 && ch < 16 && voices[ch]) set_params(ch, vol, sep); }
static int Snd_StartSound(sfxinfo_t *s, int ch, int vol, int sep) {
  if (ch < 0 || ch >= 16 || !voices[ch]) return -1;
  int lump = s->lumpnum; if (lump < 0) lump = Snd_GetSfxLumpNum(s);
  const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC); int len = W_LumpLength(lump);
  if (len < 8 || d[0] != 3 || d[1] != 0) return -1;
  uint32_t rate = d[2] | d[3] << 8, n = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
  if (n > (uint32_t)len - 8) n = (uint32_t)len - 8;
  // the DMX format pads 16 samples at each end
  const uint8_t *pcm = d + 8; if (n > 32) { pcm += 16; n -= 32; }
  VCALL(voices[ch], SV_Stop, HRESULT (*)(void *, uint32_t, uint32_t), 0, 0);
  VCALL(voices[ch], SV_FlushSourceBuffers, HRESULT (*)(void *));
  VCALL(voices[ch], SV_SetFrequencyRatio, HRESULT (*)(void *, float, uint32_t), (float)rate / voice_rate[ch], 0);
  set_params(ch, vol, sep);
  XAUDIO2_BUFFER b; memset(&b, 0, sizeof b); b.Flags = 0x40 /* END_OF_STREAM */; b.AudioBytes = n; b.pAudioData = pcm;
  VCALL(voices[ch], SV_SubmitSourceBuffer, HRESULT (*)(void *, const XAUDIO2_BUFFER *, const void *), &b, NULL);
  VCALL(voices[ch], SV_Start, HRESULT (*)(void *, uint32_t, uint32_t), 0, 0);
  return ch;
}
static void Snd_StopSound(int ch) { if (ch >= 0 && ch < 16 && voices[ch]) { VCALL(voices[ch], SV_Stop, HRESULT (*)(void *, uint32_t, uint32_t), 0, 0); VCALL(voices[ch], SV_FlushSourceBuffers, HRESULT (*)(void *)); } }
static boolean Snd_SoundIsPlaying(int ch) {
  if (ch < 0 || ch >= 16 || !voices[ch]) return false;
  XAUDIO2_VOICE_STATE st; memset(&st, 0, sizeof st);
  VCALL(voices[ch], SV_GetState, void (*)(void *, XAUDIO2_VOICE_STATE *, uint32_t), &st, 0);
  return st.BuffersQueued > 0;
}
static void Snd_CacheSounds(sfxinfo_t *s, int n) { (void)s; (void)n; }
sound_module_t DG_sound_module = {sound_devices, sizeof sound_devices / sizeof sound_devices[0], Snd_Init, Snd_Shutdown, Snd_GetSfxLumpNum, Snd_Update, Snd_UpdateSoundParams, Snd_StartSound, Snd_StopSound, Snd_SoundIsPlaying, Snd_CacheSounds};
// no music: the module refuses to start
static boolean Mus_Init(void) { return false; }
static void Mus_Nop(void) {}
static void Mus_SetVolume(int v) { (void)v; }
static void *Mus_Register(void *d, int n) { (void)d; (void)n; return NULL; }
static void Mus_Unregister(void *h) { (void)h; }
static void Mus_Play(void *h, boolean l) { (void)h; (void)l; }
static boolean Mus_Playing(void) { return false; }
music_module_t DG_music_module = {sound_devices, sizeof sound_devices / sizeof sound_devices[0], Mus_Init, Mus_Nop, Mus_SetVolume, Mus_Nop, Mus_Nop, Mus_Register, Mus_Unregister, Mus_Play, Mus_Nop, Mus_Playing, Mus_Nop};

// ---- the title ----
char *dg_save_dir = ".";
int use_libsamplerate = 0;       // settings i_sound.c binds for SDL's module
float libsamplerate_scale = 0.65f;
static void wait_async(XAsyncBlock *b) { while (XAsyncGetStatus(b, 0) == (HRESULT)0x8000000A) Sleep(1); }
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-hashframes")) hash_frames = 1;
  dump_args(argc, argv);
  QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qpc0);
  check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");
  XAsyncBlock ab; memset(&ab, 0, sizeof ab); void *user = NULL;
  if (!FAILED(XUserAddAsync(1 /* AddDefaultUserSilently */, &ab))) { wait_async(&ab); XUserAddResult(&ab, &user); }
  static char folder[260];
  memset(&ab, 0, sizeof ab);
  if (user && !FAILED(XGameSaveFilesGetFolderWithUiAsync(user, "00000000-0000-0000-0000-000000000000", &ab))) {
    wait_async(&ab);
    if (!FAILED(XGameSaveFilesGetFolderWithUiResult(&ab, sizeof folder, folder))) { strcat(folder, "\\"); dg_save_dir = folder; printf("doom: saves in %s\n", folder); } // Doom appends names to it
  }
  if (FAILED(GameInputCreate(&gameinput))) gameinput = NULL;
  gfx_init();
  printf("doom: D3D12.X device ready (%dx%d)\n", W, H);
  // the WAD is in the title's package
  int n = argc; char **args = malloc(sizeof(char *) * (size_t)(argc + 3));
  for (int i = 0; i < argc; i++) args[i] = argv[i];
  int has_iwad = 0; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-iwad")) has_iwad = 1;
  if (!has_iwad) { args[n++] = "-iwad"; args[n++] = "G:\\freedoom1.wad"; }
  args[n] = NULL;
  doomgeneric_Create(n, args);
  for (;;) doomgeneric_Tick();
}
