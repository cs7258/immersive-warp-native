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
static int g_cfgTries = 0;

static IWNHookCfg* ensureCfg() {
    if (g_cfg) return g_cfg;
    DWORD now = GetTickCount();
    if (now - g_cfgLastTry < 1000) return nullptr;   // retry at 1 Hz
    g_cfgLastTry = now;
    g_cfgTries++;
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, IWN_HOOK_MAPPING_NAME);
    if (!h) {
        if (g_cfgTries == 10)
            hookLog("cfg: mapping not found after 10 tries (host not running or name mismatch)");
        return nullptr;
    }
    g_cfg = (IWNHookCfg*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IWNHookCfg));
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
static volatile LONG g_swapCount = 0;

// Bitblt and flip-model swapchains are distinct implementation classes with
// distinct vtables - patch each vtable once and remember its original Present.
struct VtEntry { void** vt; PFN_Present orig; IDXGISwapChain* probe; };
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
    if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    PFN_Present orig = (PFN_Present)vt[8];
    *(void* volatile*)&vt[8] = (void*)&HookPresent;
    VirtualProtect(&vt[8], sizeof(void*), old, &old);
    // volatile readback - the compiler must not fold this into the store
    void* now = *(void* volatile*)&vt[8];
    if (now != (void*)&HookPresent) {
        hookLog("Present patch VERIFY FAILED - readback differs");
        return orig;
    }
    char buf[96];
    wsprintfA(buf, "Present vtable patched (vt=%p)", (void*)vt);
    hookLog(buf);
    g_vts[g_vtCount].vt = vt;
    g_vts[g_vtCount].orig = orig;
    g_vts[g_vtCount].probe = sw;
    g_vtCount++;
    return orig;
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
    if (SUCCEEDED(hr) && out && *out) {
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
    // telemetry: count every detour hit in the shared mapping (the log file
    // proved unreliable from the app's render thread). Interlocked so a
    // multi-swapchain app stays consistent.
    IWNHookCfg* cfgT = ensureCfg();
    if (cfgT) InterlockedIncrement((volatile LONG*)&cfgT->reserved[0]);
    static bool firstCallLogged = false;
    if (!firstCallLogged) {
        firstCallLogged = true;
        hookLog("HookPresent invoked (first call)");
    }
    // PRESENT_TEST calls (our own probe) stop here: forwarding them would
    // race the app's real Present on the same swapchain and crash DXGI.
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    PFN_Present orig = origPresentFor(sw);
    IWNHookCfg* cfg = ensureCfg();
    if (cfg && cfg->enabled && orig && sw) {
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
    if (c) {
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
            void* objVptr = g_vts[i].probe ? *(void* volatile*)g_vts[i].probe : nullptr;
            static bool objVptrLogged = false;
            if (objVptr != vt) {
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
        }
    }
    return 0;
}

static DWORD g_initTid = 0;

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        AddVectoredExceptionHandler(1, &excFilter);
        char buf[300];
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wsprintfA(buf, "WarpHook.dll attached pid=%u exe=%ls", (unsigned)GetCurrentProcessId(), exe);
        hookLog(buf);
        HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, &g_initTid);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        hookLog("WarpHook.dll detached");
    }
    return TRUE;
}
