// Immersive Warp Native - WarpHook.dll (scanout pipeline Phase 1)
//
// Injected into a target process. Hooks DXGI swapchain creation (both the
// classic IDXGIFactory::CreateSwapChain and IDXGIFactory2::CreateSwapChainForHwnd
// via vtable patch) and, for every swapchain created afterwards, hooks Present.
// Inside the Present hook the back buffer is warped in place: copy the app's
// frame, render it through a grid mesh whose vertices carry the warp offsets
// from the host's shared config, then let the original Present scan it out.
//
// Phase 1 scope: pipeline proof. Mesh is a bilinear blend of 4 corner NDC
// offsets published by the host through IWNHookCfg. Blend shaders, per-output
// meshes and the full control-point model arrive in later phases.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wchar.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <vector>
#include <cstring>
#include <mutex>
#include "hook_ipc.h"

// forward decls (vtable patchers reference them)
static HRESULT WINAPI HookPresent(IDXGISwapChain* sw, UINT sync, UINT flags);
static HRESULT WINAPI HookPresent1(IDXGISwapChain1* sw, UINT sync, UINT flags,
                                   DXGI_PRESENT_PARAMETERS* pp);
static HRESULT WINAPI HookCreateSwapChain(IUnknown* fac, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
static HRESULT WINAPI HookCreateSwapChainForHwnd(IUnknown* fac, IUnknown* dev, HWND hwnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* d1,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd,
                                                 IDXGIOutput* ign, IDXGISwapChain1** out);

// ------------------------------------------------------- exported progress
// File logging can fail silently in a manual-mapped image (no CRT, possibly no
// writable APPDATA under SYSTEM), so the injector reads these two counters back
// out of the target image instead:
//   g_entryHits - incremented as the first instruction of HookEntry
//   g_stage     - how far HookEntry got: 1 enter, 2 unwind ok, 3 DTL ok,
//                 4 VEH ok, 5 thread spawned, 6 returned
// Both are exported data so the host can find their RVA in the export table.
extern "C" {
__declspec(dllexport) volatile LONG g_entryHits = 0;
__declspec(dllexport) volatile LONG g_stage = 0;
}
static HANDLE g_instMutex = nullptr;   // single-instance guard (leaked on purpose)

// desktop warp stage 0 probe (DwmProbe.cpp): only armed inside dwm.exe
extern "C" void DwmProbeInstall(HMODULE self);
static HMODULE g_selfMod = nullptr;
static DWORD WINAPI probeThread(LPVOID) { DwmProbeInstall(g_selfMod); return 0; }

// ---------------------------------------------------------------- logging
// No CRT, no std::mutex here: manual-mapped images never run the CRT init, so
// keep the logger to raw Win32 + Interlocked. Falls back to C://Windows//Temp
// when APPDATA is unavailable/unwritable (that is the SYSTEM/dwm case).
static BOOL appendFileA(const char* path, const char* buf, int len) {
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    SetFilePointer(f, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(f, buf, (DWORD)len, &w, nullptr);
    CloseHandle(f);
    return TRUE;
}

static void hookLog(const char* line) {
    static volatile LONG lock = 0;
    char path[MAX_PATH];
    char buf[512];
    SYSTEMTIME st; GetLocalTime(&st);
    int len = wsprintfA(buf, "[%02d:%02d:%02d.%03d] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    while (InterlockedCompareExchange((volatile LONG*)&lock, 1, 0) != 0) Sleep(0);
    DWORD n = GetEnvironmentVariableA("APPDATA", path, MAX_PATH);
    BOOL wrote = FALSE;
    if (n && n + 40 < MAX_PATH) {
        lstrcatA(path, "\\ImmersiveWarpNative");
        CreateDirectoryA(path, nullptr);
        lstrcatA(path, "\\hook.log");
        wrote = appendFileA(path, buf, len);
    }
    if (!wrote) appendFileA("C:\\Windows\\Temp\\iwndashook.log", buf, len);
    InterlockedExchange((volatile LONG*)&lock, 0);
}

// ---------------------------------------------------------------- config
static IWNHookCfg* g_cfg = nullptr;          // mapped view, may stay null
static DWORD g_cfgLastTry = 0;               // GetTickCount pacing
static int g_cfgTries = 0;
// false when we could only attach the mapping read-only: the write-backs
// (pass counter + exception telemetry) must be skipped or they AV on a
// PAGE_READONLY view. Warp config itself is read-only for us either way.
static bool g_cfgWrite = false;

static IWNHookCfg* ensureCfg() {
    if (g_cfg) return g_cfg;
    DWORD now = GetTickCount();
    if (now - g_cfgLastTry < 1000) return nullptr;   // retry at 1 Hz
    g_cfgLastTry = now;
    g_cfgTries++;
    DWORD access = FILE_MAP_ALL_ACCESS;
    HANDLE h = OpenFileMappingW(access, FALSE, IWN_HOOK_MAPPING_NAME);
    if (!h) {
        DWORD gle = GetLastError();
        h = OpenFileMappingW(FILE_MAP_READ, FALSE, IWN_HOOK_MAPPING_NAME);
        if (h) {
            access = FILE_MAP_READ;
            if (g_cfgTries % 10 == 1)
                hookLog("cfg: ALL_ACCESS open denied, read-only attach (write-backs disabled)");
        } else {
            if (g_cfgTries % 10 == 1) {
                char b[64];
                wsprintfA(b, "cfg: open failed gle=%u", gle);
                hookLog(b);
            }
            return nullptr;
        }
    }
    g_cfgWrite = (access == FILE_MAP_ALL_ACCESS);
    g_cfg = (IWNHookCfg*)MapViewOfFile(h, access, 0, 0, sizeof(IWNHookCfg));
    if (g_cfg && (g_cfg->magic != IWN_HOOK_MAGIC || g_cfg->size != sizeof(IWNHookCfg))) {
        hookLog("cfg: magic/size mismatch, ignoring host mapping");
        UnmapViewOfFile(g_cfg); g_cfg = nullptr;
    } else if (g_cfg) {
        char buf[80];
        wsprintfA(buf, "cfg mapping attached (enabled=%d gridN=%d)", (int)g_cfg->enabled, (int)g_cfg->gridN);
        hookLog(buf);
    }
    return g_cfg;
}

// ------------------------------------------------- desktop probe bridge (DwmProbe.cpp)
// The desktop-warp stage 0 probe lives in its own translation unit so the
// dwm-specific import-table surgery stays separable from the dxgi path. It
// needs only two things from here: the file logger and the IPC write-back.
void iwnHookLog(const char* line) { hookLog(line); }

void iwnHookCounters(LONG c0, LONG c1, LONG c2) {
    // dwm has no Present detour, so reserved[] is free for the probe counters
    // (confirm frames / render-target binds / d3d11 device creations).
    if (g_cfg && g_cfgWrite) {
        g_cfg->reserved[0] = (DWORD)c0;
        g_cfg->reserved[1] = (DWORD)c1;
        g_cfg->reserved[2] = (DWORD)c2;
    }
}

// ---------------------------------------------------------------- shaders
static const char* kVS = R"(
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 1.0);
    o.uv = i.uv;
    return o;
}
)";
static const char* kPS = R"(
Texture2D srcTex : register(t0);
SamplerState sam : register(s0);
// blend-band constants, packed by the DLL from IWNHookCfg:
//  fadeW = per-edge fade width (L,R,T,B), uv fraction
//  edgeG = per-edge gamma exponent
//  gainS = per-edge gain, slopeS = per-edge slope (parametric curve,
//          same formula as the host engine: (g+1)x / (s(x-1)+gx+1))
//  misc  = x: edges mask (bit0 L, 1 R, 2 T, 3 B)
//  chan  = per-channel blend gamma R,G,B (1/1/1 = legacy scalar weight)
cbuffer BlendCB : register(b0) {
    float4 fadeW; float4 edgeG; float4 gainS; float4 slopeS; float4 misc; float4 chan;
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
static float curve(float t, float g, float s) {
    return (g + 1.0) * t / (s * (t - 1.0) + g * t + 1.0);
}
static float3 edgeFactor(float t, int idx) {
    float fade = fadeW[idx];
    if (fade <= 0.0) return 1.0.xxx;
    float x = saturate(t / fade);
    float b = curve(x, gainS[idx], slopeS[idx]);
    float g = edgeG[idx];
    return float3(pow(b, g * chan.x), pow(b, g * chan.y), pow(b, g * chan.z));
}
float4 main(PSIn i) : SV_TARGET {
    float4 c = srcTex.Sample(sam, i.uv);
    uint m = (uint)misc.x;
    float3 w = 1.0.xxx;
    if (m & 1u)  w *= edgeFactor(i.uv.x, 0);
    if (m & 2u)  w *= edgeFactor(1.0 - i.uv.x, 1);
    if (m & 4u)  w *= edgeFactor(i.uv.y, 2);
    if (m & 8u)  w *= edgeFactor(1.0 - i.uv.y, 3);
    c.rgb *= w;
    return c;
}
)";
// solid-color pixel shader for the calibration mesh-line overlay
static const char* kPSLine = R"(
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET { return float4(1.0, 1.0, 0.0, 1.0); }
)";

// ---------------------------------------------------------------- per-swapchain context
struct HookCtx {
    ID3D11Device*           dev = nullptr;
    ID3D11DeviceContext*    ctx = nullptr;
    ID3D11Texture2D*        copyTex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11Buffer*           vb = nullptr;
    ID3D11VertexShader*     vs = nullptr;
    ID3D11PixelShader*      ps = nullptr;
    ID3D11SamplerState*     ss = nullptr;
    ID3D11InputLayout*      il = nullptr;
    ID3D11Buffer*           cb = nullptr;   // blend constants (b0)
    ID3D11PixelShader*      psLine = nullptr;   // mesh-line overlay
    ID3D11Buffer*           lineVB = nullptr;   // line-segment topology mesh
    UINT lineVertCount = 0;
    UINT w = 0, h = 0;          // back buffer size of cached resources
    UINT vertCount = 0;         // vertices in the cached VB
    LONG  gridN = 0;            // corner-model mesh resolution of cached VB
    int   mode = -1;            // 0 = corner, 1 = full mesh (-1 = build pending)
    int   slotIdx = -1;         // per-output slot the cached VB was built from
    LONG  meshRev = 0;          // host publish revision of cached VB
    float corner[4][2] = {};    // corner model of cached VB
    float blendCache[24] = {};  // last blend constants uploaded to cb
    DWORD frames = 0, logged = 0;
    void* bbKey = nullptr;      // ID3D11Texture2D* of back buffer
};

static std::mutex g_ctxMutex;
static HookCtx g_ctx[8];                 // fixed slots, keyed by back buffer

static void releaseSlot(HookCtx& c) {
    if (c.srv) c.srv->Release();
    if (c.rtv) c.rtv->Release();
    if (c.copyTex) c.copyTex->Release();
    if (c.vb) c.vb->Release();
    if (c.vs) c.vs->Release();
    if (c.ps) c.ps->Release();
    if (c.ss) c.ss->Release();
    if (c.il) c.il->Release();
    if (c.cb) c.cb->Release();
    if (c.psLine) c.psLine->Release();
    if (c.lineVB) c.lineVB->Release();
    if (c.ctx) c.ctx->Release();
    if (c.dev) c.dev->Release();
    ZeroMemory(&c, sizeof(c));
}

static bool compileShaders(HookCtx& c) {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kVS, lstrlenA(kVS), nullptr, nullptr, nullptr, "main",
                          "vs_5_0", 0, 0, &vsb, &err))) {
        hookLog("vs compile failed"); return false;
    }
    if (FAILED(D3DCompile(kPS, lstrlenA(kPS), nullptr, nullptr, nullptr, "main",
                          "ps_5_0", 0, 0, &psb, &err))) {
        hookLog("ps compile failed"); return false;
    }
    HRESULT hr1 = c.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &c.vs);
    HRESULT hr2 = c.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &c.ps);
    if (FAILED(hr1) || FAILED(hr2)) { hookLog("shader create failed"); return false; }
    ID3DBlob* lsb = nullptr;
    if (FAILED(D3DCompile(kPSLine, lstrlenA(kPSLine), nullptr, nullptr, nullptr, "main",
                          "ps_5_0", 0, 0, &lsb, &err)) ||
        FAILED(c.dev->CreatePixelShader(lsb->GetBufferPointer(), lsb->GetBufferSize(), nullptr, &c.psLine))) {
        if (lsb) lsb->Release();
        hookLog("line ps create failed"); return false;
    }
    if (lsb) lsb->Release();
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(c.dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(),
                                        vsb->GetBufferSize(), &c.il))) {
        hookLog("input layout failed"); return false;
    }
    vsb->Release(); psb->Release();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(c.dev->CreateSamplerState(&sd, &c.ss))) { hookLog("sampler failed"); return false; }
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 96;    // 6 float4: fadeW edgeG gainS slopeS misc chan
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(c.dev->CreateBuffer(&cbd, nullptr, &c.cb))) { hookLog("cb failed"); return false; }
    return true;
}

static bool createVB(HookCtx& c, const void* data, size_t bytes, ID3D11Buffer** out, const char* tag) {
    if (*out) { (*out)->Release(); *out = nullptr; }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)bytes;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    if (FAILED(c.dev->CreateBuffer(&bd, &init, out))) { hookLog(tag); return false; }
    return true;
}

static bool buildMesh(HookCtx& c, const IWNHookCfg& cfg, const IWNOutSlot* slot) {
    const LONG n = cfg.gridN >= 2 && cfg.gridN <= 64 ? cfg.gridN : 16;
    struct V { float pos[3]; float uv[2]; };
    std::vector<V> grid;               // row-major unique grid vertices
    int RV, CV;
    if (slot && slot->active && slot->meshRows >= 2 && slot->meshCols >= 2 &&
        (LONG)slot->meshRows * slot->meshCols <= IWN_MAX_MESH_VERTS) {
        // per-output control-point mesh published by the host: row-major grid
        // of IWNMeshVtx (sx,sy = src uv, dx,dy = dest in 0..1 space)
        RV = slot->meshRows; CV = slot->meshCols;
        grid.resize((size_t)RV * CV);
        for (int i = 0; i < RV; i++)
            for (int j = 0; j < CV; j++) {
                const IWNMeshVtx& s = slot->verts[(size_t)i * CV + j];
                V& d = grid[(size_t)i * CV + j];
                d.pos[0] = 2 * s.dx - 1;
                d.pos[1] = 1 - 2 * s.dy;
                d.pos[2] = 0;
                d.uv[0] = s.sx;
                d.uv[1] = s.sy;
            }
        c.mode = 1;
        c.meshRev = cfg.meshRev;
    } else {
        // corner fallback (Phase 1 PoC): bilinear blend of 4 NDC offsets
        RV = CV = (int)n;
        grid.resize((size_t)n * n);
        const float* c00 = cfg.corner[0];  // TL
        const float* c10 = cfg.corner[1];  // TR
        const float* c01 = cfg.corner[2];  // BL
        const float* c11 = cfg.corner[3];  // BR
        for (LONG y = 0; y < n; y++) {
            for (LONG x = 0; x < n; x++) {
                float u = (float)x / (n - 1), v = (float)y / (n - 1);
                float dx = (1 - u) * (1 - v) * c00[0] + u * (1 - v) * c10[0] +
                           (1 - u) * v * c01[0] + u * v * c11[0];
                float dy = (1 - u) * (1 - v) * c00[1] + u * (1 - v) * c10[1] +
                           (1 - u) * v * c01[1] + u * v * c11[1];
                V& d = grid[(size_t)y * n + x];
                d.pos[0] = (2 * u - 1) + dx;
                d.pos[1] = (1 - 2 * v) + dy;
                d.pos[2] = 0;
                d.uv[0] = u;
                d.uv[1] = v;
            }
        }
        c.mode = 0;
        c.gridN = n;
    }
    // warp pass: two triangles per cell (0,2,1)(1,2,3), TL,TR,BL,BR ordering
    std::vector<V> data;
    data.reserve((size_t)(RV - 1) * (CV - 1) * 6);
    for (int i = 0; i < RV - 1; i++) {
        for (int j = 0; j < CV - 1; j++) {
            const V* q[4] = {
                &grid[(size_t)i * CV + j],     &grid[(size_t)i * CV + j + 1],
                &grid[(size_t)(i + 1) * CV + j], &grid[(size_t)(i + 1) * CV + j + 1],
            };
            const int idl[6] = { 0, 2, 1, 1, 2, 3 };
            for (int k = 0; k < 6; k++) data.push_back(*q[idl[k]]);
        }
    }
    // calibration overlay: horizontal + vertical line segments over the grid
    std::vector<V> lines;
    lines.reserve(((size_t)(RV - 1) * CV + (size_t)(CV - 1) * RV) * 2);
    for (int i = 0; i < RV; i++)
        for (int j = 0; j < CV - 1; j++) {
            lines.push_back(grid[(size_t)i * CV + j]);
            lines.push_back(grid[(size_t)i * CV + j + 1]);
        }
    for (int j = 0; j < CV; j++)
        for (int i = 0; i < RV - 1; i++) {
            lines.push_back(grid[(size_t)i * CV + j]);
            lines.push_back(grid[(size_t)(i + 1) * CV + j]);
        }
    if (!createVB(c, data.data(), data.size() * sizeof(V), &c.vb, "vb create failed")) return false;
    if (!createVB(c, lines.data(), lines.size() * sizeof(V), &c.lineVB, "line vb create failed")) return false;
    c.vertCount = (UINT)data.size();
    c.lineVertCount = (UINT)lines.size();
    memcpy(c.corner, cfg.corner, sizeof(c.corner));
    if (c.mode == 1) {
        char buf[128];
        wsprintfA(buf, "mesh v2: rows=%d cols=%d verts=%d slot=%d mon=%ls",
                  (int)slot->meshRows, (int)slot->meshCols, (int)(slot->meshRows * slot->meshCols),
                  c.slotIdx, slot->monitor[0] ? slot->monitor : L"<any>");
        hookLog(buf);
    }
    return true;
}

static bool ensureResources(HookCtx& c, ID3D11Texture2D* bb, const IWNHookCfg& cfg,
                            const IWNOutSlot* slot, int slotIdx) {
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    bool rebuild = (!c.dev || c.bbKey != (void*)bb || c.w != td.Width || c.h != td.Height);
    if (rebuild) {
        releaseSlot(c);
        bb->GetDevice(&c.dev);   // DeviceChild::GetDevice(ID3D11Device**), void return
        if (!c.dev) { hookLog("GetDevice failed"); return false; }
        c.dev->GetImmediateContext(&c.ctx);
        c.bbKey = (void*)bb;
        c.w = td.Width; c.h = td.Height;
        if (!compileShaders(c)) { releaseSlot(c); return false; }
        D3D11_TEXTURE2D_DESC cd = td;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.MiscFlags = 0;
        if (FAILED(c.dev->CreateTexture2D(&cd, nullptr, &c.copyTex))) {
            hookLog("copy tex failed"); releaseSlot(c); return false;
        }
        if (FAILED(c.dev->CreateShaderResourceView(c.copyTex, nullptr, &c.srv))) {
            hookLog("srv failed"); releaseSlot(c); return false;
        }
        if (FAILED(c.dev->CreateRenderTargetView(bb, nullptr, &c.rtv))) {
            hookLog("rtv failed"); releaseSlot(c); return false;
        }
        c.gridN = 0;  // force mesh build
        char buf[128];
        wsprintfA(buf, "resources built: %ux%u", (int)td.Width, (int)td.Height);
        hookLog(buf);
    }
    bool meshStale =
        c.mode != (slot ? 1 : 0) ||
        c.slotIdx != slotIdx ||
        (slot && c.meshRev != cfg.meshRev) ||
        (!slot && (!c.gridN || memcmp(c.corner, cfg.corner, sizeof(c.corner)) != 0));
    if (meshStale) {
        c.slotIdx = slotIdx;
        if (!buildMesh(c, cfg, slot)) { releaseSlot(c); return false; }
    }
    // blend constants: repack the output slot's per-side AoS block
    // (blend[e*4+{fade,gamma,gain,slope}]) into the shader's SoA float4s
    // (fadeW / edgeG / gainS / slopeS = per-edge across the 4 sides), then
    // upload when anything changed. The corner fallback has no blend bands.
    {
        float blk[24] = {};
        if (slot) {
            for (int e = 0; e < 4; e++) {
                blk[e + 0]  = slot->blend[e * 4 + 0];   // fadeW
                blk[e + 4]  = slot->blend[e * 4 + 1];   // edgeG
                blk[e + 8]  = slot->blend[e * 4 + 2];   // gainS
                blk[e + 12] = slot->blend[e * 4 + 3];   // slopeS
            }
            blk[16] = slot->chanBlend[0];
            blk[17] = slot->chanBlend[1];
            blk[18] = slot->chanBlend[2];
            blk[20] = (float)slot->edgesMask;
        }
        if (memcmp(c.blendCache, blk, sizeof(blk)) != 0) {
            c.ctx->UpdateSubresource(c.cb, 0, nullptr, blk, 0, 0);
            memcpy(c.blendCache, blk, sizeof(blk));
        }
    }
    return true;
}

// ---------------------------------------------------------------- hooks
typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* PFN_CreateSwapChain)(IUnknown*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(WINAPI* PFN_CreateSwapChainForHwnd)(IUnknown*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);

static PFN_CreateSwapChain         g_origCreate = nullptr;
static PFN_CreateSwapChainForHwnd  g_origCreateHwnd = nullptr;
static volatile LONG g_swapCount = 0;
// set while createSelfProbes is creating its own swapchains: those must NOT
// log "swapchain hooked" or bump the counter - test scripts key on that line
// meaning a real application swapchain was captured
static volatile LONG g_inSelfProbe = 0;
// self-probe swapchains kept alive for the watchdog. File scope on purpose: a
// function-local static initialised from a variable needs the CRT's
// thread-safe-static guard, which lives in TLS - and a manual-mapped image has
// no loader TLS registration, so that guard dereferences a foreign slot and
// takes the init thread down (this was the DLL's only TLS consumer).
static IDXGISwapChain* g_keep1 = nullptr;
static IDXGISwapChain1* g_keep2 = nullptr;
static IDXGISwapChain1* g_keep3 = nullptr;   // composition-family probe

// Bitblt and flip-model swapchains are distinct implementation classes with
// distinct vtables - patch each vtable once and remember its original Present.
// DWM (and any flip-model app) may call Present through IDXGISwapChain1+'s
// vtable (Present1 lives in slot 9 there), so every swapchain object's
// IDXGISwapChain1 subobject vtable is patched too. vtables are shared per
// dxgi.dll implementation class, so one patch covers every instance in the
// process - including swapchains created BEFORE the DLL was injected (this
// is exactly what makes the dwm.exe injection work: its per-monitor
// swapchains are already alive at attach time).
struct VtEntry { void** vt; PFN_Present orig; IDXGISwapChain* probe; bool hasP1; PFN_Present1 orig1; };
static VtEntry g_vts[8];
static int g_vtCount = 0;
static std::mutex g_vtMutex;

// caller holds g_vtMutex
static VtEntry* findVt(void** vt) {
    for (int i = 0; i < g_vtCount; i++)
        if (g_vts[i].vt == vt) return &g_vts[i];
    return nullptr;
}

static void patchOneVt(void** vt, bool hasP1) {
    if (g_vtCount >= 8) return;
    DWORD old{};
    if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old)) return;
    PFN_Present orig = (PFN_Present)vt[8];
    *(void* volatile*)&vt[8] = (void*)&HookPresent;
    PFN_Present1 orig1 = nullptr;
    if (hasP1) {
        DWORD old1{};
        if (VirtualProtect(&vt[9], sizeof(void*), PAGE_READWRITE, &old1)) {
            orig1 = (PFN_Present1)vt[9];
            *(void* volatile*)&vt[9] = (void*)&HookPresent1;
            VirtualProtect(&vt[9], sizeof(void*), old1, &old1);
        }
    }
    VirtualProtect(&vt[8], sizeof(void*), old, &old);
    // volatile readback - the compiler must not fold this into the store
    void* now = *(void* volatile*)&vt[8];
    if (now != (void*)&HookPresent) {
        hookLog("Present patch VERIFY FAILED - readback differs");
        return;
    }
    char buf[96];
    wsprintfA(buf, "Present vtable patched (vt=%p p1=%d)", (void*)vt, hasP1 ? 1 : 0);
    hookLog(buf);
    g_vts[g_vtCount].vt = vt;
    g_vts[g_vtCount].orig = orig;
    g_vts[g_vtCount].probe = nullptr;
    g_vts[g_vtCount].hasP1 = hasP1;
    g_vts[g_vtCount].orig1 = orig1;
    g_vtCount++;
}

static PFN_Present patchSwapchainVt(IDXGISwapChain* sw) {
    void** vt = *(void***)sw;
    std::lock_guard<std::mutex> lk(g_vtMutex);
    VtEntry* e = findVt(vt);
    if (e) return e->orig;
    patchOneVt(vt, false);
    e = findVt(vt);
    if (!e) return nullptr;
    e->probe = sw;
    // the IDXGISwapChain1 subobject vtable (Present1 = slot 9)
    IDXGISwapChain1* sw1 = nullptr;
    if (SUCCEEDED(sw->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sw1)) && sw1) {
        void** vt1 = *(void***)sw1;
        if (!findVt(vt1)) patchOneVt(vt1, true);
        sw1->Release();
    }
    return e->orig;
}

static void patchSlot(void** vt, int idx, void* detour, void** origSave, const char* tag) {
    DWORD old{};
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) {
        hookLog("VirtualProtect on factory vtable failed");
        return;
    }
    if (origSave && !*origSave) *origSave = vt[idx];
    vt[idx] = detour;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    hookLog(tag);
}

static void installFactoryHooks() {
    // The app may QI any factory revision. MSVC multiple inheritance gives
    // every interface revision its own subobject vtable, so patch both
    // IDXGIFactory1 and IDXGIFactory2 (same implementation, prefix layout).
    IDXGIFactory1* f1 = nullptr;
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f1))) {
        hookLog("FATAL: CreateDXGIFactory1 failed");
        return;
    }
    f1->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2);
    void** vt1 = *(void***)f1;
    patchSlot(vt1, 10, (void*)&HookCreateSwapChain, (void**)&g_origCreate,
              "factory1 vtable patched (CreateSwapChain)");
    if (f2) {
        void** vt2 = *(void***)f2;
        if (vt2 != vt1) {
            patchSlot(vt2, 10, (void*)&HookCreateSwapChain, (void**)&g_origCreate,
                      "factory2 vtable patched (CreateSwapChain)");
        }
        // idx 15 = IDXGIFactory2::CreateSwapChainForHwnd
        patchSlot(vt2, 15, (void*)&HookCreateSwapChainForHwnd, (void**)&g_origCreateHwnd,
                  "factory2 vtable patched (CreateSwapChainForHwnd)");
        f2->Release();
    }
    f1->Release();
}

static HRESULT WINAPI HookCreateSwapChain(IUnknown* fac, IUnknown* dev,
                                          DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = g_origCreate(fac, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out && !g_inSelfProbe) {
        patchSwapchainVt(*out);
        LONG c = InterlockedIncrement(&g_swapCount);
        char buf[128];
        wsprintfA(buf, "swapchain hooked (#%d) obj=%p vt=%p", (int)c,
                  (void*)*out, (void*)*(void***)*out);
        hookLog(buf);
    }
    return hr;
}

static HRESULT WINAPI HookCreateSwapChainForHwnd(IUnknown* fac, IUnknown* dev, HWND hwnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* d1,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd,
                                                 IDXGIOutput* ign, IDXGISwapChain1** out) {
    HRESULT hr = g_origCreateHwnd(fac, dev, hwnd, d1, fd, ign, out);
    if (SUCCEEDED(hr) && out && *out && !g_inSelfProbe) {
        patchSwapchainVt(*out);
        LONG c = InterlockedIncrement(&g_swapCount);
        char buf[96];
        wsprintfA(buf, "swapchain hooked via CreateSwapChainForHwnd (#%d)", (int)c);
        hookLog(buf);
    }
    return hr;
}

static PFN_Present origPresentFor(IDXGISwapChain* sw) {
    void** vt = *(void***)sw;
    std::lock_guard<std::mutex> lk(g_vtMutex);
    VtEntry* e = findVt(vt);
    return e ? e->orig : nullptr;
}

static PFN_Present1 origPresent1For(IDXGISwapChain* sw) {
    void** vt = *(void***)sw;
    std::lock_guard<std::mutex> lk(g_vtMutex);
    VtEntry* e = findVt(vt);
    return (e && e->hasP1) ? e->orig1 : nullptr;
}

// the whole warp-in-place body, shared by Present and Present1 detours
static void doWarpPass(IDXGISwapChain* sw) {
    IWNHookCfg* cfg = ensureCfg();
    // per-output slot selection (v3 multi-mesh): match the swapchain's
    // containing monitor against the published slots; a slot with an empty
    // monitor name is the catch-all for unmatched swapchains. In the global
    // corner model (gmode=0, hooktest PoC) every swapchain is warped.
    const IWNOutSlot* oslot = nullptr;
    int slotIdx = -1;
    bool monitorOk = true;
    if (cfg && cfg->enabled && cfg->gmode == 1) {
        wchar_t devName[32] = L"";
        IDXGIOutput* out = nullptr;
        if (SUCCEEDED(sw->GetContainingOutput(&out)) && out) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(out->GetDesc(&od)))
                lstrcpynW(devName, od.DeviceName, 32);
            out->Release();
        }
        int fallback = -1;
        for (int i = 0; i < IWN_MAX_OUTPUTS && i < (int)cfg->outCount; i++) {
            if (!cfg->outs[i].active) continue;
            if (!cfg->outs[i].monitor[0]) { if (fallback < 0) fallback = i; continue; }
            if (lstrcmpiW(devName, cfg->outs[i].monitor) == 0) { slotIdx = i; break; }
        }
        if (slotIdx < 0) slotIdx = fallback;
        if (slotIdx >= 0) oslot = &cfg->outs[slotIdx];
        monitorOk = oslot != nullptr;   // unmatched monitor = clean passthrough
    }
    if (cfg && cfg->enabled && sw && monitorOk) {
        static bool firstPassLogged = false;
        if (!firstPassLogged) {
            firstPassLogged = true;
            hookLog("first warp pass attempt (enabled=1 seen)");
        }
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            std::lock_guard<std::mutex> lk(g_ctxMutex);
            HookCtx* slot = nullptr;
            for (HookCtx& c : g_ctx) if (!c.bbKey || c.bbKey == (void*)bb) { slot = &c; break; }
            if (slot && ensureResources(*slot, bb, *cfg, oslot, slotIdx)) {
                HookCtx& c = *slot;
                c.ctx->CopyResource(c.copyTex, bb);
                D3D11_VIEWPORT vp = { 0, 0, (float)c.w, (float)c.h, 0, 1 };
                c.ctx->OMSetRenderTargets(1, &c.rtv, nullptr);
                c.ctx->RSSetViewports(1, &vp);
                UINT stride = 20, off = 0;
                ID3D11Buffer* vbp = c.vb;
                c.ctx->IASetVertexBuffers(0, 1, &vbp, &stride, &off);
                c.ctx->IASetInputLayout(c.il);
                c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c.ctx->VSSetShader(c.vs, nullptr, 0);
                c.ctx->PSSetShader(c.ps, nullptr, 0);
                c.ctx->PSSetConstantBuffers(0, 1, &c.cb);
                c.ctx->PSSetSamplers(0, 1, &c.ss);
                ID3D11ShaderResourceView* srvp = c.srv;
                c.ctx->PSSetShaderResources(0, 1, &srvp);
                c.ctx->Draw(c.vertCount, 0);
                // calibration overlay: redraw the warp mesh as yellow lines
                if (cfg->overlay && c.lineVB && c.lineVertCount) {
                    c.ctx->PSSetShader(c.psLine, nullptr, 0);
                    ID3D11Buffer* lvb = c.lineVB;
                    c.ctx->IASetVertexBuffers(0, 1, &lvb, &stride, &off);
                    c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                    c.ctx->Draw(c.lineVertCount, 0);
                    // restore for the next frame's warp draw
                    c.ctx->IASetVertexBuffers(0, 1, &vbp, &stride, &off);
                    c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                }
                ID3D11ShaderResourceView* nullSrv = nullptr;
                c.ctx->PSSetShaderResources(0, 1, &nullSrv);
                c.frames++;
                if (c.frames - c.logged >= 300) {
                    c.logged = c.frames;
                    char buf[96];
                    wsprintfA(buf, "warp pass running, frames=%d", (int)c.frames);
                    hookLog(buf);
                }
            }
            bb->Release();
        }
    }
}

static HRESULT WINAPI HookPresent(IDXGISwapChain* sw, UINT sync, UINT flags) {
    // telemetry: count every detour hit in the shared mapping (the log file
    // proved unreliable from the app's render thread). Interlocked so a
    // multi-swapchain app stays consistent.
    IWNHookCfg* cfgT = ensureCfg();
    if (cfgT && g_cfgWrite) InterlockedIncrement((volatile LONG*)&cfgT->reserved[0]);
    static bool firstCallLogged = false;
    if (!firstCallLogged) {
        firstCallLogged = true;
        hookLog("HookPresent invoked (first call)");
    }
    // PRESENT_TEST calls (our own probe) stop here: forwarding them would
    // race the app's real Present on the same swapchain and crash DXGI.
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    doWarpPass(sw);
    PFN_Present orig = origPresentFor(sw);
    if (!orig) return S_OK;   // call must come from a patched vtable; never happens
    return orig(sw, sync, flags);
}

// flip-model apps and dwm.exe may present through IDXGISwapChain1::Present1
// (slot 9 of the interface subobject vtable). Same warp body; the dirty-rect
// parameters are ignored (we redraw the full backbuffer anyway) and the call
// is forwarded to the original Present1.
static HRESULT WINAPI HookPresent1(IDXGISwapChain1* sw1, UINT sync, UINT flags,
                                   DXGI_PRESENT_PARAMETERS* pp) {
    (void)pp;
    IWNHookCfg* cfgT = ensureCfg();
    if (cfgT && g_cfgWrite) InterlockedIncrement((volatile LONG*)&cfgT->reserved[0]);
    static bool firstCallLogged = false;
    if (!firstCallLogged) {
        firstCallLogged = true;
        hookLog("HookPresent1 invoked (first call - flip-model path)");
    }
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    IDXGISwapChain* sw = sw1;   // same object, base interface
    doWarpPass(sw);
    PFN_Present1 orig1 = origPresent1For(sw);
    if (!orig1) return S_OK;
    return orig1(sw1, sync, flags, pp);
}

// ---------------------------------------------------------------- crash telemetry
// The app's render thread silently drops hookLog writes, so a crash inside
// Present would be invisible. The VEH publishes first-chance exception info
// through the shared mapping (reserved[1]=code, reserved[2]=tid) which the
// host can read via `GET hook` even if the process dies right after.
// The bare ExceptionAddress always lands in KERNELBASE!RaiseException for
// MSVC C++ throws, so we additionally decode the ThrowInfo (x64 EH v4 uses
// image-relative offsets) into the exception TYPE NAME and scan the stack
// for the throwing module - the type name alone usually fingers the cause.

// MSVC x64 EH v4 lite layouts - all cross-references are 32-bit RVAs
struct TiLite { DWORD attr; int unwind; int fwd; int ctaRva; };
struct CtaLite { int n; int rvas[1]; };
struct CtLite { DWORD props; int typeRva; int mdisp; int pdisp; int vdisp;
                int sizeOff; int copyFn; };
struct TdLite { void* vf; void* spare; char name[80]; };

static void logThrowType(PEXCEPTION_POINTERS ep) {
    __try {
        ULONG_PTR* ei = ep->ExceptionRecord->ExceptionInformation;
        if (ei[0] != 0x19930520u) { hookLog("throw: unknown magic"); return; }
        // verified on this machine: ei[1] is the ABSOLUTE address of the
        // ThrowInfo (it landed inside msvcprt.dll for a std::system_error)
        const void* tiAbs = (const void*)ei[1];
        void* dum = nullptr;
        void* ownerMb = RtlPcToFileHeader((PVOID)tiAbs, &dum);
        char buf2[96];
        wsprintfA(buf2, "throw module: %p (ti=%p)", ownerMb, tiAbs);
        hookLog(buf2);
        if (!ownerMb) { hookLog("throw: no base"); return; }
        const TiLite* ti = (const TiLite*)tiAbs;
        if (ti->ctaRva <= 0) { hookLog("throw: no catchable types"); return; }
        const CtaLite* cta = (const CtaLite*)((const BYTE*)ownerMb + ti->ctaRva);
        if (cta->n <= 0) { hookLog("throw: empty cta"); return; }
        const CtLite* ct = (const CtLite*)((const BYTE*)ownerMb + cta->rvas[0]);
        const TdLite* td = (const TdLite*)((const BYTE*)ownerMb + ct->typeRva);
        char buf[112];
        wsprintfA(buf, "throw type: %.70s", td->name);
        hookLog(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hookLog("throw type: parse fault");
    }
}

static LONG WINAPI excFilter(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xE06D7363 && code != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    IWNHookCfg* c = ensureCfg();
    if (c && g_cfgWrite) {
        // InterlockedCompareExchange keeps the FIRST occurrence - later
        // first-chance repeats (same throw unwinding) must not overwrite it
        InterlockedCompareExchange((volatile LONG*)&c->reserved[1], (LONG)code, 0);
        InterlockedCompareExchange((volatile LONG*)&c->reserved[2],
                                   (LONG)GetCurrentThreadId(), 0);
    }
    static volatile LONG reported = 0;
    if (InterlockedCompareExchange(&reported, 1, 0) == 0) {
        PVOID pc = ep->ExceptionRecord->ExceptionAddress;
        void* base = nullptr;
        void* modBase = RtlPcToFileHeader(pc, &base);
        DWORD off = modBase ? (DWORD)((uintptr_t)pc - (uintptr_t)modBase) : 0;
        char buf[192];
        wsprintfA(buf, "EXC code=0x%08X addr=%p mod=%p off=0x%X tid=%u",
                  code, pc, modBase, off, (unsigned)GetCurrentThreadId());
        hookLog(buf);
        if (code == 0xE06D7363) logThrowType(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- init thread
// Self-probes: create tiny swapchains of BOTH implementation families
// (legacy bitblt + flip-model) and patch their vtables. This is what makes
// the dwm.exe path work: dwm.exe's per-monitor swapchains already exist at
// injection time and will never be created again through the factory hooks.
// dxgi.dll vtables are shared per implementation class, so patching our
// probes' vtables detours the existing instances as well. The probes stay
// alive (8x8, never presented) so the watchdog can verify the object vptr.
static void createSelfProbes() {
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"STATIC", L"IWNprobe", WS_POPUP,
                              0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
    if (!hw) { hookLog("selfprobe: window failed"); return; }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev) {
        char buf[96];
        wsprintfA(buf, "selfprobe: D3D11CreateDevice failed hr=0x%08X", (unsigned)hr);
        hookLog(buf);
        return;
    }
    IDXGIDevice* dxdev = nullptr;
    IDXGIFactory2* fac = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)) && dxdev) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxdev->GetAdapter(&ad)) && ad) {
            if (FAILED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac)) || !fac)
                hookLog("selfprobe: factory not found");
            ad->Release();
        }
        dxdev->Release();
    }
    if (fac) {
        InterlockedExchange(&g_inSelfProbe, 1);
        // legacy bitblt probe (IDXGISwapChain class)
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = hw;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* sc = nullptr;
        if (SUCCEEDED(fac->CreateSwapChain(dev, &sd, &sc)) && sc) {
            patchSwapchainVt(sc);
            // keep alive for the watchdog; leak one static ref on purpose
            g_keep1 = sc;
            (void)g_keep1;
            hookLog("selfprobe: bitblt probe created");
        } else {
            hookLog("selfprobe: bitblt probe failed");
        }
        // flip-model probe (IDXGISwapChain1 class, the dwm.exe family)
        DXGI_SWAP_CHAIN_DESC1 d1{};
        d1.Width = 8; d1.Height = 8;
        d1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d1.SampleDesc.Count = 1;
        d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d1.BufferCount = 2;
        d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(fac->CreateSwapChainForHwnd(dev, hw, &d1, nullptr, nullptr, &sc1)) && sc1) {
            patchSwapchainVt(sc1);
            g_keep2 = sc1;
            (void)g_keep2;
            hookLog("selfprobe: flip probe created");
        } else {
            hookLog("selfprobe: flip probe failed");
        }
        // composition-model probe (a third implementation family; the dwm
        // desktop swapchains are not hwnd-bound, so this is the likeliest
        // class they use - and the one whose vtable was never seen before)
        DXGI_SWAP_CHAIN_DESC1 d2 = d1;
        d2.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        IDXGISwapChain1* sc2 = nullptr;
        if (SUCCEEDED(fac->CreateSwapChainForComposition(dev, &d2, nullptr, &sc2)) && sc2) {
            patchSwapchainVt(sc2);
            g_keep3 = sc2;
            (void)g_keep3;
            hookLog("selfprobe: composition probe created");
        } else {
            hookLog("selfprobe: composition probe failed");
        }
        InterlockedExchange(&g_inSelfProbe, 0);
        fac->Release();
    }
    if (ctx) ctx->Release();
    dev->Release();
    // window stays around (tiny, hidden); DestroyWindow from this thread is
    // fine but the swapchains reference it - keep it alive instead
}

// Probe present loop: the probes were previously "8x8, never presented", which
// meant the detour chain was never exercised in-process. Presenting them at
// 4 Hz gives pass a KNOWN baseline (probe traffic only). If pass grows beyond
// the probe cadence, real host-process swapchains are flowing through the
// hook; if pass == probe traffic only, the host bypasses dxgi entirely.
// Probes are private objects, so no race with the app's own presents.
static DWORD WINAPI probePresentThread(LPVOID) {
    static bool logged = false;
    for (;;) {
        Sleep(250);
        // Present() on IDXGISwapChain1 goes through the base-class slot 8
        HRESULT hr1 = g_keep1 ? g_keep1->Present(0, 0) : E_FAIL;
        HRESULT hr2 = g_keep2 ? g_keep2->Present(0, 0) : E_FAIL;
        HRESULT hr3 = g_keep3 ? g_keep3->Present(0, 0) : E_FAIL;
        if (!logged) {
            logged = true;
            char buf[96];
            wsprintfA(buf, "probe present loop: hr1=0x%08X hr2=0x%08X hr3=0x%08X",
                      (unsigned)hr1, (unsigned)hr2, (unsigned)hr3);
            hookLog(buf);
        }
    }
    return 0;
}

static DWORD WINAPI initThread(LPVOID) {
    // wait for dxgi to be loaded by the host process (poll 50 ms)
    for (int i = 0; i < 600; i++) {
        if (GetModuleHandleW(L"dxgi.dll")) break;
        Sleep(50);
    }
    {
        char buf[48];
        wsprintfA(buf, "init thread tid=%u", (unsigned)GetCurrentThreadId());
        hookLog(buf);
    }
    installFactoryHooks();
    createSelfProbes();
    // start the probe present loop - see probePresentThread for why
    HANDLE ppt = CreateThread(nullptr, 0, probePresentThread, nullptr, 0, nullptr);
    if (ppt) CloseHandle(ppt);
    // open the host mapping eagerly - not lazily inside Present - so a
    // mapping failure is visible in the log with a real error code
    if (ensureCfg()) {
        hookLog("cfg mapping ready at init");
    } else {
        char buf[96];
        wsprintfA(buf, "cfg mapping NOT available at init, gle=%u", GetLastError());
        hookLog(buf);
    }
    // one-shot dispatch probe: call the (hooked) slot directly 5 s in.
    // PRESENT_TEST never displays; proves the detour is live end-to-end.
    // NOTE: the lock guard MUST die here - a bare declaration would keep
    // g_vtMutex held for the rest of initThread, and the watchdog's own
    // lock_guard below would recursively lock the same std::mutex, which
    // throws system_error (0xE06D7363) and kills the process 2 s later.
    Sleep(5000);
    {
        std::lock_guard<std::mutex> lk(g_vtMutex);
        if (g_vtCount > 0 && g_vts[0].probe) {
            void** vt = g_vts[0].vt;
            if (*(void* volatile*)&vt[8] == (void*)&HookPresent) {
                hookLog("probe: calling hooked slot with PRESENT_TEST");
                ((PFN_Present)vt[8])(g_vts[0].probe, 0, DXGI_PRESENT_TEST);
            }
        }
    }
    // watchdog: re-apply the detour if something restored the original entry.
    // NOTE: no repeated live probe - calling Present from this thread races
    // the app's own Present on the same swapchain and crashes DXGI.
    // First cycle logs every step - the mystery crash lands exactly here
    // (2.0 s after the probe) and these lines pin the throwing statement.
    static int wdCycle = 0;
    while (true) {
        Sleep(2000);
        if (wdCycle == 0) hookLog("wd: awake");
        std::lock_guard<std::mutex> lk(g_vtMutex);
        if (wdCycle == 0) hookLog("wd: locked");
        if (wdCycle == 0) hookLog("watchdog cycle 1 enter");
        wdCycle++;
        for (int i = 0; i < g_vtCount; i++) {
            void** vt = g_vts[i].vt;
            // re-read the OBJECT's vptr: if dxgi swapped the vtable after
            // creation, the object no longer dispatches through our patch
            // (only entries with a live probe can be checked)
            void* objVptr = g_vts[i].probe ? *(void* volatile*)g_vts[i].probe : nullptr;
            static bool objVptrLogged = false;
            if (g_vts[i].probe && objVptr != vt) {
                char buf[128];
                wsprintfA(buf, "OBJECT VPTR CHANGED: obj=%p now=%p patched=%p",
                          (void*)g_vts[i].probe, objVptr, (void*)vt);
                hookLog(buf);
            } else if (!objVptrLogged) {
                objVptrLogged = true;
                char buf[160];
                wsprintfA(buf, "vptr check: obj=%p vt=%p slot8=%p hook=%p",
                          (void*)g_vts[i].probe, (void*)vt, vt[8], (void*)&HookPresent);
                hookLog(buf);
            }
            if (*(void* volatile*)&vt[8] != (void*)&HookPresent) {
                DWORD old{};
                if (VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old)) {
                    *(void* volatile*)&vt[8] = (void*)&HookPresent;
                    VirtualProtect(&vt[8], sizeof(void*), old, &old);
                    char buf[96];
                    wsprintfA(buf, "vtable reverted (vt=%p) - repatched", (void*)vt);
                    hookLog(buf);
                }
            }
            // Present1 slot (flip-model family)
            if (g_vts[i].hasP1 && g_vts[i].orig1 &&
                *(void* volatile*)&vt[9] != (void*)&HookPresent1) {
                DWORD old{};
                if (VirtualProtect(&vt[9], sizeof(void*), PAGE_READWRITE, &old)) {
                    *(void* volatile*)&vt[9] = (void*)&HookPresent1;
                    VirtualProtect(&vt[9], sizeof(void*), old, &old);
                    hookLog("Present1 slot reverted - repatched");
                }
            }
        }
    }
    return 0;
}

static DWORD g_initTid = 0;

// exported raw entry: the manual-map injector calls this directly, skipping
// the CRT startup stub (_DllMainCRTStartup) - a manual-mapped image has no
// loader TLS registration, and the stub touches it, which killed the target
// (round 28). Everything below uses plain Win32 only; with the dynamic CRT
// the runtime state was initialized when vcruntime140.dll itself was loaded,
// and our globals are all constexpr (no dynamic initializers).
typedef LONG (WINAPI *PFN_RtlAddFunctionTable)(void*, ULONG, ULONG_PTR);

extern "C" __declspec(dllexport) BOOL WINAPI HookEntry(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // counters first: they are the only channel that works when the file
        // logger cannot open anything in this process
        InterlockedIncrement((volatile LONG*)&g_entryHits);
        g_stage = 1;                                 // entered
        hookLog("he: enter");
        // single-instance guard: the host may map this DLL into the same
        // process more than once (SET hook=on auto-inject + explicit INJECT
        // DWM). Two mapped copies fight over the same dxgi vtables forever -
        // each copy's watchdog sees the other's trampoline in slot 8, treats
        // it as a revert and repatches, 0.5s forever. First copy wins.
        wchar_t instName[64];
        iwnHookInstanceMutexName(GetCurrentProcessId(), instName, 64);
        g_instMutex = CreateMutexW(nullptr, TRUE, instName);
        if (g_instMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            g_stage = 7;                             // duplicate copy, no-op
            hookLog("he: another instance already active in this process - no-op");
            return TRUE;
        }
        // register the exception table ourselves: manual-mapped images get no
        // loader unwind registration, and std::mutex paths below can unwind
        {
            auto dos = (IMAGE_DOS_HEADER*)mod;
            auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
            auto& edir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (edir.Size) {
                PFN_RtlAddFunctionTable aft = (PFN_RtlAddFunctionTable)
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAddFunctionTable");
                if (aft)
                    aft((void*)((BYTE*)mod + edir.VirtualAddress),
                        edir.Size / sizeof(RUNTIME_FUNCTION), (ULONG_PTR)mod);
            }
        }
        g_stage = 2;                                 // unwind table registered
        hookLog("he: unwound registered");
        DisableThreadLibraryCalls(mod);
        g_stage = 3;
        hookLog("he: dtl ok");
        AddVectoredExceptionHandler(1, &excFilter);
        g_stage = 4;
        hookLog("he: veh ok");
        char buf[300];
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wsprintfA(buf, "WarpHook.dll attached pid=%u exe=%ls", (unsigned)GetCurrentProcessId(), exe);
        hookLog(buf);
        // dwm.exe is the composition host: the dxgi Present path does not exist
        // there (dwm imports no swapchain API). Arm the desktop probe instead,
        // on its own thread - DwmProbeInstall waits up to 30s for dwmcore.dll
        // and HookEntry must return promptly for the manual-map handshake.
        const wchar_t* base = wcsrchr(exe, L'\\');
        if (base && _wcsicmp(base + 1, L"dwm.exe") == 0) {
            hookLog("he: dwm.exe detected -> arming desktop probe");
            g_selfMod = mod;
            HANDLE tp = CreateThread(nullptr, 0, probeThread, nullptr, 0, nullptr);
            if (tp) CloseHandle(tp);
        }
        HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, &g_initTid);
        g_stage = 5;                                 // init thread spawned
        hookLog("he: thread spawned");
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        hookLog("WarpHook.dll detached");
    }
    g_stage = 6;                                     // HookEntry finished
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID r) {
    return HookEntry(mod, reason, r);
}
