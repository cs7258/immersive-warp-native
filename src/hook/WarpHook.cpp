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
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <vector>
#include <cstring>
#include <mutex>
#include "hook_ipc.h"

// forward decls (vtable patchers reference them)
static HRESULT WINAPI HookPresent(IDXGISwapChain* sw, UINT sync, UINT flags);
static HRESULT WINAPI HookCreateSwapChain(IUnknown* fac, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
static HRESULT WINAPI HookCreateSwapChainForHwnd(IUnknown* fac, IUnknown* dev, HWND hwnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* d1,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd,
                                                 IDXGIOutput* ign, IDXGISwapChain1** out);

// ---------------------------------------------------------------- logging
static void hookLog(const char* line) {
    static std::mutex m;
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", path, MAX_PATH);
    if (!n) return;
    lstrcatA(path, "\\ImmersiveWarpNative");
    CreateDirectoryA(path, nullptr);
    lstrcatA(path, "\\hook.log");
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[512];
    int len = wsprintfA(buf, "[%02d:%02d:%02d.%03d] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    std::lock_guard<std::mutex> lk(m);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, nullptr, FILE_END);
        DWORD w; WriteFile(f, buf, (DWORD)len, &w, nullptr);
        CloseHandle(f);
    }
}

// ---------------------------------------------------------------- config
static IWNHookCfg* g_cfg = nullptr;          // mapped view, may stay null
static DWORD g_cfgLastTry = 0;               // GetTickCount pacing

static IWNHookCfg* ensureCfg() {
    if (g_cfg) return g_cfg;
    DWORD now = GetTickCount();
    if (now - g_cfgLastTry < 1000) return nullptr;   // retry at 1 Hz
    g_cfgLastTry = now;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, IWN_HOOK_MAPPING_NAME);
    if (!h) return nullptr;
    g_cfg = (IWNHookCfg*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(IWNHookCfg));
    if (g_cfg && (g_cfg->magic != IWN_HOOK_MAGIC || g_cfg->size != sizeof(IWNHookCfg))) {
        hookLog("cfg: magic/size mismatch, ignoring host mapping");
        UnmapViewOfFile(g_cfg); g_cfg = nullptr;
    }
    return g_cfg;
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
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET { return srcTex.Sample(sam, i.uv); }
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
    UINT w = 0, h = 0;          // back buffer size of cached resources
    UINT vertCount = 0;         // vertices in the cached VB
    LONG  gridN = 0;            // mesh resolution of cached VB
    float corner[4][2] = {};    // mesh of cached VB
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
    return true;
}

static bool buildMesh(HookCtx& c, const IWNHookCfg& cfg) {
    const LONG n = cfg.gridN >= 2 && cfg.gridN <= 64 ? cfg.gridN : 16;
    const int verts = (int)((n - 1) * (n - 1) * 6);
    struct V { float pos[3]; float uv[2]; };
    std::vector<V> data; data.reserve(verts);
    const float* c00 = cfg.corner[0];  // TL
    const float* c10 = cfg.corner[1];  // TR
    const float* c01 = cfg.corner[2];  // BL
    const float* c11 = cfg.corner[3];  // BR
    for (LONG y = 0; y < n - 1; y++) {
        for (LONG x = 0; x < n - 1; x++) {
            float u0 = (float)x / (n - 1), u1 = (float)(x + 1) / (n - 1);
            float v0 = (float)y / (n - 1), v1 = (float)(y + 1) / (n - 1);
            float us[4] = { u0, u1, u0, u1 }, vs_[4] = { v0, v0, v1, v1 };
            V quad[4];
            for (int i = 0; i < 4; i++) {
                float u = us[i], v = vs_[i];
                // bilinear blend of the 4 corner offsets == Coons patch with
                // straight edges (matches the host's per-corner warp model)
                float dx = (1 - u) * (1 - v) * c00[0] + u * (1 - v) * c10[0] +
                           (1 - u) * v * c01[0] + u * v * c11[0];
                float dy = (1 - u) * (1 - v) * c00[1] + u * (1 - v) * c10[1] +
                           (1 - u) * v * c01[1] + u * v * c11[1];
                quad[i].pos[0] = (2 * u - 1) + dx;
                quad[i].pos[1] = (1 - 2 * v) + dy;
                quad[i].pos[2] = 0;
                quad[i].uv[0] = u;
                quad[i].uv[1] = v;
            }
            // two triangles: (0,2,1) (1,2,3) with TL,TR,BL,BR ordering
            const int idx[6] = { 0, 2, 1, 1, 2, 3 };
            for (int i = 0; i < 6; i++) data.push_back(quad[idx[i]]);
        }
    }
    if (c.vb) { c.vb->Release(); c.vb = nullptr; }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)(data.size() * sizeof(V));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { data.data(), 0, 0 };
    if (FAILED(c.dev->CreateBuffer(&bd, &init, &c.vb))) { hookLog("vb create failed"); return false; }
    c.gridN = n;
    c.vertCount = (UINT)verts;
    memcpy(c.corner, cfg.corner, sizeof(c.corner));
    return true;
}

static bool ensureResources(HookCtx& c, ID3D11Texture2D* bb, const IWNHookCfg& cfg) {
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
    if (!c.gridN || memcmp(c.corner, cfg.corner, sizeof(c.corner)) != 0) {
        if (!buildMesh(c, cfg)) { releaseSlot(c); return false; }
    }
    return true;
}

// ---------------------------------------------------------------- hooks
typedef HRESULT(WINAPI* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* PFN_CreateSwapChain)(IUnknown*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(WINAPI* PFN_CreateSwapChainForHwnd)(IUnknown*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);

static PFN_CreateSwapChain         g_origCreate = nullptr;
static PFN_CreateSwapChainForHwnd  g_origCreateHwnd = nullptr;
static void** g_factoryVt = nullptr;      // patched vtable (process-wide)
static volatile LONG g_swapCount = 0;

// Bitblt and flip-model swapchains are distinct implementation classes with
// distinct vtables - patch each vtable once and remember its original Present.
struct VtEntry { void** vt; PFN_Present orig; };
static VtEntry g_vts[4];
static int g_vtCount = 0;
static std::mutex g_vtMutex;

static PFN_Present patchSwapchainVt(IDXGISwapChain* sw) {
    void** vt = *(void***)sw;
    std::lock_guard<std::mutex> lk(g_vtMutex);
    for (int i = 0; i < g_vtCount; i++)
        if (g_vts[i].vt == vt) return g_vts[i].orig;
    if (g_vtCount >= 4) return nullptr;
    DWORD old{};
    if (!VirtualProtect(&vt[13], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    PFN_Present orig = (PFN_Present)vt[13];
    vt[13] = (void*)&HookPresent;
    VirtualProtect(&vt[13], sizeof(void*), old, &old);
    g_vts[g_vtCount].vt = vt;
    g_vts[g_vtCount].orig = orig;
    g_vtCount++;
    hookLog("Present vtable patched (idx 13, new class)");
    return orig;
}

static void installFactoryHooks() {
    IDXGIFactory2* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&fac))) {
        hookLog("FATAL: CreateDXGIFactory1 failed");
        return;
    }
    g_factoryVt = *(void***)fac;
    DWORD old{};
    // idx 10 = IDXGIFactory::CreateSwapChain, idx 15 = IDXGIFactory2::CreateSwapChainForHwnd
    if (VirtualProtect(&g_factoryVt[10], sizeof(void*) * 6, PAGE_READWRITE, &old)) {
        g_origCreate = (PFN_CreateSwapChain)g_factoryVt[10];
        g_factoryVt[10] = (void*)&HookCreateSwapChain;
        g_origCreateHwnd = (PFN_CreateSwapChainForHwnd)g_factoryVt[15];
        g_factoryVt[15] = (void*)&HookCreateSwapChainForHwnd;
        VirtualProtect(&g_factoryVt[10], sizeof(void*) * 6, old, &old);
        hookLog("factory hooks installed (CreateSwapChain + CreateSwapChainForHwnd)");
    } else {
        hookLog("FATAL: VirtualProtect on factory vtable failed");
    }
    fac->Release();
}

static HRESULT WINAPI HookCreateSwapChain(IUnknown* fac, IUnknown* dev,
                                          DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = g_origCreate(fac, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) {
        patchSwapchainVt(*out);
        LONG c = InterlockedIncrement(&g_swapCount);
        char buf[96];
        wsprintfA(buf, "swapchain hooked via CreateSwapChain (#%d)", (int)c);
        hookLog(buf);
    }
    return hr;
}

static HRESULT WINAPI HookCreateSwapChainForHwnd(IUnknown* fac, IUnknown* dev, HWND hwnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* d1,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd,
                                                 IDXGIOutput* ign, IDXGISwapChain1** out) {
    HRESULT hr = g_origCreateHwnd(fac, dev, hwnd, d1, fd, ign, out);
    if (SUCCEEDED(hr) && out && *out) {
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
    for (int i = 0; i < g_vtCount; i++)
        if (g_vts[i].vt == vt) return g_vts[i].orig;
    return nullptr;
}

static HRESULT WINAPI HookPresent(IDXGISwapChain* sw, UINT sync, UINT flags) {
    PFN_Present orig = origPresentFor(sw);
    IWNHookCfg* cfg = ensureCfg();
    if (cfg && cfg->enabled && orig && sw) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            std::lock_guard<std::mutex> lk(g_ctxMutex);
            HookCtx* slot = nullptr;
            for (HookCtx& c : g_ctx) if (!c.bbKey || c.bbKey == (void*)bb) { slot = &c; break; }
            if (slot && ensureResources(*slot, bb, *cfg)) {
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
                c.ctx->PSSetSamplers(0, 1, &c.ss);
                ID3D11ShaderResourceView* srvp = c.srv;
                c.ctx->PSSetShaderResources(0, 1, &srvp);
                c.ctx->Draw(c.vertCount, 0);
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
    if (!orig) return S_OK;   // call must come from a patched vtable; never happens
    return orig(sw, sync, flags);
}

// ---------------------------------------------------------------- init thread
static DWORD WINAPI initThread(LPVOID) {
    // wait for dxgi to be loaded by the host process (poll 50 ms)
    for (int i = 0; i < 600; i++) {
        if (GetModuleHandleW(L"dxgi.dll")) break;
        Sleep(50);
    }
    installFactoryHooks();
    return 0;
}

static DWORD g_initTid = 0;

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        hookLog("WarpHook.dll attached");
        HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, &g_initTid);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        hookLog("WarpHook.dll detached");
    }
    return TRUE;
}
