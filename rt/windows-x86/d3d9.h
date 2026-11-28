// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9: what d3d9.c (the Direct3D object, the device, statistics), d3d9_resources.c (surfaces, textures,
// volumes, buffers, declarations), d3d9_state.c (the device's state, scenes, draws), d3d9_stateblock.c and
// d3d9_gpu.c share. Every object exists, keeps its state and its bytes and answers as Direct3D does (the null
// device of milestone M3); d3d9_gpu.c turns clears, draws and presents into records of the GPU ring, with the
// fixed-function shaders d3d9_ffp.c writes. A 32-bit guest's structures are laid out as these C structures are
// (wasm32), so the device's state is read and written directly.
#pragma once
#include "com.h"
#if GUEST_BITS != 32
#error "rt/windows-x86 is the runtime of a 32-bit guest"
#endif

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_OUTOFMEMORY 0x8007000Eu
#define IID_IUnknown_text "00000000-0000-0000-c000-000000000046"

// Direct3D's own structures (the ones apps pass by address)
typedef struct { float m[16]; } Matrix9;                          // D3DMATRIX, row by row
typedef struct { float r, g, b, a; } Color9;                      // D3DCOLORVALUE
typedef struct { Color9 diffuse, ambient, specular, emissive; float power; } Material9;                       // D3DMATERIAL9
typedef struct { u32 type; Color9 diffuse, specular, ambient; float position[3], direction[3], range, falloff, attenuation[3], theta, phi; } Light9; // D3DLIGHT9
typedef struct { u32 x, y, width, height; float min_z, max_z; } Viewport9;                                   // D3DVIEWPORT9
typedef struct { s32 left, top, right, bottom; } Rect9;                                                      // RECT
typedef struct { u32 left, top, right, bottom, front, back; } Box9;                                          // D3DBOX
typedef struct { u16 stream, offset; u8 type, method, usage, usage_index; } Element9;                         // D3DVERTEXELEMENT9
typedef struct { u32 width, height, format, count, multisample, quality, swap_effect, window, windowed, auto_depth, depth_format, flags, refresh, interval; } Present9; // D3DPRESENT_PARAMETERS
typedef struct { u32 format, type, usage, pool, multisample, quality, width, height; } SurfaceDesc9;        // D3DSURFACE_DESC
typedef struct { u32 format, type, usage, pool, width, height, depth; } VolumeDesc9;                         // D3DVOLUME_DESC
typedef struct { u32 format, type, usage, pool, size, fvf; } VertexBufferDesc9;                              // D3DVERTEXBUFFER_DESC
typedef struct { u32 format, type, usage, pool, size; } IndexBufferDesc9;                                    // D3DINDEXBUFFER_DESC
_Static_assert(sizeof(Light9) == 104 && sizeof(Material9) == 68 && sizeof(Present9) == 56 && sizeof(Element9) == 8, "Direct3D 9 structure sizes");

// D3DRESOURCETYPE, D3DPOOL, D3DUSAGE_*, D3DLOCK_*, D3DFORMAT values this file's users name
enum { RTYPE_SURFACE = 1, RTYPE_VOLUME = 2, RTYPE_TEXTURE = 3, RTYPE_VOLUMETEXTURE = 4, RTYPE_CUBETEXTURE = 5, RTYPE_VERTEXBUFFER = 6, RTYPE_INDEXBUFFER = 7 };
enum { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };
enum { USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2, USAGE_DYNAMIC = 0x200, USAGE_AUTOGENMIPMAP = 0x400 };
enum { LOCK_READONLY = 0x10, LOCK_NOSYSLOCK = 0x800, LOCK_NOOVERWRITE = 0x1000, LOCK_DISCARD = 0x2000, LOCK_DONOTWAIT = 0x4000, LOCK_NO_DIRTY_UPDATE = 0x8000 };
enum { FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101, FMT_INDEX32 = 102 };

// ---- objects ----
// Every object starts the same way. `refs` are the app's references; `held` counts the device's own uses of
// the object (bound to a stage, a stream or a target, or kept by a state block). An object lives while either
// is above zero, and holds a reference on its device while `refs` is, as the oracle's does (d3d9probe.txt).
// A texture's surfaces and a volume texture's volumes have no count of their own: theirs is the texture's.
enum { K_DIRECT3D = 1, K_DEVICE, K_SWAPCHAIN, K_SURFACE, K_TEXTURE, K_VOLUMETEXTURE, K_VOLUME, K_VERTEXBUFFER, K_INDEXBUFFER, K_DECLARATION, K_STATEBLOCK, K_VERTEXSHADER, K_PIXELSHADER, K_CUBETEXTURE };
typedef struct Device9 Device9;
typedef struct Object9 {
  u32 vtable; s32 refs, held; u32 kind;
  Device9 *device;
  struct Object9 *container;   // the texture a surface or a volume is a level of
} Object9;
typedef struct Surface9 {
  Object9 o;
  SurfaceDesc9 desc; u32 level;
  u8 *bits; u32 pitch, size;   // the pixels: `pitch` bytes a row (a row of blocks for DXT formats)
  s32 locked; u32 lock_flags;
  u8 lockable;                 // not a render target inside a texture, not a back buffer or a target made without the option
  u32 dirty;                   // counts unlocks and copies into it: the back end's sign that the bytes changed
  u32 gpu_id, gpu_dirty;       // d3d9_gpu.c: its texture on the GPU (a surface that is no texture's level), and the `dirty` last sent there
  u32 part_id, part_w, part_h; // d3d9_gpu.c: a depth buffer drawn with a smaller target: the target-sized depth texture used then
  u8 part_written;             // ... which was written since the buffer's last clear: the buffer's own corner no longer agrees
} Surface9;
typedef struct Volume9 { Object9 o; VolumeDesc9 desc; u32 level; u8 *bits; u32 row_pitch, slice_pitch, size; s32 locked; u32 dirty, gpu_dirty; } Volume9;
typedef struct Texture9 {      // a 2D texture (K_TEXTURE: `surfaces`), a cube texture (K_CUBETEXTURE: `surfaces`, six faces of
                               // `levels` each, face by face) or a volume texture (K_VOLUMETEXTURE: `volumes`)
  Object9 o;
  u32 format, usage, pool, width, height, depth, levels, lod, autogen_filter, priority;
  Surface9 **surfaces; Volume9 **volumes;
  u32 gpu_id;                  // d3d9_gpu.c: the texture on the GPU, all its levels
} Texture9;
typedef struct Buffer9 {       // a vertex buffer or an index buffer
  Object9 o;
  u32 format, usage, pool, size, fvf, priority;
  u8 *bits; s32 locked; u32 dirty;
  u32 gpu_id, gpu_dirty;       // d3d9_gpu.c: the buffer on the GPU, and the `dirty` last sent there
  u32 gpu_lo, gpu_hi;          //   the bytes locked for writing since then,
  u8 gpu_pending, gpu_mode;    //   whether any lock said which, and how they may be written (GPU_WRITE_*)
  u8 *gpu_copy;                //   what the GPU holds, for a large buffer locked whole (only what differs is sent)
} Buffer9;
typedef struct Declaration9 { Object9 o; u32 count, fvf; Element9 *elements;   // `count` includes the end marker; `fvf`: the code it was made from, 0 for an app's own
                              struct Declaration9 *next_fvf; } Declaration9;
typedef struct SwapChain9 { Object9 o; } SwapChain9;
// a vertex or pixel shader: its bytecode (d3d9_sm.c translates it for a draw), from the version token to the end token
typedef struct Shader9 { Object9 o; u32 *tokens; u32 count; u64 hash; } Shader9;   // hash: of the tokens (d3d9_gpu.c, once)

// ---- the state a state block can hold ----
enum { RS_COUNT = 256, TSS_COUNT = 33, STAGES = 8, SAMP_COUNT = 14, SAMPLERS = 21, STREAMS = 16, CLIP_PLANES = 8,
       VS_FLOATS = 256, PS_FLOATS = 224, SHADER_INTS = 16, SHADER_BOOLS = 16, TRANSFORMS = 512 };
typedef struct { Light9 light; u8 set, enabled;   // `set`: the light exists (SetLight, or LightEnable gave it the default light)
                 u8 has_light, has_enable; } LightSlot9;   // in a state block: which of the two the block covers
typedef struct State9 {
  u32 rs[RS_COUNT];                       // render states, by D3DRENDERSTATETYPE
  u32 tss[STAGES][TSS_COUNT];             // texture stage states, by D3DTEXTURESTAGESTATETYPE
  u32 samp[SAMPLERS][SAMP_COUNT];         // sampler states: 0..15 the pixel samplers, 16 the displacement map, 17..20 the vertex samplers
  Texture9 *textures[SAMPLERS];
  struct { Buffer9 *buffer; u32 offset, stride, freq; } streams[STREAMS];
  Buffer9 *indices;
  Declaration9 *declaration; u32 fvf;     // the vertex format: an app's declaration (fvf 0), or the device's own declaration for an FVF code
  struct Shader9 *vertex_shader, *pixel_shader;   // 0: the fixed-function pipeline
  Material9 material; Viewport9 viewport; Rect9 scissor;
  float clip_planes[CLIP_PLANES][4];
  float npatch;
  Matrix9 transforms[TRANSFORMS];         // by D3DTRANSFORMSTATETYPE: 2 view, 3 projection, 16..23 texture, 256..511 world
  LightSlot9 *lights; u32 light_count;    // by light index
  float vs_float[VS_FLOATS][4], ps_float[PS_FLOATS][4]; s32 vs_int[SHADER_INTS][4], ps_int[SHADER_INTS][4]; u32 vs_bool[SHADER_BOOLS], ps_bool[SHADER_BOOLS];
} State9;
// What a state block covers, as lists (Capture and Apply walk them) with a bit set per entry (so that each is listed once)
enum { M_MATERIAL = 1, M_VIEWPORT = 2, M_SCISSOR = 4, M_INDICES = 8, M_DECLARATION = 0x10, M_VERTEX_SHADER = 0x20, M_PIXEL_SHADER = 0x40, M_NPATCH = 0x80 };
typedef struct StateBlock9 {
  Object9 o;
  u32 type;                               // D3DSTATEBLOCKTYPE, or 0 for one that was recorded
  State9 s;
  u32 single;                             // M_*
  u16 *list; u32 list_count, list_cap;    // entries: a class in the high 4 bits, an index below (see d3d9_stateblock.c)
  u8 seen[16][512];
  u32 *lights; u32 lights_count, lights_cap;   // light indices set or enabled in the block
} StateBlock9;

struct Device9 {
  Object9 o;
  Object9 *d3d;
  u32 adapter, device_type, focus_window, behavior;   // D3DDEVICE_CREATION_PARAMETERS
  Present9 pp;
  SwapChain9 *swapchain; Surface9 *back_buffer, *auto_depth;
  Surface9 *targets[4], *depth;           // SetRenderTarget, SetDepthStencilSurface: not part of any state block
  State9 state;                           // what a draw uses
  StateBlock9 *recording;                 // between BeginStateBlock and EndStateBlock every Set* goes to it instead
  int in_scene; u32 frames;
  Declaration9 *fvf_declarations;         // the declarations made for FVF codes: the device's, kept until it goes
  u32 software_vp; s32 cursor_x, cursor_y; u32 cursor_shown;
  Buffer9 *up[2];                         // DrawPrimitiveUP's vertices and DrawIndexedPrimitiveUP's indices: the device's own buffers
};

// ---- shared helpers ----
// Formats: bytes a pixel (0 for a block format), block size in pixels and bytes (DXT: 4 and 8 or 16); 0 if unknown
typedef struct { u32 format; u8 bytes, block, block_bytes, depth; } Format9;
const Format9 *d3d9_format(u32 format);
u32 d3d9_pitch(const Format9 *f, u32 width);
u32 d3d9_rows(const Format9 *f, u32 height);
// references
u32 d3d9_addref(Object9 *o);              // the app's: returns the new count
u32 d3d9_release(Object9 *o);
void d3d9_hold(Object9 *o);               // the device's own; null is allowed
void d3d9_unhold(Object9 *o);
void d3d9_object_init(Object9 *o, u32 kind, Device9 *device, Object9 *container);   // refs 1, and the device's reference
void d3d9_destroy(Object9 *o);            // d3d9_resources.c and d3d9_stateblock.c: frees what the object owns
void d3d9_stateblock_destroy(StateBlock9 *b);
Declaration9 *d3d9_fvf_declaration(Device9 *d, u32 fvf);   // the vertex layout an FVF code stands for
Surface9 *d3d9_surface_new(Device9 *d, Object9 *container, u32 level, u32 width, u32 height, u32 format, u32 usage, u32 pool, u32 multisample, u32 quality);
// state (d3d9_state.c, d3d9_stateblock.c)
void d3d9_state_init(Device9 *d);         // a new device's state, from the oracle's
void d3d9_state_free(State9 *s, int unhold);
LightSlot9 *d3d9_light(State9 *s, u32 index, int create);
int d3d9_sampler_index(u32 sampler);      // 0..15, 256 (D3DDMAPSAMPLER), 257..260 (the vertex samplers) -> 0..20, or -1
void d3d9_recorded(StateBlock9 *b, u32 klass, u32 index);   // marks an entry of the block being recorded
enum { C_RS = 0, C_TSS, C_SAMP, C_TEXTURE, C_STREAM, C_STREAM_FREQ, C_TRANSFORM, C_CLIP_PLANE, C_VS_FLOAT, C_PS_FLOAT, C_VS_INT, C_PS_INT, C_VS_BOOL, C_PS_BOOL };
void d3d9_recorded_light(StateBlock9 *b, u32 index);
extern Light9 d3d9_default_light;   // what a light is when it is enabled without having been set (the oracle's)

// ---- d3d9_gpu.c: clears, draws and presents as records of the GPU ring (rt/d3d12_cmd.h, the explicit records) ----
void d3d9_gpu_clear(Device9 *d, u32 flags, u32 color, float z, u32 stencil);   // Clear, of the whole target
// DrawPrimitive (indexed 0: `first` is a vertex) or DrawIndexedPrimitive (`first` is an index, `base` is added to
// every index) of `primitives` primitives of a D3DPRIMITIVETYPE, with the device's state as it is now
void d3d9_gpu_draw(Device9 *d, u32 type, s32 base, u32 first, u32 primitives, int indexed);
void d3d9_gpu_present(Device9 *d);                 // the back buffer goes to the screen
void d3d9_gpu_buffer_locked(Buffer9 *b, u32 offset, u32 size, u32 flags);   // a lock for writing: what it may change
void d3d9_gpu_release(Object9 *o);                 // the object is going: so is what it has on the GPU
int d3d9_gpu_read(Surface9 *s, u8 *dst, u32 pitch);   // a render target's pixels from the GPU into `dst` (0: it has none there)
// d3d9_resources.c: the device's buffer for a UP draw's vertices (0) or indices (1), holding `bytes` from `src`
Buffer9 *d3d9_up_buffer(Device9 *d, int which, const void *src, u32 bytes, u32 format);
void d3d9_gpu_shot(const char *name);              // a session's checkpoint: the host saves the frame presented last
void d3d9_gpu_stats(void);                         // logs how many shaders, pipelines and draws there were
void d3d9_gpu_finish(void);                        // the process ends: returns once the back end has seen everything

// ---- statistics: calls per method under the names Wine's +d3d9 trace gives them. SW_D3D9_STATS=1 prints them when
// the process ends, with the values the arguments that matter took; the session player prints the calls since
// the last checkpoint at each checkpoint; SW_D3D9_FRAMES=1 prints the calls of every frame (runs of frames that
// made the same calls on one line). tools/oracle-wine/d3d9-compare.py sets them against Wine's trace.
typedef struct Stat9 { const char *name; u64 calls, mark, frame_mark; struct Stat9 *next; } Stat9;
void d3d9_stat_add(Stat9 *s);
#define STAT(n) do { static Stat9 stat_ = {#n, 0, 0, 0, 0}; if (!stat_.calls++) d3d9_stat_add(&stat_); } while (0)
extern int d3d9_stats;                    // SW_D3D9_STATS
void d3d9_arg_count(const char *what, u32 key, u32 value);
#define ARGV(what, value) do { if (d3d9_stats) d3d9_arg_count(what, 0xFFFFFFFFu, (u32)(value)); } while (0)
#define ARGK(what, key, value) do { if (d3d9_stats) d3d9_arg_count(what, (u32)(key), (u32)(value)); } while (0)
void d3d9_stats_print(const char *title, int since_mark);   // also called by the session player at a checkpoint
