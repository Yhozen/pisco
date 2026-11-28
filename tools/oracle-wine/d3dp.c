// SPDX-License-Identifier: GPL-2.0-or-later
// d3dp.dll: a stand-in for d3d9.dll's Direct3DCreate9 that writes every shader bytecode the app (and D3DX in it)
// passes to IDirect3DDevice9::CreateVertexShader / CreatePixelShader, then calls Wine's real d3d9.
// Files: %PISCO_SHADER_DIR%\NNN-vs.bin / NNN-ps.bin (the tokens up to and including the end token).
// Build: i686-w64-mingw32-clang -shared tools/oracle-wine/d3dp.c -o d3dp.dll (docs/tag.md: how it is put in front of d3d9 for a capture)
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *create_shader_fn)(void *dev, const DWORD *tokens, void **shader);
typedef HRESULT (WINAPI *create_device_fn)(void *d3d, UINT a, DWORD t, HWND w, DWORD f, void *pp, void **dev);
typedef void *(WINAPI *create9_fn)(UINT sdk);
static create_shader_fn real_vs, real_ps;
static create_device_fn real_cd;
static int seq;

static size_t shader_size(const DWORD *t) {
    DWORD ver = t[0]; size_t i = 1; int major = (ver >> 8) & 0xff;
    for (;;) {
        DWORD tok = t[i];
        if (tok == 0x0000FFFF) return (i + 1) * 4;
        if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        if (major >= 2) i += 1 + ((tok >> 24) & 0xF); else i += 1;
        if (i > (1u << 20)) return 0;
    }
}
static void dump(const char *kind, const DWORD *t) {
    char dir[MAX_PATH], path[MAX_PATH];
    if (!GetEnvironmentVariableA("PISCO_SHADER_DIR", dir, sizeof dir)) return;
    size_t n = shader_size(t);
    snprintf(path, sizeof path, "%s\\%03d-%s.bin", dir, seq++, kind);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(t, 1, n, f); fclose(f);
}
static HRESULT WINAPI hook_vs(void *dev, const DWORD *t, void **s) { if (t) dump("vs", t); return real_vs(dev, t, s); }
static HRESULT WINAPI hook_ps(void *dev, const DWORD *t, void **s) { if (t) dump("ps", t); return real_ps(dev, t, s); }
static void patch(void **vtbl, int i, void *fn, void **old) {
    DWORD prot; VirtualProtect(&vtbl[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &prot);
    if (!*old) *old = vtbl[i];
    vtbl[i] = fn; VirtualProtect(&vtbl[i], sizeof(void *), prot, &prot);
}
static HRESULT WINAPI hook_cd(void *d3d, UINT a, DWORD t, HWND w, DWORD f, void *pp, void **dev) {
    HRESULT hr = real_cd(d3d, a, t, w, f, pp, dev);
    if (SUCCEEDED(hr) && dev && *dev && !real_vs) {
        void **vtbl = *(void ***)*dev;
        patch(vtbl, 91, (void *)hook_vs, (void **)&real_vs);   // IDirect3DDevice9::CreateVertexShader
        patch(vtbl, 106, (void *)hook_ps, (void **)&real_ps);  // IDirect3DDevice9::CreatePixelShader
    }
    return hr;
}
__declspec(dllexport) void *WINAPI Direct3DCreate9(UINT sdk) {
    HMODULE m = LoadLibraryA("d3d9.dll");
    create9_fn c = (create9_fn)GetProcAddress(m, "Direct3DCreate9");
    void *d3d = c(sdk);
    if (d3d && !real_cd) patch(*(void ***)d3d, 16, (void *)hook_cd, (void **)&real_cd);  // IDirect3D9::CreateDevice
    return d3d;
}
