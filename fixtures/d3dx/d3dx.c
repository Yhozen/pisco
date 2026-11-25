// SPDX-License-Identifier: GPL-2.0-or-later
// A 32-bit title that calls D3DX's HLSL preprocessor and compiler directly, as Tag's effects reach them: a source
// with an #include answered by the title's own ID3DXInclude, preprocessed, then compiled for vs_2_0 and ps_2_0. It
// prints the preprocessed text, each bytecode's length and hash, and any error text. Run with Microsoft's
// d3dx9_35.dll beside it (the user's own, from Tag's installer: never in this repository): what it prints under
// Wine with that DLL is what PISCO's lifted copy must print (tests/run.sh d3dx). No C runtime of its own.
// expected.txt: what it printed under Wine with Microsoft's DLL loaded natively (WINEDLLOVERRIDES=d3dx9_35=n, checked with
// +loaddll): Wine's own d3dx9_35 makes other text and other bytecode.
typedef unsigned int u32; typedef unsigned char u8;
__declspec(dllimport) void *__stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void __stdcall ExitProcess(u32);
typedef struct Blob { struct BlobVtbl *v; } Blob;
struct BlobVtbl { long (__stdcall *QueryInterface)(Blob *, const void *, void **); u32 (__stdcall *AddRef)(Blob *); u32 (__stdcall *Release)(Blob *);
                  void *(__stdcall *GetBufferPointer)(Blob *); u32 (__stdcall *GetBufferSize)(Blob *); };
typedef struct Include { struct IncludeVtbl *v; } Include;
struct IncludeVtbl { long (__stdcall *Open)(Include *, u32 type, const char *name, const void *parent, const void **data, u32 *bytes);
                     long (__stdcall *Close)(Include *, const void *data); };
typedef struct { const char *name, *definition; } Macro;
__declspec(dllimport) long __stdcall D3DXPreprocessShader(const char *src, u32 len, const Macro *defines, Include *inc, Blob **text, Blob **errors);
__declspec(dllimport) long __stdcall D3DXCompileShader(const char *src, u32 len, const Macro *defines, Include *inc, const char *entry, const char *profile,
                                                       u32 flags, Blob **code, Blob **errors, void **constants);

static void out(const char *s) { u32 n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((u32)-11), s, n, &w, 0); }
static void outn(const char *s, u32 n) { u32 w; WriteFile(GetStdHandle((u32)-11), s, n, &w, 0); }
static void hex(u32 v) { char b[11] = "0x"; for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[10] = 0; out(b); }
static void dec(u32 v) { char b[12]; int k = 11; b[k] = 0; do { b[--k] = (char)('0' + v % 10); v /= 10; } while (v); out(b + k); }
static u32 fnv(const u8 *p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

static const char header[] = "float4 tint = float4(0.5, 0.25, 1.0, 1.0);\n#define SCALE 2.0\n";
static long __stdcall inc_open(Include *self, u32 type, const char *name, const void *parent, const void **data, u32 *bytes) {
  (void)self; (void)parent; out("include open: type "); dec(type); out(" name \""); out(name ? name : "(null)"); out("\"\n");
  *data = header; *bytes = sizeof header - 1; return 0;
}
static long __stdcall inc_close(Include *self, const void *data) { (void)self; (void)data; return 0; }
static struct IncludeVtbl inc_vtbl = {inc_open, inc_close};
static Include inc = {&inc_vtbl};

static const char source[] =
  "#include \"scene.h\"\n"
  "float4x4 wvp;\nsampler2D tex;\n"
  "struct V { float4 p : POSITION; float2 t : TEXCOORD0; float3 n : NORMAL; };\n"
  "struct P { float4 p : POSITION; float2 t : TEXCOORD0; float3 n : TEXCOORD1; };\n"
  "P vs(V v) { P o; o.p = mul(v.p, wvp); o.t = v.t * SCALE; o.n = normalize(v.n); return o; }\n"
  "float4 ps(P i) : COLOR0 { float d = saturate(dot(normalize(i.n), float3(0, 1, 0))); return tex2D(tex, i.t) * tint * pow(d, 4); }\n";

static void show(const char *what, long hr, Blob *b, Blob *errors, int text) {
  out(what); out(": "); hex((u32)hr);
  if (b) { u8 *p = b->v->GetBufferPointer(b); u32 n = b->v->GetBufferSize(b); out(" bytes "); dec(n); out(" fnv "); hex(fnv(p, n)); out("\n"); if (text) { outn((const char *)p, n && !p[n - 1] ? n - 1 : n); out("\n"); } b->v->Release(b); }
  else out("\n");
  if (errors) { u8 *p = errors->v->GetBufferPointer(errors); u32 n = errors->v->GetBufferSize(errors); out("errors: "); outn((const char *)p, n && !p[n - 1] ? n - 1 : n); out("\n"); errors->v->Release(errors); }
}
void start(void) {
  Blob *b = 0, *e = 0;
  long hr = D3DXPreprocessShader(source, sizeof source - 1, 0, &inc, &b, &e); show("preprocess", hr, b, e, 1);
  b = e = 0; hr = D3DXCompileShader(source, sizeof source - 1, 0, &inc, "vs", "vs_2_0", 0, &b, &e, 0); show("vs_2_0", hr, b, e, 0);
  b = e = 0; hr = D3DXCompileShader(source, sizeof source - 1, 0, &inc, "ps", "ps_2_0", 0, &b, &e, 0); show("ps_2_0", hr, b, e, 0);
  b = e = 0; hr = D3DXCompileShader(source, sizeof source - 1, 0, &inc, "ps", "ps_2_a", 0, &b, &e, 0); show("ps_2_a", hr, b, e, 0);
  ExitProcess(0);
}
