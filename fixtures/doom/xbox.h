// The console interfaces our Doom uses, written for this project (no Microsoft header is used or copied):
// D3D12.X, GameInput, XAudio2 and XGameRuntime as C declarations with explicit vtable slots. Structures follow the
// public D3D12 / XAudio2 layouts; the console's vtable order follows XCL's observations of retail titles
// (github.com/ivall/xcl, GPL-2.0): command list methods from SetDescriptorHeaps on sit 4 slots after the PC's,
// the frame clock is device #78-#80, PresentX is queue #30. Methods are called through VCALL.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int32_t HRESULT;
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
#define VCALL(obj, slot, type, ...) (((type)((*(void ***)(obj))[slot]))((obj), ##__VA_ARGS__))
#define FAILED(hr) ((HRESULT)(hr) < 0)

// ---- D3D12.X ----
static const GUID IID_ID3D12Device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID IID_ID3D12CommandQueue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
static const GUID IID_ID3D12CommandAllocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
static const GUID IID_ID3D12GraphicsCommandList = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
static const GUID IID_ID3D12Fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
static const GUID IID_ID3D12Resource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
static const GUID IID_ID3D12DescriptorHeap = {0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}};
static const GUID IID_ID3D12RootSignature = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
static const GUID IID_ID3D12PipelineState = {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};

typedef struct { uint32_t Version, ProcessDebugFlags, GraphicsCommandQueueRingSizeBytes, GraphicsScratchMemorySizeBytes, ComputeScratchMemorySizeBytes, DisableGeometryShaderAllocations, DisableTessellationShaderAllocations, pad; } D3D12XBOX_CREATE_DEVICE_PARAMETERS;
typedef struct { uint32_t Type; int32_t Priority; uint32_t Flags, NodeMask; } D3D12_COMMAND_QUEUE_DESC;
typedef struct { uint32_t Type, CPUPageProperty, MemoryPoolPreference, CreationNodeMask, VisibleNodeMask; } D3D12_HEAP_PROPERTIES;
typedef struct { uint32_t Dimension; uint64_t Alignment, Width; uint32_t Height; uint16_t DepthOrArraySize, MipLevels; uint32_t Format, SampleCount, SampleQuality, Layout, Flags; } D3D12_RESOURCE_DESC;
typedef struct { uint32_t Type, NumDescriptors, Flags, NodeMask; } D3D12_DESCRIPTOR_HEAP_DESC;
typedef struct { uint64_t ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct { uint64_t ptr; } D3D12_GPU_DESCRIPTOR_HANDLE;
typedef struct { const void *pShaderBytecode; size_t BytecodeLength; } D3D12_SHADER_BYTECODE;
typedef struct { uint32_t BlendEnable, LogicOpEnable, SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, LogicOp; uint8_t RenderTargetWriteMask, pad[3]; } D3D12_RENDER_TARGET_BLEND_DESC;
typedef struct { uint32_t StencilFailOp, StencilDepthFailOp, StencilPassOp, StencilFunc; } D3D12_DEPTH_STENCILOP_DESC;
typedef struct {
  void *pRootSignature;
  D3D12_SHADER_BYTECODE VS, PS, DS, HS, GS;
  struct { const void *pSODeclaration; uint32_t NumEntries; const uint32_t *pBufferStrides; uint32_t NumStrides, RasterizedStream; } StreamOutput;
  struct { uint32_t AlphaToCoverageEnable, IndependentBlendEnable; D3D12_RENDER_TARGET_BLEND_DESC RenderTarget[8]; } BlendState;
  uint32_t SampleMask;
  struct { uint32_t FillMode, CullMode, FrontCounterClockwise; int32_t DepthBias; float DepthBiasClamp, SlopeScaledDepthBias; uint32_t DepthClipEnable, MultisampleEnable, AntialiasedLineEnable, ForcedSampleCount, ConservativeRaster; } RasterizerState;
  struct { uint32_t DepthEnable, DepthWriteMask, DepthFunc, StencilEnable; uint8_t StencilReadMask, StencilWriteMask; D3D12_DEPTH_STENCILOP_DESC FrontFace, BackFace; } DepthStencilState;
  struct { const void *pInputElementDescs; uint32_t NumElements; } InputLayout;
  uint32_t IBStripCutValue, PrimitiveTopologyType, NumRenderTargets, RTVFormats[8], DSVFormat, SampleCount, SampleQuality, NodeMask;
  D3D12_SHADER_BYTECODE CachedPSO;
  uint32_t Flags;
} D3D12_GRAPHICS_PIPELINE_STATE_DESC;
_Static_assert(sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) == 656, "PSO desc layout");
typedef struct { uint64_t Offset; uint32_t Format, Width, Height, Depth, RowPitch, pad; } D3D12_PLACED_SUBRESOURCE_FOOTPRINT;
typedef struct { void *pResource; uint32_t Type, pad; union { D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint; uint32_t SubresourceIndex; }; } D3D12_TEXTURE_COPY_LOCATION;
typedef struct { uint32_t Type, Flags; void *pResource; uint32_t Subresource, StateBefore, StateAfter, pad; } D3D12_RESOURCE_BARRIER;
typedef struct { float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; } D3D12_VIEWPORT;
typedef struct { int32_t left, top, right, bottom; } D3D12_RECT;
typedef struct { uint32_t Format, ViewDimension, Shader4ComponentMapping, pad; uint32_t MostDetailedMip, MipLevels, PlaneSlice; float ResourceMinLODClamp; uint32_t pad2[2]; } D3D12_SHADER_RESOURCE_VIEW_DESC;
// a PresentX plane: the frame token, then the resources shown
typedef struct { uint64_t Token; uint32_t ResourceCount, pad; void *const *ppResources; const D3D12_RECT *pSrcViewRects, *pDestPlacementBase; uint32_t ColorSpace, Flags; uint64_t reserved; } D3D12XBOX_PRESENT_PLANE_PARAMETERS;
typedef struct { uint32_t Flags, pad; uint64_t reserved[4]; } D3D12XBOX_PRESENT_PARAMETERS;

enum { // ID3D12Device (console)
  DEV_CreateCommandQueue = 8, DEV_CreateCommandAllocator = 9, DEV_CreateGraphicsPipelineState = 10, DEV_CreateCommandList = 12,
  DEV_CreateDescriptorHeap = 14, DEV_GetDescriptorHandleIncrementSize = 15, DEV_CreateRootSignature = 16, DEV_CreateShaderResourceView = 18,
  DEV_CreateRenderTargetView = 20, DEV_CreateCommittedResource = 27, DEV_CreateFence = 36, DEV_GetCopyableFootprints = 38,
  DEV_SetFrameIntervalX = 78, DEV_ScheduleFrameEventX = 79, DEV_WaitFrameEventX = 80,
};
enum { Q_ExecuteCommandLists = 10, Q_Signal = 14, Q_PresentX = 30 };
enum { // ID3D12GraphicsCommandList (console order)
  CL_Close = 9, CL_Reset = 10, CL_DrawInstanced = 12, CL_CopyTextureRegion = 16, CL_IASetPrimitiveTopology = 20, CL_RSSetViewports = 21,
  CL_RSSetScissorRects = 22, CL_SetPipelineState = 25, CL_ResourceBarrier = 26, CL_SetDescriptorHeaps = 32, CL_SetGraphicsRootSignature = 34,
  CL_SetGraphicsRootDescriptorTable = 36, CL_OMSetRenderTargets = 50, CL_ClearRenderTargetView = 52,
};
enum { ALLOC_Reset = 8, FENCE_GetCompletedValue = 8, FENCE_SetEventOnCompletion = 9, RES_Map = 8, HEAP_GetCPUStart = 9, HEAP_GetGPUStart = 10 };
__declspec(dllimport) HRESULT D3D12XboxCreateDevice(void *adapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS *params, const GUID *riid, void **device);

// ---- GameInput ----
enum { GI_GetCurrentReading = 4, GIR_Release = 2, GIR_GetKeyState = 15, GIR_GetGamepadState = 22 };
#define GameInputKindKeyboard 0x10
#define GameInputKindGamepad 0x40000
typedef struct { uint32_t scanCode, codePoint; uint8_t virtualKey; uint8_t isDeadKey; uint8_t pad[2]; } GameInputKeyState;
typedef struct { uint32_t buttons; float leftTrigger, rightTrigger, leftThumbstickX, leftThumbstickY, rightThumbstickX, rightThumbstickY; } GameInputGamepadState;
enum { GP_Menu = 1, GP_View = 2, GP_A = 4, GP_B = 8, GP_X = 0x10, GP_Y = 0x20, GP_Up = 0x40, GP_Down = 0x80, GP_Left = 0x100, GP_Right = 0x200, GP_LB = 0x400, GP_RB = 0x800 };
__declspec(dllimport) HRESULT GameInputCreate(void **gameInput);

// ---- XAudio2 ----
enum { XA_CreateSourceVoice = 5, XA_CreateMasteringVoice = 7 };
enum { V_SetVolume = 12, V_SetOutputMatrix = 16, V_DestroyVoice = 18, SV_Start = 19, SV_Stop = 20, SV_SubmitSourceBuffer = 21, SV_FlushSourceBuffers = 22, SV_GetState = 25, SV_SetFrequencyRatio = 26 };
typedef struct { uint16_t wFormatTag, nChannels; uint32_t nSamplesPerSec, nAvgBytesPerSec; uint16_t nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct { uint32_t Flags, AudioBytes; const uint8_t *pAudioData; uint32_t PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount; void *pContext; } XAUDIO2_BUFFER;
typedef struct { void *pCurrentBufferContext; uint32_t BuffersQueued; uint64_t SamplesPlayed; } XAUDIO2_VOICE_STATE;
__declspec(dllimport) HRESULT XAudio2Create(void **xaudio2, uint32_t flags, uint32_t processor);

// ---- XGameRuntime ----
typedef struct { void *queue, *context, *callback; uint8_t internal[32]; } XAsyncBlock;
__declspec(dllimport) HRESULT XGameRuntimeInitialize(void);
__declspec(dllimport) HRESULT XAsyncGetStatus(XAsyncBlock *block, uint8_t wait);
__declspec(dllimport) HRESULT XUserAddAsync(uint32_t options, XAsyncBlock *block);
__declspec(dllimport) HRESULT XUserAddResult(XAsyncBlock *block, void **user);
__declspec(dllimport) HRESULT XGameSaveFilesGetFolderWithUiAsync(void *user, const char *scid, XAsyncBlock *block);
__declspec(dllimport) HRESULT XGameSaveFilesGetFolderWithUiResult(XAsyncBlock *block, size_t size, char *folder);
