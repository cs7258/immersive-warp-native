// Immersive Warp Native - dwm composite-surface warp engine (Stage 1)
//
// Goal: the desktop visibly warps, while a screenshot still returns the
// unwarped rectangle. That is what the original product does, and it is what
// the user asked for - it is NOT a GPU-driver-level deformation.
//
// Mechanism (identical shape to the original's forcePreserveBB):
//     save clean copy -> paint warped grid over the surface -> submit -> restore
//
// All facts below are measured on this machine (Win10 18363, WARP composition):
//   * the composed desktop surface is 1366x768, DXGI_FORMAT_B8G8R8A8_UNORM (87),
//     BindFlags 0x28 = SHADER_RESOURCE | RENDER_TARGET, single sample
//     -> we can sample it AND render into it, no staging round-trip needed
//   * dwm unbinds its render target before submitting (OMGetRenderTargets is
//     always empty at ConfirmFrame), so we must save/restore every pipeline
//     state we touch - dwm will not do it for us
//   * shaders are compiled at runtime with D3DCompile (d3dcompiler_47.dll is
//     part of Windows); no build-time fxc dependency, same as the original
//
// STAGED SELF-DIAGNOSIS
// The only unknown left is "how late may we paint relative to the submit".
// Instead of guessing, the engine walks four stages on a timer after attach,
// logging each transition, so ONE injection tells us where the warp survives:
//   stage 1 (0..6s)    save + restore only      -> proves the copy is lossless
//   stage 2 (6..14s)   save + paint + restore   -> the original's exact shape
//   stage 3 (14..24s)  save + paint, restore one submit later
//   stage 4 (24s+)     freeze the clean baseline, never restore
// A stage that kills dwm is visible in the log because the next stage line
// never appears.

#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include "DwmWarp.h"

extern void iwnHookLog(const char* line);

// ---------------------------------------------------------------- tiny float fmt
// wsprintfA has no %f support, and linking the CRT float formatter into a
// manual-mapped DLL is not worth it for log lines.
static void f2a(char* b, float v) {
    int neg = (v < 0.0f);
    if (neg) v = -v;
    int ip = (int)v;
    int fp = (int)((v - (float)ip) * 1000.0f + 0.5f);
    if (fp >= 1000) { fp -= 1000; ip += 1; }
    wsprintfA(b, "%s%d.%03d", neg ? "-" : "", ip, fp);
}

// ---------------------------------------------------------------- state (POD only)
// NOTE: no function-local static objects with dynamic initialisation anywhere in
// this file - a manual-mapped image gets no loader TLS, so the thread-safe
// static guard would crash (round 28, measured).
#define IWN_GRID_N 17          // 17x17 vertices -> 16*16*2*3 = 1536 verts
#define IWN_MAX_VP 8
#define IWN_MAX_TGT 8          // dwm faces + swapchain wire buffers

struct IWNVtx { float x, y, u, v; };

static ID3D11Device*              g_dev = nullptr;      // owned ref (dwm's device)
static ID3D11DeviceContext*       g_ctx = nullptr;      // dwm's immediate context
static ID3D11Texture2D*           g_target = nullptr;   // first candidate (twin source)
static ID3D11Texture2D*           g_tgt[IWN_MAX_TGT];   // ALL screen-size RT candidates
static ID3D11RenderTargetView*    g_rtvA[IWN_MAX_TGT];  // one RTV per candidate
static int                        g_tgtN = 0;           // surfaces painted per frame
// copy mode: RTV creation on a private composition face can fail with
// E_INVALIDARG even with an explicit typed desc (16:33 run: SRV on the SAME
// face succeeds). CopyResource into these faces is proven to work (the round
// 65 dump read them back pixel-perfect). So: render the warp onto our OWN
// render target, then CopyResource it into the face.
static ID3D11Texture2D*           g_warpRT  = nullptr;  // own render target (copy mode)
static ID3D11RenderTargetView*    g_warpRTV = nullptr;
static BOOL                       g_copyMode[IWN_MAX_TGT];
static ID3D11Texture2D*           g_twin = nullptr;     // clean copy buffer
static ID3D11Texture2D*           g_stageTex = nullptr; // CPU-readable mirror (diagnostics)
static ID3D11RenderTargetView*    g_rtv = nullptr;      // kept for releaseAll compat (unused)
static ID3D11ShaderResourceView*  g_srv = nullptr;      // view onto g_twin
static ID3D11SamplerState*        g_smp = nullptr;
static ID3D11VertexShader*        g_vs = nullptr;
static ID3D11PixelShader*         g_ps = nullptr;
static ID3D11InputLayout*         g_il = nullptr;
static ID3D11Buffer*              g_vb = nullptr;
static ID3D11Buffer*              g_cb = nullptr;
static ID3D11RasterizerState*     g_rs = nullptr;
static ID3D11BlendState*          g_bs = nullptr;

static D3D11_TEXTURE2D_DESC       g_desc;               // g_target's description
static UINT                       g_rtW = 0, g_rtH = 0;
static UINT                       g_vertCount = 0;

static volatile LONG  g_ready = 0;      // 1 = resources live, paint path usable
static volatile LONG  g_stage = 0;      // 0 off, 1..4 staged rollout
static volatile LONG  g_frames = 0;
static volatile LONG  g_fail = 0;
static volatile LONG  g_draws = 0;
static volatile LONG  g_unpaintDone = 0;   // clean baseline saved / probe colours cleared
static volatile LONG  g_gridDirty = 1;     // vertex buffer needs a refill
static volatile LONG  g_proofWarp = 0;     // one readback after the first warped draw
static volatile LONG  g_proofFlip = 0;     // one readback at the deferred post-flip restore
static volatile LONG  g_flipCalls = 0;     // inner present calls seen (path A/B)
static volatile LONG  g_flipInspec = 0;    // one-time flip-object inspection done
static volatile LONG  g_flipDead = 0;      // 1 = hooks installed but never called
static DWORD          g_flipInstallTick = 0;
static volatile LONG  g_presntTried = 0;   // round 74: mode B entered once
static volatile LONG  g_scHooked = 0;      // round 76: wire swapchain Present hooked
static volatile LONG  g_scCalls = 0;       // wire Present calls seen

// round 78: dwm's own present override. Round 77 fingerprinted the wire object
// as CDXGISwapChainDWMLegacy and mapped slot54 = dxgi+0x6BAA0 Present - the
// method PresentInternal's trampoline actually calls (COM slot 8: zero calls).
// That is the last paint point before the back->front blt.
typedef HRESULT (STDMETHODCALLTYPE* PFN_PRES10)(void*, void*, void*, void*, void*,
                                                void*, void*, void*, void*, void*);
static PFN_PRES10     g_dwmPresentTramp = nullptr; // 0x6BAA0 trampoline
static volatile LONG  g_dwmPresHooked = 0;  // 0x6BAA0 hook installed (paint owner)
static volatile LONG  g_dwmPresCalls = 0;   // calls seen through the paint hook
static int            g_dwmPresSlot = -1;
static PFN_PRES10     g_pdwmObsTramp = nullptr;    // 0x1AE30 observation trampoline
static volatile LONG  g_pdwmObsHooked = 0;
static volatile LONG  g_pdwmObsCalls = 0;
static int            g_pdwmObsSlot = -1;

// round 75: wire-buffer registration (defined below IWN_FlipInspect, also used
// by DwmWarpAttachTargets to survive probe re-attach cycles)
static void applyWireFromSC(IDXGISwapChain* sc);
static void installSCPresentHook();        // round 76 (defined near PDW hook)
static void* patchVtableSlot(void* obj, int slot, void* hook, const char* tag);
// round 78: dwm-legacy present wrappers (defined before IWN_FlipInspect)
static HRESULT STDMETHODCALLTYPE IWN_DWMPresentWrap(void* self, void* a2, void* a3, void* a4, void* a5,
                                                    void* a6, void* a7, void* a8, void* a9, void* a10);
static HRESULT STDMETHODCALLTYPE IWN_LegacyObsWrap(void* self, void* a2, void* a3, void* a4, void* a5,
                                                   void* a6, void* a7, void* a8, void* a9, void* a10);
static IDXGISwapChain* g_flipSC = nullptr; // dwm's own presenting swapchain (AddRef'd)
static int            g_frontTgt = -1;     // wire face named by dwm's present object
static ID3D11Texture2D* g_markerTex = nullptr; // 8x8 per-face wire marker source
static DWORD          g_t0 = 0;
static LONG           g_lastLoggedStage = -1;

// ---- round 72: clean-face dump + PrtSc comparison ---------------------------
// Painting fully suppressed in a 30s quiet window (engine el 30..52s) so every
// registered face holds dwm's OWN composed frame. Three dump rounds (el 32/42/
// 52s) write each face to C:\Windows\Temp\iwnf<t>_<r>.raw; the host pairs each
// dump with the CopyFromScreen shot nearest in time (PNG mtime). The face whose
// pixels match the shot IS the composed-desktop surface - and LBIDX front/back
// is logged with every round to bind dwm's own answer to the physical faces.
static volatile LONG  g_quiet = 0;      // 1 = all painting suppressed
static volatile LONG  g_dwin = 0;       // 0 none, 1 window active, 2 done
static volatile LONG  g_lbFrontLast = -1; // updated every present by the wrapper
static volatile LONG  g_lbBackLast  = -1;

// warp corners as DESTINATION positions (uv fractions of the surface): TL, TR,
// BL, BR. Default = visible keystone: the drawn desktop becomes a trapezoid
// (top edge pulled toward the middle), the rest of the surface is cleared to
// black. uv always spans the full source - nothing is zoomed.
static float g_cTLu = 0.15f, g_cTLv = 0.02f;
static float g_cTRu = 0.85f, g_cTRv = 0.02f;
static float g_cBLu = 0.00f, g_cBLv = 1.00f;
static float g_cBRu = 1.00f, g_cBRv = 1.00f;

// blend band widths in uv fraction: L, R, T, B (0 = blending off)
static float g_fadeL = 0.0f, g_fadeR = 0.0f, g_fadeT = 0.0f, g_fadeB = 0.0f;
static float g_gain = 1.0f, g_gamma = 1.0f;

// ---------------------------------------------------------------- shaders
// VS is a pure pass-through: the warp lives in the CPU-generated grid (same
// split the original uses - the GPU only rasterises an already-deformed mesh).
// PS does bilinear sample + optional edge blend bands.
static const char* kSrc =
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VSOut { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(VSIn i) {\n"
    "  VSOut o; o.p = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o;\n"
    "}\n"
    "Texture2D    srcTex : register(t0);\n"
    "SamplerState srcSmp : register(s0);\n"
    "cbuffer WarpCB : register(b0) { float4 fade; float4 ctl; };\n"
    "float4 PSMain(VSOut i) : SV_Target {\n"
    "  float4 c = srcTex.Sample(srcSmp, i.uv);\n"
    "  float wsum = fade.x + fade.y + fade.z + fade.w;\n"
    "  if (wsum > 0.0001) {\n"
    "    float l = saturate(i.uv.x / max(fade.x, 1e-5));\n"
    "    float r = saturate((1.0 - i.uv.x) / max(fade.y, 1e-5));\n"
    "    float t = saturate(i.uv.y / max(fade.z, 1e-5));\n"
    "    float b = saturate((1.0 - i.uv.y) / max(fade.w, 1e-5));\n"
    "    float f = min(min(l, r), min(t, b));\n"
    "    f = pow(f, max(ctl.y, 0.01));\n"
    "    c.rgb *= f * ctl.x;\n"
    "  }\n"
    "  return c;\n"
    "}\n";

// ---------------------------------------------------------------- state save/restore
// dwm is a black box: whatever it had bound before we run must be exactly what
// it has afterwards. We always run at the submit boundary, which is also where
// dwm has already unbound its own RT, but nothing guarantees it stays that way
// across Windows builds - so save everything we touch.
struct IWNState {
    ID3D11RenderTargetView*   rtv;
    ID3D11DepthStencilView*   dsv;
    UINT                      nvp;
    D3D11_VIEWPORT            vp[IWN_MAX_VP];
    ID3D11VertexShader*       vs;
    ID3D11PixelShader*        ps;
    ID3D11InputLayout*        il;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11Buffer*             vb;
    UINT                      stride;
    UINT                      offset;
    ID3D11ShaderResourceView* srv;
    ID3D11SamplerState*       smp;
    ID3D11Buffer*             cb;
    ID3D11RasterizerState*    rs;
    ID3D11BlendState*         bs;
    FLOAT                     bf[4];
    UINT                      mask;
    ID3D11DepthStencilState*  dss;
    UINT                      stencil;
};

static void stateSave(IWNState* s) {
    if (!s || !g_ctx) return;
    ZeroMemory(s, sizeof(*s));
    s->nvp = IWN_MAX_VP;
    __try {
        g_ctx->OMGetRenderTargets(1, &s->rtv, &s->dsv);
        g_ctx->RSGetViewports(&s->nvp, s->vp);
        g_ctx->VSGetShader(&s->vs, nullptr, nullptr);
        g_ctx->PSGetShader(&s->ps, nullptr, nullptr);
        g_ctx->IAGetInputLayout(&s->il);
        g_ctx->IAGetPrimitiveTopology(&s->topo);
        g_ctx->IAGetVertexBuffers(0, 1, &s->vb, &s->stride, &s->offset);
        g_ctx->PSGetShaderResources(0, 1, &s->srv);
        g_ctx->PSGetSamplers(0, 1, &s->smp);
        g_ctx->PSGetConstantBuffers(0, 1, &s->cb);
        g_ctx->RSGetState(&s->rs);
        g_ctx->OMGetBlendState(&s->bs, s->bf, &s->mask);
        g_ctx->OMGetDepthStencilState(&s->dss, &s->stencil);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("dwm: warp stateSave EXCEPTION");
    }
}

static void stateRestore(IWNState* s) {
    if (!s || !g_ctx) return;
    __try {
        if (s->rtv) g_ctx->OMSetRenderTargets(1, &s->rtv, s->dsv);
        else        g_ctx->OMSetRenderTargets(0, nullptr, s->dsv);
        g_ctx->RSSetViewports(s->nvp <= IWN_MAX_VP ? s->nvp : IWN_MAX_VP, s->vp);
        g_ctx->VSSetShader(s->vs, nullptr, 0);
        g_ctx->PSSetShader(s->ps, nullptr, 0);
        g_ctx->IASetInputLayout(s->il);
        g_ctx->IASetPrimitiveTopology(s->topo);
        g_ctx->IASetVertexBuffers(0, 1, &s->vb, &s->stride, &s->offset);
        g_ctx->PSSetShaderResources(0, 1, &s->srv);
        g_ctx->PSSetSamplers(0, 1, &s->smp);
        g_ctx->PSSetConstantBuffers(0, 1, &s->cb);
        g_ctx->RSSetState(s->rs);
        g_ctx->OMSetBlendState(s->bs, s->bf, s->mask);
        g_ctx->OMSetDepthStencilState(s->dss, s->stencil);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("dwm: warp stateRestore EXCEPTION");
    }
    if (s->rtv) s->rtv->Release();
    if (s->dsv) s->dsv->Release();
    if (s->vs)  s->vs->Release();
    if (s->ps)  s->ps->Release();
    if (s->il)  s->il->Release();
    if (s->vb)  s->vb->Release();
    if (s->srv) s->srv->Release();
    if (s->smp) s->smp->Release();
    if (s->cb)  s->cb->Release();
    if (s->rs)  s->rs->Release();
    if (s->bs)  s->bs->Release();
    if (s->dss) s->dss->Release();
    ZeroMemory(s, sizeof(*s));
}

// ---------------------------------------------------------------- grid
// dest position = full screen in NDC; source uv = bilinear blend of the four
// corner uv values. Painting the whole surface means no uncovered border, so a
// wrong restore shows up as a wrong *picture*, never as a black frame.
static bool buildGrid() {
    const int n = IWN_GRID_N;
    const int quads = (n - 1) * (n - 1);
    const int verts = quads * 6;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(verts * sizeof(IWNVtx));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!g_dev) return false;
    if (g_vb) { g_vb->Release(); g_vb = nullptr; }
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_vb)) || !g_vb) {
        iwnHookLog("dwm: warp VB create failed");
        return false;
    }
    g_vertCount = (UINT)verts;
    {
        char b[160];
        wsprintfA(b, "dwm: warp grid %dx%d verts=%u vbBytes=%u",
                  n, n, g_vertCount, (unsigned)bd.ByteWidth);
        iwnHookLog(b);
    }
    return true;
}

// Rewrite the dynamic vertex buffer from the current corner/fade parameters.
static void fillGrid() {
    if (!g_ctx || !g_vb) return;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return;
    IWNVtx* out = (IWNVtx*)m.pData;
    const int n = IWN_GRID_N;
    for (int gy = 0; gy < n - 1; gy++) {
        for (int gx = 0; gx < n - 1; gx++) {
            // four grid corners of this quad, ordered TL TR BL BR
            int cx[4] = { gx, gx + 1, gx,     gx + 1 };
            int cy[4] = { gy, gy,     gy + 1, gy + 1 };
            IWNVtx q[4];
            for (int k = 0; k < 4; k++) {
                float u = (float)cx[k] / (float)(n - 1);
                float v = (float)cy[k] / (float)(n - 1);
                // The four corner params are DESTINATION positions (exactly how
                // the original's mesh editor works and how the operator thinks:
                // "pull the top corners toward the middle -> trapezoid"). uv
                // always spans the full source, so nothing is zoomed - the
                // desktop is compressed INTO the trapezoid instead.
                float du = (1 - u) * (1 - v) * g_cTLu + u * (1 - v) * g_cTRu
                         + (1 - u) * v * g_cBLu + u * v * g_cBRu;
                float dv = (1 - u) * (1 - v) * g_cTLv + u * (1 - v) * g_cTRv
                         + (1 - u) * v * g_cBLv + u * v * g_cBRv;
                q[k].x = du * 2.0f - 1.0f;      // dest: keystone quad in NDC
                q[k].y = 1.0f - dv * 2.0f;
                q[k].u = u;                     // src: full-frame uv
                q[k].v = v;
            }
            // triangle order matches D3D's default (clockwise = front)
            *out++ = q[0]; *out++ = q[1]; *out++ = q[2];
            *out++ = q[1]; *out++ = q[3]; *out++ = q[2];
        }
    }
    g_ctx->Unmap(g_vb, 0);
}

static void fillCb() {
    if (!g_ctx || !g_cb) return;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return;
    float* f = (float*)m.pData;
    f[0] = g_fadeL; f[1] = g_fadeR; f[2] = g_fadeT; f[3] = g_fadeB;   // fade
    f[4] = g_gain;  f[5] = g_gamma; f[6] = 0.0f;    f[7] = 0.0f;      // ctl
    g_ctx->Unmap(g_cb, 0);
}

// ---------------------------------------------------------------- resource build
static bool buildShaders() {
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT h1 = D3DCompile(kSrc, lstrlenA(kSrc), "iwn_warp_vs", nullptr, nullptr,
                            "VSMain", "vs_5_0", 0, 0, &vsb, &err);
    if (err) { iwnHookLog((const char*)err->GetBufferPointer()); err->Release(); err = nullptr; }
    if (FAILED(h1) || !vsb) {
        char b[128];
        wsprintfA(b, "dwm: warp VS compile hr=0x%08X", (unsigned)h1);
        iwnHookLog(b);
        return false;
    }
    HRESULT h2 = D3DCompile(kSrc, lstrlenA(kSrc), "iwn_warp_ps", nullptr, nullptr,
                            "PSMain", "ps_5_0", 0, 0, &psb, &err);
    if (err) { iwnHookLog((const char*)err->GetBufferPointer()); err->Release(); err = nullptr; }
    if (FAILED(h2) || !psb) {
        char b[128];
        wsprintfA(b, "dwm: warp PS compile hr=0x%08X", (unsigned)h2);
        iwnHookLog(b);
        vsb->Release();
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC el[2] = {};
    el[0].SemanticName = "POSITION";
    el[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    el[0].InputSlot = 0;
    el[0].AlignedByteOffset = 0;
    el[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    el[1].SemanticName = "TEXCOORD";
    el[1].SemanticIndex = 0;
    el[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    el[1].InputSlot = 0;
    el[1].AlignedByteOffset = 8;
    el[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    HRESULT h3 = g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    HRESULT h4 = g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    HRESULT h5 = g_dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il);
    vsb->Release();
    psb->Release();
    {
        char b[192];
        wsprintfA(b, "dwm: warp shaders vs=0x%08X ps=0x%08X il=0x%08X",
                  (unsigned)h3, (unsigned)h4, (unsigned)h5);
        iwnHookLog(b);
    }
    return SUCCEEDED(h3) && SUCCEEDED(h4) && SUCCEEDED(h5);
}

static void releaseAll() {
    if (g_vb) { g_vb->Release(); g_vb = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_il) { g_il->Release(); g_il = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_vs) { g_vs->Release(); g_vs = nullptr; }
    if (g_smp) { g_smp->Release(); g_smp = nullptr; }
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    for (int i = 0; i < g_tgtN; i++) {
        if (g_rtvA[i]) { g_rtvA[i]->Release(); g_rtvA[i] = nullptr; }
        if (g_tgt[i])  { g_tgt[i]->Release();  g_tgt[i]  = nullptr; }
    }
    g_tgtN = 0;
    if (g_twin) { g_twin->Release(); g_twin = nullptr; }
    if (g_stageTex) { g_stageTex->Release(); g_stageTex = nullptr; }
    if (g_warpRTV) { g_warpRTV->Release(); g_warpRTV = nullptr; }
    if (g_warpRT)  { g_warpRT->Release();  g_warpRT  = nullptr; }
    ZeroMemory(g_copyMode, sizeof(g_copyMode));
    if (g_markerTex) { g_markerTex->Release(); g_markerTex = nullptr; }
    if (g_flipSC)   { g_flipSC->Release();   g_flipSC   = nullptr; }
    if (g_rs) { g_rs->Release(); g_rs = nullptr; }
    if (g_bs) { g_bs->Release(); g_bs = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    // g_target is a borrowed alias of g_tgt[0] - its ref is owned by g_tgt[0]
    g_target = nullptr;
}

// Own render target for copy-mode faces. Created lazily on first need; kept
static void installCopyObserver();   // fwd: defined in the paint section below
static void cpyObserverHeartbeat();  // round 79: patch-alive + per-slot counters
// Own render target for copy-mode faces. Created lazily on first need; kept
// across re-attaches (dims never change). If THIS RTV also fails, the device
// genuinely lacks RT support for the surface format and copy mode is
// impossible - the log line tells us which world we are in.
static BOOL ensureWarpRT() {
    char b[192];
    if (g_warpRTV) return TRUE;
    if (!g_dev) return FALSE;
    D3D11_TEXTURE2D_DESC td = g_desc;
    td.BindFlags      = D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;
    td.MiscFlags      = 0;
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    HRESULT hr = g_dev->CreateTexture2D(&td, nullptr, &g_warpRT);
    if (FAILED(hr) || !g_warpRT) {
        wsprintfA(b, "dwm: warp copy-mode RT texture failed hr=0x%08X", (unsigned)hr);
        iwnHookLog(b);
        return FALSE;
    }
    hr = g_dev->CreateRenderTargetView(g_warpRT, nullptr, &g_warpRTV);
    if (FAILED(hr) || !g_warpRTV) {
        wsprintfA(b, "dwm: warp copy-mode RT VIEW failed hr=0x%08X (no RT support for fmt=%u on this device)",
                  (unsigned)hr, (unsigned)td.Format);
        iwnHookLog(b);
        g_warpRT->Release();
        g_warpRT = nullptr;
        return FALSE;
    }
    iwnHookLog("dwm: warp copy-mode RT ready");
    return TRUE;
}

// ---- round 70: device discipline --------------------------------------------
// The 03:25 and 03:44 dwm deaths were both DXGI EFRE device-removed crashes,
// each landing within 20ms of the first capture attempt, with EVERY registered
// target in copy mode (RTV refused) - i.e. textures our RTV/QI device does not
// own. Cross-device CopyResource / CopySubresourceRegion is undefined behavior
// and the prime suspect for the removal; the 17:00 round survived it only
// because the original payload's resident device topology differed. Rule: any
// texture not owned by g_dev is logged once and skipped everywhere.
static BOOL g_tgtDevOk[IWN_MAX_TGT];          // POD zero-init

static BOOL texOnOurDevice(ID3D11Texture2D* t, int slot) {
    ID3D11Device* d = nullptr;
    if (t) t->GetDevice(&d);
    BOOL ok = (d == g_dev && d != nullptr);
    char b[160];
    wsprintfA(b, "dwm: DEVCHK tgt%d tex=%p dev=%p g_dev=%p -> %s", slot, (void*)t,
              (void*)d, (void*)g_dev, ok ? "SAME" : "CROSS-SKIP");
    iwnHookLog(b);
    if (d) d->Release();
    return ok;
}

static void logDeviceRemoved(const char* where) {
    static volatile LONG s_rmLogged = 0;      // POD
    if (!g_dev || InterlockedCompareExchange(&s_rmLogged, 1, 0) != 0) return;
    HRESULT rr = g_dev->GetDeviceRemovedReason();
    char b[128];
    wsprintfA(b, "dwm: DEVICE REMOVED REASON at %s = 0x%08X", where, (unsigned)rr);
    iwnHookLog(b);
}

bool DwmWarpAttachTargets(const IWNWarpTarget* list, int n) {
    if (!list || n <= 0) return false;
    char b[256];

    if (g_ready) {
        // Re-attach while running (periodic rescan): rebuild the RTV set only.
        // Twin/SRV/shaders stay alive; the frozen stage-4 baseline is preserved.
        for (int i = 0; i < g_tgtN; i++) {
            if (g_rtvA[i]) { g_rtvA[i]->Release(); g_rtvA[i] = nullptr; }
            if (g_tgt[i])  { g_tgt[i]->Release();  g_tgt[i]  = nullptr; }
        }
        g_tgtN = 0;
        ZeroMemory(g_copyMode, sizeof(g_copyMode));
        ZeroMemory(g_tgtDevOk, sizeof(g_tgtDevOk));
        for (int i = 0; i < n && g_tgtN < IWN_MAX_TGT; i++) {
            if (!list[i].tex) continue;
            if (!(list[i].desc.BindFlags & D3D11_BIND_RENDER_TARGET)) continue;
            if (!texOnOurDevice(list[i].tex, g_tgtN)) continue;   // round 70
            ID3D11RenderTargetView* rv = nullptr;
            HRESULT hr2 = g_dev->CreateRenderTargetView(list[i].tex, nullptr, &rv);
            if ((FAILED(hr2) || !rv) && ensureWarpRT()) {
                // copy-mode fallback: paint via own RT + CopyResource
                g_copyMode[g_tgtN] = TRUE;
                g_tgtDevOk[g_tgtN] = TRUE;
                list[i].tex->AddRef();
                g_rtvA[g_tgtN] = nullptr;
                g_tgt[g_tgtN]  = list[i].tex;
                g_tgtN++;
                continue;
            }
            if (FAILED(hr2) || !rv) continue;
            list[i].tex->AddRef();
            g_rtvA[g_tgtN] = rv;
            g_tgt[g_tgtN]  = list[i].tex;
            g_tgtN++;
        }
        if (g_tgtN > 0) {
            g_target = g_tgt[0];
            wsprintfA(b, "dwm: warp re-attach surfaces=%d (twin/frozen baseline kept)", g_tgtN);
            iwnHookLog(b);
            return true;
        }
        iwnHookLog("dwm: warp re-attach found no RT surface");
        return false;
    }

    const D3D11_TEXTURE2D_DESC& d = list[0].desc;
    if (d.SampleDesc.Count != 1) {
        wsprintfA(b, "dwm: warp refuses MSAA surface (samp=%u) - needs Resolve path",
                  (unsigned)d.SampleDesc.Count);
        iwnHookLog(b);
        return false;
    }
    if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET) || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        wsprintfA(b, "dwm: warp refuses surface bind=0x%X (need RT|SRV)", (unsigned)d.BindFlags);
        iwnHookLog(b);
        return false;
    }

    g_desc = d;
    g_rtW = d.Width;
    g_rtH = d.Height;

    __try { list[0].tex->GetDevice(&g_dev); } __except (EXCEPTION_EXECUTE_HANDLER) { g_dev = nullptr; }
    if (!g_dev) { iwnHookLog("dwm: warp no device"); releaseAll(); return false; }
    __try { g_dev->GetImmediateContext(&g_ctx); } __except (EXCEPTION_EXECUTE_HANDLER) { g_ctx = nullptr; }
    if (!g_ctx) { iwnHookLog("dwm: warp no immediate context"); releaseAll(); return false; }

    HRESULT hr;
    // twin: the clean copy. Same size/format, single mip, no MSAA.
    D3D11_TEXTURE2D_DESC td = d;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MipLevels = 1;
    td.ArraySize = 1;
    hr = g_dev->CreateTexture2D(&td, nullptr, &g_twin);
    wsprintfA(b, "dwm: warp twin %ux%u fmt=%u hr=0x%08X",
              (unsigned)td.Width, (unsigned)td.Height, (unsigned)td.Format, (unsigned)hr);
    iwnHookLog(b);
    if (FAILED(hr) || !g_twin) { releaseAll(); return false; }

    // CPU-readable mirror. Every "nothing happened on screen" so far had two
    // indistinguishable causes: the draw never landed, or it landed on a surface
    // that is not the one being scanned out. One readback tells them apart
    // without asking the operator to describe what he sees.
    {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.BindFlags = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        HRESULT hsr = g_dev->CreateTexture2D(&sd, nullptr, &g_stageTex);
        wsprintfA(b, "dwm: warp staging %ux%u hr=0x%08X", (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)hsr);
        iwnHookLog(b);
    }

    // One RTV per candidate surface. The 20:22 run painted a single
    // (proof-carrying) surface and the screen showed nothing, so now every
    // screen-size RT|SRV surface in dwm gets the same paint - if dwm rotates
    // composition buffers, at least one of them is the one on the wire.
    // 16:18 run: the private composition face (misc=0) fails RTV with
    // E_INVALIDARG when pDesc=nullptr while the SHARED face succeeds - the
    // classic typeless-resource symptom. Retry with an explicit typed desc.
    static BOOL s_rtvDiagDone = FALSE;
    ZeroMemory(g_copyMode, sizeof(g_copyMode));
    ZeroMemory(g_tgtDevOk, sizeof(g_tgtDevOk));
    for (int i = 0; i < n && g_tgtN < IWN_MAX_TGT; i++) {
        if (!list[i].tex) continue;
        if (!(list[i].desc.BindFlags & D3D11_BIND_RENDER_TARGET)) continue;
        if (list[i].desc.SampleDesc.Count != 1) continue;
        if (!texOnOurDevice(list[i].tex, i)) continue;            // round 70
        ID3D11RenderTargetView* rv = nullptr;
        HRESULT hr2 = g_dev->CreateRenderTargetView(list[i].tex, nullptr, &rv);
        if (FAILED(hr2) || !rv) {
            // one-time diagnosis on the first failure + explicit-format retry
            if (!s_rtvDiagDone) {
                s_rtvDiagDone = TRUE;
                D3D11_TEXTURE2D_DESC fresh = {};
                __try { list[i].tex->GetDesc(&fresh); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                ID3D11ShaderResourceView* srvT = nullptr;
                D3D11_SHADER_RESOURCE_VIEW_DESC sdT = {};
                sdT.Format = fresh.Format;
                sdT.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sdT.Texture2D.MostDetailedMip = 0;
                sdT.Texture2D.MipLevels = 1;
                HRESULT hrS = g_dev->CreateShaderResourceView(list[i].tex, &sdT, &srvT);
                UINT fsup = 0;
                HRESULT hrF = g_dev->CheckFormatSupport((DXGI_FORMAT)fresh.Format, &fsup);
                IDXGIDevice* dxgiD = nullptr;
                char adesc[64] = "?";
                if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiD)) && dxgiD) {
                    IDXGIAdapter* adp = nullptr;
                    if (SUCCEEDED(dxgiD->GetAdapter(&adp)) && adp) {
                        DXGI_ADAPTER_DESC ad = {};
                        if (SUCCEEDED(adp->GetDesc(&ad))) {
                            // Description may be any locale - keep it ASCII-safe
                            char t[64]; int k = 0;
                            for (; k < 62 && ad.Description[k]; k++) {
                                wchar_t c = ad.Description[k];
                                t[k] = (c >= 32 && c < 127) ? (char)c : '?';
                            }
                            t[k] = 0;
                            lstrcpynA(adesc, t, 64);
                        }
                        adp->Release();
                    }
                    dxgiD->Release();
                }
                wsprintfA(b, "dwm: warp RTV diag fresh %ux%u fmt=%u bind=0x%X misc=0x%X arr=%u srv(explicit)=0x%08X fmtsup hr=0x%08X flags=0x%X adpt=%s",
                          (unsigned)fresh.Width, (unsigned)fresh.Height, (unsigned)fresh.Format,
                          (unsigned)fresh.BindFlags, (unsigned)fresh.MiscFlags,
                          (unsigned)fresh.ArraySize, (unsigned)hrS,
                          (unsigned)hrF, fsup, adesc);
                iwnHookLog(b);
                if (srvT) srvT->Release();
            }
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = list[i].desc.Format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Texture2D.MipSlice = 0;
            hr2 = g_dev->CreateRenderTargetView(list[i].tex, &rd, &rv);
            if (FAILED(hr2) || !rv) {
                // copy-mode fallback: our own RT + CopyResource (proven path)
                if (ensureWarpRT()) {
                    g_copyMode[g_tgtN] = TRUE;
                    g_tgtDevOk[g_tgtN] = TRUE;
                    list[i].tex->AddRef();
                    g_rtvA[g_tgtN] = nullptr;
                    g_tgt[g_tgtN]  = list[i].tex;
                    g_tgtN++;
                    wsprintfA(b, "dwm: warp tgt[%d] registered via COPY MODE (RTV refused)", i);
                    iwnHookLog(b);
                    continue;
                }
                wsprintfA(b, "dwm: warp RTV[%d] hr=0x%08X (skipped, explicit retry also failed)",
                          i, (unsigned)hr2);
                iwnHookLog(b);
                continue;
            }
            wsprintfA(b, "dwm: warp RTV[%d] OK via explicit desc", i);
            iwnHookLog(b);
        }
        list[i].tex->AddRef();
        g_rtvA[g_tgtN] = rv;
        g_tgt[g_tgtN]  = list[i].tex;
        g_tgtDevOk[g_tgtN] = TRUE;
        g_tgtN++;
    }
    wsprintfA(b, "dwm: warp RTV-on-composite n=%d", g_tgtN);
    iwnHookLog(b);
    if (g_tgtN == 0) { releaseAll(); return false; }
    g_target = g_tgt[0];      // borrowed ref: twin source + save/restore subject

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    hr = g_dev->CreateShaderResourceView(g_twin, &sd, &g_srv);
    if (FAILED(hr) || !g_srv) {
        wsprintfA(b, "dwm: warp SRV hr=0x%08X", (unsigned)hr);
        iwnHookLog(b);
        releaseAll();
        return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_dev->CreateSamplerState(&smp, &g_smp);
    if (FAILED(hr) || !g_smp) { iwnHookLog("dwm: warp sampler failed"); releaseAll(); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 32;                       // two float4s
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dev->CreateBuffer(&cbd, nullptr, &g_cb);
    if (FAILED(hr) || !g_cb) { iwnHookLog("dwm: warp cbuffer failed"); releaseAll(); return false; }

    // cull none: we generate the winding ourselves, but a NULL rasteriser state
    // would inherit dwm's cull mode and silently drop half the grid.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = FALSE;
    hr = g_dev->CreateRasterizerState(&rd, &g_rs);
    if (FAILED(hr) || !g_rs) { iwnHookLog("dwm: warp rasterizer failed"); releaseAll(); return false; }

    // opaque write: dwm's own blend state may be premultiplied-alpha, which would
    // darken everything we paint.
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_dev->CreateBlendState(&bld, &g_bs);
    if (FAILED(hr) || !g_bs) { iwnHookLog("dwm: warp blend failed"); releaseAll(); return false; }

    if (!buildShaders()) { releaseAll(); return false; }
    if (!buildGrid())    { releaseAll(); return false; }
    fillCb();

    // Baseline copy: stage 4 paints from a frozen clean frame, so it must exist
    // before the first paint.
    __try { g_ctx->CopyResource(g_twin, g_target); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { g_ctx->Flush(); } __except (EXCEPTION_EXECUTE_HANDLER) { }

    g_ready = 1;
    g_stage = 1;
    g_t0 = GetTickCount();
    g_lastLoggedStage = -1;
    // 8x8 solid marker source for the wire-identification experiment (round 68)
    {
        D3D11_TEXTURE2D_DESC md = {};
        md.Width = 8; md.Height = 8; md.MipLevels = 1; md.ArraySize = 1;
        md.Format = g_desc.Format; md.SampleDesc.Count = 1;
        md.Usage = D3D11_USAGE_DEFAULT; md.BindFlags = 0;
        HRESULT hmr = g_dev->CreateTexture2D(&md, nullptr, &g_markerTex);
        wsprintfA(b, "dwm: marker tex 8x8 fmt=%u hr=0x%08X", (unsigned)g_desc.Format, (unsigned)hmr);
        iwnHookLog(b);
    }
    installCopyObserver();   // find dwm's composition->scanout copy (round 66)
    // round 75: if the wire (dwm's own swapchain buffers) was already acquired,
    // the probe's face list must NOT override it - the wire is the scanout source
    if (g_flipSC) {
        iwnHookLog("dwm: re-attach - re-applying wire buffers over face list");
        applyWireFromSC(g_flipSC);
    }
    wsprintfA(b, "dwm: WARP READY %ux%u fmt=%u - staged rollout starting",
              (unsigned)g_rtW, (unsigned)g_rtH, (unsigned)g_desc.Format);
    iwnHookLog(b);
    return true;
}

// ---- CopyResource observation hook ------------------------------------------
// Round-66 verdict: painting the live private composition faces at CF AND at
// PresentInternal entry never reaches the screen, while painting the SHARED
// input layer does - and the dump proved the private face holds the finished
// frame at CF time. Conclusion: dwm copies the composition surface into the
// real scanout allocation (a kernel-side surface with no fingerprintable
// ID3D11Texture2D wrapper) between CF and flip. This hook OBSERVES dwm's own
// CopyResource calls to find that copy. Vtable slot 47 = CopyResource. The
// patch is one aligned 8-byte vtable store (atomic on x64), same pattern as
// the proven flip-hook vtable patches.
// ---- round 77: copy observer range (ctx vtable slots 44..54) ----------------
// The round-77 observer (slots 44-54, 6-arg forwarders) saw ZERO traffic - not
// even our OWN CopyResource calls (readbackTex calls g_ctx->CopyResource every
// proof read through vtable slot 47). A live patch cannot miss those, so the
// round-77 "zero copies" verdict was a FALSE NEGATIVE: the patch was not
// effective (and its 6-arg forwarders would have dropped args 7-8 of
// CopySubresourceRegion anyway). Round 79 rebuild:
//   - the 8 slots that matter per the standard IDL vtable order:
//     12 DrawIndexed  13 Draw  33 OMSetRenderTargets  46 CopySubresourceRegion
//     47 CopyResource 48 UpdateSubresource 50 ClearRenderTargetView
//     55 ResolveSubresource
//   - 8-arg forwarders (the widest method is CopySubresourceRegion with 8)
//   - unconditional per-slot call counters -> the heartbeat proves the patch
//     is alive (our own CopyResource MUST tick slot idx 4)
//   - install-time selftest: one real CopyResource right after patching
typedef void (STDMETHODCALLTYPE* PFN_CPY8)(void*, void*, void*, void*, void*, void*, void*, void*);
#define IWN_COBS_N 8
static const int kCpySlots[IWN_COBS_N] = { 12, 13, 33, 46, 47, 48, 50, 55 };
static PFN_CPY8 g_cpyOrigR[IWN_COBS_N] = {};
static volatile LONG g_cpyCalls[IWN_COBS_N] = {};
static volatile LONG g_cpyLogged = 0;
static void** g_cpyVt = nullptr;               // ctx vtable base (self-check)
static const void* const* g_cpyHooks = nullptr;
static LONG g_cpyPatched = 0;
static volatile LONG g_cpyRepatch = 0;         // round 80: self-heal counter
static const char* kCpyNames[IWN_COBS_N] = {
    "DrawIndexed", "Draw", "OMSetRT", "CopySubres", "CopyResource",
    "UpdateSubres", "ClearRTV", "Resolve" };

// round 80: patch (or re-patch) all 8 observed slots. Returns how many slots
// were (re)written. Re-entrant - the heartbeat calls it to self-heal.
static int patchCpySlots(const char* why) {
    int n = 0;
    for (int i = 0; i < IWN_COBS_N; i++) {
        void** slot = &g_cpyVt[kCpySlots[i]];
        if (*slot == g_cpyHooks[i]) continue;
        DWORD op = 0;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &op)) continue;
        if (*slot != (void*)g_cpyHooks[i]) g_cpyOrigR[i] = (PFN_CPY8)*slot;
        *slot = (void*)g_cpyHooks[i];
        n++;
    }
    if (n) {
        InterlockedExchangeAdd(&g_cpyRepatch, n);
        char b[128];
        wsprintfA(b, "dwm: CPYR %s re-patched %d slot(s)", why, n);
        iwnHookLog(b);
    }
    return n;
}

static void IWN_CpyLog(int slot, void* a1, void* a2) {
    if (g_cpyLogged >= 40) return;
    if (!a1 && !a2) return;
    D3D11_TEXTURE2D_DESC d1 = {}, d2 = {};
    int ok1 = 0, ok2 = 0;
    __try { if (a1) { ((ID3D11Texture2D*)a1)->GetDesc(&d1); ok1 = 1; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { if (a2) { ((ID3D11Texture2D*)a2)->GetDesc(&d2); ok2 = 1; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    // only report screen-size texture traffic (dwm's desktop surfaces)
    BOOL hit = (ok1 && d1.Width == (UINT)g_rtW && d1.Height == (UINT)g_rtH) ||
               (ok2 && d2.Width == (UINT)g_rtW && d2.Height == (UINT)g_rtH);
    if (!hit) return;
    InterlockedIncrement(&g_cpyLogged);
    char b[280];
    wsprintfA(b, "dwm: CPYR slot=%d a1=%p %lux%lu bind=0x%X misc=0x%X | a2=%p %lux%lu bind=0x%X misc=0x%X",
              slot, a1, (unsigned long)d1.Width, (unsigned long)d1.Height,
              (unsigned)d1.BindFlags, (unsigned)d1.MiscFlags,
              a2, (unsigned long)d2.Width, (unsigned long)d2.Height,
              (unsigned)d2.BindFlags, (unsigned)d2.MiscFlags);
    iwnHookLog(b);
}

#define IWN_COBS_W(N, IDX)                                                       \
static void STDMETHODCALLTYPE IWN_CObs##N(void* s, void* a, void* b, void* c,    \
                                          void* d, void* e, void* f, void* g) {  \
    InterlockedIncrement(&g_cpyCalls[IDX]);                                      \
    IWN_CpyLog(kCpySlots[IDX], a, b);                                            \
    if (g_cpyOrigR[IDX]) g_cpyOrigR[IDX](s, a, b, c, d, e, f, g);                \
}
IWN_COBS_W(0, 0) IWN_COBS_W(1, 1) IWN_COBS_W(2, 2) IWN_COBS_W(3, 3)
IWN_COBS_W(4, 4) IWN_COBS_W(5, 5) IWN_COBS_W(6, 6) IWN_COBS_W(7, 7)

static void installCopyObserver() {
    if (g_cpyVt || !g_ctx) return;
    void** vt = *(void***)g_ctx;
    if (!vt || IsBadReadPtr(vt, 56 * 8)) { iwnHookLog("dwm: CPYR vtable unreadable"); return; }
    static const void* hooks[IWN_COBS_N] = {
        (void*)&IWN_CObs0, (void*)&IWN_CObs1, (void*)&IWN_CObs2, (void*)&IWN_CObs3,
        (void*)&IWN_CObs4, (void*)&IWN_CObs5, (void*)&IWN_CObs6, (void*)&IWN_CObs7
    };
    g_cpyHooks = hooks;
    g_cpyVt = vt;
    patchCpySlots("install");
    char b[96];
    wsprintfA(b, "dwm: CPYR v2 observer installed on %d/%d ctx slots", g_cpyPatched, IWN_COBS_N);
    iwnHookLog(b);
    // install-time selftest: one REAL CopyResource must tick slot idx 4
    // (g_stageTex <- g_twin, both screen-size). If it does not, the patch is
    // ineffective and every "zero traffic" verdict is void.
    if (g_stageTex && g_twin) {
        __try { g_ctx->CopyResource(g_stageTex, g_twin); } __except (1) { }
        wsprintfA(b, "dwm: CPYR SELFTEST CopyResource calls=%ld (must be >=1)",
                  (long)g_cpyCalls[4]);
        iwnHookLog(b);
    }
}

// round 80: 3s heartbeat with SELF-HEAL. Round 79 proved the patch IS
// effective at install (selftest hit) but gets REVERTED within ~2s by an
// unknown writer - while probe's IAT hooks survive the whole run. So: on every
// heartbeat, re-patch any slot that no longer points at us (keeps the observer
// alive), count the reverts, and forensically log the page protection of the
// vtable page (VirtualProtect-based revert vs raw write) plus WHICH slots got
// reverted (whole-page restore vs targeted slot).
static void cpyObserverHeartbeat() {
    if (!g_cpyVt || !g_cpyHooks) return;
    static DWORD s_last = 0;               // POD static - no TLS guard
    DWORD t = GetTickCount();
    if (t - s_last < 3000) return;
    s_last = t;
    // count reverted slots + capture the page protection BEFORE re-patching
    int rev = 0, firstRev = -1;
    for (int i = 0; i < IWN_COBS_N; i++) {
        if (g_cpyVt[kCpySlots[i]] != g_cpyHooks[i]) {
            rev++;
            if (firstRev < 0) firstRev = i;
        }
    }
    DWORD prot = 0;
    char pbuf[96] = "";
    if (rev) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(&g_cpyVt[kCpySlots[firstRev]], &mbi, sizeof(mbi)))
            prot = (DWORD)mbi.Protect;
        wsprintfA(pbuf, " prot=0x%lX", prot);
    }
    int n = patchCpySlots(rev ? "self-heal" : "noop");
    char b[320];
    wsprintfA(b, "CPYR hb reverted=%d%s repatched=%d total=%ld calls: DI=%ld D=%ld OM=%ld CSR=%ld CR=%ld US=%ld CRTV=%ld RS=%ld logged=%ld",
              rev, pbuf, n, (long)g_cpyRepatch,
              (long)g_cpyCalls[0], (long)g_cpyCalls[1], (long)g_cpyCalls[2],
              (long)g_cpyCalls[3], (long)g_cpyCalls[4], (long)g_cpyCalls[5],
              (long)g_cpyCalls[6], (long)g_cpyCalls[7], (long)g_cpyLogged);
    iwnHookLog(b);
}

// ---- paint
static void paintWarp() {
    if (!g_ready || !g_ctx || g_tgtN == 0) return;
    IWNState st;
    ZeroMemory(&st, sizeof(st));      // stateRestore releases these; never let it see stack garbage
    stateSave(&st);

    // The grid and the cbuffer only change when the operator changes a corner or
    // a blend value; re-uploading them every submit costs a Map/Unmap pair and
    // showed up as visible stutter on the 22:51 run.
    if (g_gridDirty) {
        fillGrid();
        fillCb();
        g_gridDirty = 0;
    }

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)g_rtW;
    vp.Height = (float)g_rtH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    UINT stride = sizeof(IWNVtx);
    UINT off = 0;

    __try {
        // common pipeline state once; only the RTV changes per surface
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs);
        g_ctx->OMSetBlendState(g_bs, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(g_il);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_srv);
        g_ctx->PSSetSamplers(0, 1, &g_smp);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        // copy-mode faces render through our own RT; clear it once per frame
        if (g_warpRTV) {
            const float blk[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_ctx->ClearRenderTargetView(g_warpRTV, blk);
        }
        for (int i = 0; i < g_tgtN; i++) {
            if (g_copyMode[i]) {
                if (!g_warpRTV) continue;
                if (!g_tgtDevOk[i]) continue;        // round 70: cross-device guard
                g_ctx->OMSetRenderTargets(1, &g_warpRTV, nullptr);
                g_ctx->Draw(g_vertCount, 0);
                __try { g_ctx->CopyResource(g_tgt[i], g_warpRT); }
                __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_fail); }
                InterlockedIncrement(&g_draws);
                continue;
            }
            if (!g_rtvA[i]) continue;
            g_ctx->OMSetRenderTargets(1, &g_rtvA[i], nullptr);
            g_ctx->Draw(g_vertCount, 0);
            InterlockedIncrement(&g_draws);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        logDeviceRemoved("paint");
        iwnHookLog("dwm: warp paint EXCEPTION");
    }

    stateRestore(&st);
}

// ---- wire markers (round 68) -------------------------------------------------
// An 8x8 solid square per registered face, each face at its own x position and
// in its own colour, drawn in the same paint pass as the warp. Whichever square
// the external CopyFromScreen screenshots show is the face dwm actually
// presents - the front surf named by experiment instead of by guesswork.
//   tgt0 RED @(4,4)  tgt1 LIME @(24,4)  tgt2 BLUE @(44,4)
//   tgt3 YELLOW @(64,4)  tgt4 WHITE @(84,4)
static const UINT kMarkCol[IWN_MAX_TGT] = {
    0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu, 0xFFFFFFFFu
};
static void drawWireMarkers() {
    if (!g_markerTex || !g_ctx || g_tgtN == 0) return;
    static BOOL s_mlegDone = FALSE;      // POD init - no TLS guard
    if (!s_mlegDone) {
        s_mlegDone = TRUE;
        char b[160];
        for (int i = 0; i < g_tgtN && i < IWN_MAX_TGT; i++) {
            wsprintfA(b, "dwm: MARKER tgt%d col=%08X @(%d,4)", i, (unsigned)kMarkCol[i], i * 20 + 4);
            iwnHookLog(b);
        }
    }
    D3D11_BOX bx = { 0, 0, 0, 8, 8, 1 };
    for (int i = 0; i < g_tgtN && i < IWN_MAX_TGT; i++) {
        if (!g_tgt[i]) continue;
        if (!g_tgtDevOk[i]) continue;          // round 70: cross-device guard
        UINT px = kMarkCol[i];
        g_ctx->UpdateSubresource(g_markerTex, 0, nullptr, &px, 32, 0);
        __try {
            g_ctx->CopySubresourceRegion(g_tgt[i], 0, i * 20 + 4, 4, 0, g_markerTex, 0, &bx);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

// ---------------------------------------------------------------- diagnostics
// One pixel out of a candidate surface, via a staging copy. Never call this from
// the reporter thread: the immediate context is not free-threaded.
static void readbackTex(ID3D11Texture2D* tex, const char* tag) {
    if (!g_stageTex || !g_ctx || !tex) return;
    __try {
        g_ctx->CopyResource(g_stageTex, tex);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_ctx->Map(g_stageTex, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            size_t off = (size_t)(g_rtH / 2) * m.RowPitch + (size_t)(g_rtW / 2) * 4;
            unsigned mid = *(const unsigned*)((const BYTE*)m.pData + off);
            size_t off2 = (size_t)32 * m.RowPitch + 32 * 4;
            unsigned px2 = *(const unsigned*)((const BYTE*)m.pData + off2);
            // one more sample near the centre-right, free extra signal
            size_t off3 = (size_t)(g_rtH / 2) * m.RowPitch + (size_t)(g_rtW * 3 / 4) * 4;
            unsigned px3 = *(const unsigned*)((const BYTE*)m.pData + off3);
            g_ctx->Unmap(g_stageTex, 0);
            char b[192];
            wsprintfA(b, "dwm: READBACK %s mid=%08X corner=%08X q3=%08X", tag, mid, px2, px3);
            iwnHookLog(b);
        } else {
            iwnHookLog("dwm: READBACK map failed");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("dwm: READBACK EXCEPTION");
    }
}

// --------------------------------------------------- round 82: scanout source
// CDWMSwapChain::GetBuffer (dwmcore+0xE6060): resolves a LOGICAL buffer index
// to the real ID3D11Texture2D by calling wire->GetBuffer(idx, IID_Tex2D). This
// is dwm's own accessor - the exact "get the composed desktop" function.
static const GUID kIIDTex2D =
    { 0xdc8e63f3, 0xd12b, 0x4952, { 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d } };
typedef HRESULT (STDMETHODCALLTYPE* PFN_SCGETBUF)(void*, UINT, const GUID&, void**);

static ID3D11Texture2D* iwnWireBuffer(void* self, UINT idx) {
    if (!self) return nullptr;
    HMODULE core = GetModuleHandleW(L"dwmcore.dll");
    if (!core) return nullptr;
    PFN_SCGETBUF fn = (PFN_SCGETBUF)(void*)((char*)core + 0xE6060);
    void* tex = nullptr;
    __try {
        if (FAILED(fn(self, idx, kIIDTex2D, &tex))) return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return (ID3D11Texture2D*)tex;    // caller owns the AddRef
}

// One-shot forensic dump: which of the wire buffers actually holds composed
// desktop content at PresentInternal entry? Pairs with the LBIDX back index.
static void iwnProbeWireBuffers(void* self) {
    static volatile LONG s_done = 0;             // POD static - no TLS guard
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    UINT n = 3;
    __try {
        UINT c = *(UINT*)((char*)self + 0xa0);
        if (c > 0 && c <= 4) n = c;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    char b[160];
    wsprintfA(b, "WBUF: bufcnt=%u backIdx=%ld", n, (long)g_lbBackLast);
    iwnHookLog(b);
    for (UINT i = 0; i < n; i++) {
        ID3D11Texture2D* t = iwnWireBuffer(self, i);
        if (!t) { wsprintfA(b, "WBUF: idx=%u GetBuffer FAILED", i); iwnHookLog(b); continue; }
        D3D11_TEXTURE2D_DESC d = {};
        __try { t->GetDesc(&d); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        BOOL ours = (d.Width == (UINT)g_rtW && d.Height == (UINT)g_rtH);
        wsprintfA(b, "WBUF: idx=%u tex=%p %lux%lu bind=0x%X misc=0x%X sameDev=%d",
                  i, t, (unsigned long)d.Width, (unsigned long)d.Height,
                  (unsigned)d.BindFlags, (unsigned)d.MiscFlags,
                  (d.Width == (UINT)g_rtW && d.Height == (UINT)g_rtH) ? 1 : 0);
        iwnHookLog(b);
        if (ours) {
            char tag[16];
            wsprintfA(tag, "WB%d", i);
            readbackTex(t, tag);
        }
        t->Release();
    }
}

// The submit-side half: put our warped frame into the exact buffer dwm is
// about to hand to scanout. Index comes from CDWMSwapChain::GetLogical+
// BackBufferIndex (already ticked in IWN_PresentWrap), texture from
// CDWMSwapChain::GetBuffer. Cross-device copies are the one thing that hard-
// crashes dwm, so the device identity is checked before every CopyResource.
static volatile LONG g_scanBlits = 0;
static volatile LONG g_scanFails = 0;

static HMODULE moduleFromPtr(const void* p);   // fwd: defined further down

// r85: identify what object actually receives the submit. PresentInternal does
//   C = *(void**)( *(void**)( *(char**)self + 0x10 ) + 0x50 ) + 0xF0
//   call C->vt[13](wireSwapchain, rects, count, ...)
// Logging C's vtable RVA lets us name the class from the PDB `vftable` symbols.
static void iwnIdentifySubmit(void* self) {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    __try {
        char* A = *(char**)((char*)self + 0x10);
        if (!A || IsBadReadPtr((char*)A + 0x50, 8)) { iwnHookLog("SUBMIT: A bad"); return; }
        char* B = *(char**)(A + 0x50);
        if (!B || IsBadReadPtr(B + 0xf0, 8)) { iwnHookLog("SUBMIT: B bad"); return; }
        void* C = *(void**)(B + 0xf0);
        if (!C || IsBadReadPtr(C, 8)) { iwnHookLog("SUBMIT: C bad"); return; }
        void** vt = *(void***)C;
        HMODULE m = moduleFromPtr((void*)vt);
        char b[192];
        wsprintfA(b, "SUBMIT C=%p vtable=mod+0x%05lX (A=%p B=%p)",
                  C, m ? (unsigned long)((char*)vt - (char*)m) : 0, A, B);
        iwnHookLog(b);
        if (vt && !IsBadReadPtr(&vt[13], 8)) {
            void* f13 = vt[13];
            HMODULE m2 = moduleFromPtr(f13);
            wsprintfA(b, "SUBMIT slot13=mod+0x%05lX  slot0=mod+0x%05lX",
                      m2 ? (unsigned long)((char*)f13 - (char*)m2) : 0,
                      moduleFromPtr(vt[0])
                          ? (unsigned long)((char*)vt[0] - (char*)moduleFromPtr(vt[0])) : 0);
            iwnHookLog(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("SUBMIT identify EXCEPTION");
    }
}

// r86: the bind info handed to the DComposition core. Layout comes straight
// from CGlobalCompositionSurfaceInfo::CBindInfo::UnpinResources (0xEFEF8):
//   +0xB4  hasResources flag
//   +0xD0  resource array A          +0xD8  resource array B
//   +0xE0  count
// Those resources are what the kernel compositor composites/scans out - the
// user-mode swapchain buffers are only staging.
static volatile LONG g_bindBlits = 0;
static volatile LONG g_bindFails = 0;

static void iwnBlitToBindInfo(void* self) {
    if (!self || !g_target || !g_ctx) return;
    char* A = nullptr; char* B = nullptr; char* C = nullptr;
    __try {
        A = *(char**)((char*)self + 0x10);
        if (!A || IsBadReadPtr(A + 0x50, 8)) return;
        B = *(char**)(A + 0x50);
        if (!B || IsBadReadPtr(B + 0xf0, 8)) return;
        C = *(char**)(B + 0xf0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!C || IsBadReadPtr(C + 0xE0, 4)) return;

    UINT n = 0; void** arr = nullptr; void** arr2 = nullptr;
    __try {
        n = *(UINT*)(C + 0xE0);
        if (!IsBadReadPtr(C + 0xD0, 8)) arr = *(void***)(C + 0xD0);
        if (!IsBadReadPtr(C + 0xD8, 8)) arr2 = *(void***)(C + 0xD8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (n == 0 || n > 8) return;

    D3D11_TEXTURE2D_DESC td = {};
    __try { g_target->GetDesc(&td); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    static volatile LONG s_dumped = 0;         // POD static - no TLS guard
    BOOL dump = (InterlockedCompareExchange(&s_dumped, 1, 0) == 0);
    UINT done = 0;
    for (int which = 0; which < 2; which++) {
        void** list = which ? arr2 : arr;
        if (!list || IsBadReadPtr(list, (size_t)n * 8)) continue;
        for (UINT i = 0; i < n; i++) {
            void* r = nullptr;
            __try { r = list[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!r || IsBadReadPtr(r, 8)) continue;
            ID3D11Texture2D* tex = nullptr;
            __try {
                if (SUCCEEDED(((IUnknown*)r)->QueryInterface(kIIDTex2D, (void**)&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC d = {};
                    tex->GetDesc(&d);
                    if (dump) {
                        char b[192];
                        wsprintfA(b, "BIND arr%d[%u]=%p %lux%lu bind=0x%X misc=0x%X fmt=%u",
                                  which, i, tex, (unsigned long)d.Width,
                                  (unsigned long)d.Height, (unsigned)d.BindFlags,
                                  (unsigned)d.MiscFlags, (unsigned)d.Format);
                        iwnHookLog(b);
                    }
                    if (d.Width == td.Width && d.Height == td.Height &&
                        d.Format == td.Format && d.SampleDesc.Count == td.SampleDesc.Count) {
                        ID3D11Device* dv = nullptr;
                        tex->GetDevice(&dv);
                        if (dv && dv == g_dev) {
                            g_ctx->CopyResource(tex, g_target);
                            done++;
                        }
                        if (dv) dv->Release();
                    }
                    tex->Release();
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }
    if (done) InterlockedIncrement(&g_bindBlits);
    else      InterlockedIncrement(&g_bindFails);
    {
        static DWORD s_last = 0;
        DWORD t = GetTickCount();
        if (t - s_last > 5000) {
            s_last = t;
            char b[160];
            wsprintfA(b, "BINDBLIT n=%u done=%u ok=%ld fail=%ld draws=%ld",
                      n, done, (long)g_bindBlits, (long)g_bindFails, (long)g_draws);
            iwnHookLog(b);
        }
    }
}

static void iwnBlitToScanout(void* self) {
    if (!self || !g_target || !g_ctx) return;
    UINT n = 3;
    __try {
        UINT c = *(UINT*)((char*)self + 0xa0);
        if (c > 0 && c <= 4) n = c;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    // r85: dwm's blt present can be 1-2 frames behind the buffer the wrapper
    // sees, so paint EVERY buffer. If the screen still ignores all three, this
    // swapchain is not the scanout source at all and the submit target must be
    // identified (iwnIdentifySubmit).
    UINT okN = 0;
    for (UINT i = 0; i < n; i++) {
        ID3D11Texture2D* bb = iwnWireBuffer(self, i);
        if (!bb) { InterlockedIncrement(&g_scanFails); continue; }
        __try {
            ID3D11Device* d = nullptr;
            D3D11_TEXTURE2D_DESC bd = {}, td = {};
            bb->GetDesc(&bd);
            g_target->GetDesc(&td);
            bb->GetDevice(&d);
            if (d && d == g_dev && bd.Width == td.Width && bd.Height == td.Height &&
                bd.Format == td.Format && bd.SampleDesc.Count == td.SampleDesc.Count) {
                g_ctx->CopyResource(bb, g_target);
                okN++;
            }
            if (d) d->Release();
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        bb->Release();
    }
    if (okN) InterlockedIncrement(&g_scanBlits);
    else     InterlockedIncrement(&g_scanFails);
    {
        static DWORD s_last = 0;             // POD static - no TLS guard
        DWORD t = GetTickCount();
        if (t - s_last > 5000) {
            s_last = t;
            char b[144];
            wsprintfA(b, "SCANOUT all=%u blits=%ld fails=%ld draws=%ld backIdx=%ld",
                      okN, (long)g_scanBlits, (long)g_scanFails, (long)g_draws,
                      (long)g_lbBackLast);
            iwnHookLog(b);
        }
    }
}

// round 84 forensics: separate "blit never landed" from "landed but present
// overwrote / never scanned out". Three samples per side, first 3 frames only.
static void iwnForensicsBefore(void* self) {
    static int s_n = 0;                      // POD static - no TLS guard
    if (s_n >= 3) return;
    s_n++;
    readbackTex(g_target, "F-SRC");          // what we blit FROM
    ID3D11Texture2D* bb = iwnWireBuffer(self, 0);
    if (bb) { readbackTex(bb, "F-DST"); bb->Release(); }
}

static void iwnForensicsAfter(void* self) {
    static int s_n = 0;                      // POD static - no TLS guard
    if (s_n >= 3) return;
    s_n++;
    ID3D11Texture2D* bb = iwnWireBuffer(self, 0);
    if (bb) { readbackTex(bb, "F-POST"); bb->Release(); }
    readbackTex(g_target, "F-SRC2");         // is the source re-rendered too?
}

static void readbackPixel(int i, const char* tag) {
    if (i < 0 || i >= g_tgtN || !g_tgt[i]) return;
    readbackTex(g_tgt[i], tag);
}

// ---- round 72: full-face raw dump (engine device only - DEVCHK-proven) ------
static void writeFaceRaw(const wchar_t* path, ID3D11Texture2D* face) {
    D3D11_TEXTURE2D_DESC td = {};
    __try { face->GetDesc(&td); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.BindFlags        = 0;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags        = 0;
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    ID3D11Texture2D* stg = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg)) || !stg) return;
    g_ctx->CopyResource(stg, face);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp)) && mp.pData) {
        SIZE_T rowBytes = (SIZE_T)td.Width * 4;
        BYTE* flat = (BYTE*)HeapAlloc(GetProcessHeap(), 0, rowBytes * td.Height);
        if (flat) {
            for (UINT y = 0; y < td.Height; y++)
                CopyMemory(flat + y * rowBytes,
                           (BYTE*)mp.pData + (SIZE_T)y * mp.RowPitch, rowBytes);
            HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                WriteFile(h, flat, (DWORD)(rowBytes * td.Height), &wrote, nullptr);
                CloseHandle(h);
            }
            HeapFree(GetProcessHeap(), 0, flat);
        }
        g_ctx->Unmap(stg, 0);
    }
    stg->Release();
}

// Four dump rounds: r1/r2 inside modes A/B (face = last painted content,
// pre-restore), r3/r4 in the quiet window (residue + cursor composition check).
static void dumpFacesTick(DWORD el) {
    static const DWORD kRounds[4] = { 25000, 55000, 122000, 140000 };
    static int s_next = 0;                       // POD static - no TLS guard
    if (s_next >= 4) return;
    if (el < kRounds[s_next]) return;
    int r = s_next + 1;
    s_next++;
    char b[192];
    for (int i = 0; i < g_tgtN && i < IWN_MAX_TGT; i++) {
        if (!g_tgt[i] || !g_tgtDevOk[i]) continue;
        wchar_t path[64];
        {
            const wchar_t* p1 = L"C:\\Windows\\Temp\\iwnf";
            const wchar_t* p2 = L".raw";
            wchar_t* o = path;
            for (const wchar_t* q = p1; *q; ) *o++ = *q++;
            *o++ = (wchar_t)(L'0' + i);
            *o++ = L'_';
            *o++ = (wchar_t)(L'0' + r);
            for (const wchar_t* q = p2; *q; ) *o++ = *q++;
            *o = 0;
        }
        __try { writeFaceRaw(path, g_tgt[i]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        POINT cp = { -1, -1 };
        GetCursorPos(&cp);
        wsprintfA(b, "FACE DUMP r=%d tgt=%d -> %S front=%ld back=%ld cur=%ld,%ld",
                  r, i, path, (long)g_lbFrontLast, (long)g_lbBackLast,
                  (long)cp.x, (long)cp.y);
        iwnHookLog(b);
    }
    if (r == 4) {
        g_quiet = 0;
        g_dwin = 2;
        iwnHookLog("FACE DUMP window closed - paint resumed");
    }
}
// ---------------------------------------------------------------- phase machine
// 00:10 run (dest-keystone + black clear at the ConfirmFrame boundary) gave the
// unifying fact: dwm renders DIRTY REGIONS only into the composite surfaces.
// Window/taskbar regions were redrawn straight every compose; everything else
// kept our last output, so the keystone mesh re-compressed the whole frame each
// compose and the desktop collapsed toward the bottom edge (faint smear in the
// t54 capture, vs-base mean climbing 37 -> 60 without settling).
//
// Consequences:
//   * painting WITHOUT restoring feeds back (00:10 run) - the twin must always
//     be a fresh snapshot of dwm's just-rendered frame, and the surfaces must
//     be handed back clean after the present (the original's forcePreserveBB);
//   * restoring at the START of the next submit erases dwm's fresh dirty
//     renders (23:18 run froze) - the restore must happen right after the
//     present returns, before dwm renders the next frame;
//   * the ConfirmFrame boundary is not the present itself. dwmcore
//     CDWMSwapChain::PresentInternal (RVA 0x6C6B0) is the actual presentation
//     call, the same layer the original product hooks ("robust DXGI hooking").
//
// One run, three phases, so a single operator pass is a controlled experiment:
//   A  1.5- 8s   paint at ConfirmFrame,  restore right after the submit
//   B  8- 16s    paint at ConfirmFrame,  restore deferred one submit (23:18
//                reference: warp visible, desktop frozen)
//   C  16s-75s   paint inside CDWMSwapChain::PresentInternal (prologue detour,
//                12-byte patch, 28-byte position-independent prologue moves to
//                a trampoline), restore at the next ConfirmFrame
// All phases use the same fresh-save + black-clear + dest-keystone paint.
#define IWN_PH_IDLE   1
#define IWN_PH_CF_IMM 2
#define IWN_PH_CF_DEF 3
#define IWN_PH_PRESNT 4

static volatile LONG  g_pendingRestore = 0;
static DWORD          g_lastPaintTick = 0;

// ---- PresentInternal wrapper detour ----------------------------------------
// 07:00 run established WHY the CF paint point cannot warp windows: dwm paints
// window content into the composite surface AFTER our CF paint (dirty-rect
// rendering - the wallpaper keeps our warp because dwm never redraws it, every
// window is redrawn unwarped on top). The only point where the frame is FINAL
// (wallpaper + all windows) and still pre-scanout is inside
// CDWMSwapChain::PresentInternal, right before the real present runs.
//
// Implementation: patch the entry with movabs rax,&IWN_PresentWrap; jmp rax.
// IWN_PresentWrap is an ORDINARY C++ function - the compiler emits the whole
// ABI (stack alignment, shadow space, register saves) instead of us hand-writing
// a stub (three dwm deaths were caused by stub byte bugs). PresentInternal's
// real signature has 4 register args + exactly ONE stack arg (measured: the
// body reads [rbp+0x100] once and nothing beyond, rbp = entryRsp-0xD8), so the
// wrapper declares 5 parameters and forwards them unchanged.
// After the original body returns (frame already submitted) we restore the
// clean composed frame, so dwm's next dirty-rect pass never sees our warp -
// no accumulation. SyncInterval is normalized to 0 + ALLOW_TEARING in dwm
// (measured in the original's utilPostPresent), so the flip happens inside the
// present call, which is what makes an immediate restore viable.
static void* g_trampMem = nullptr;             // RWX: displaced prologue + jmp back
static HRESULT (STDMETHODCALLTYPE* g_presentTramp)(void*, void*, void*, void*, UINT64) = nullptr;
static LONG  g_detourInstalled = 0;
static void restoreAllSurfaces(const char* why);   // defined below the wrapper

// ---- inner flip hook --------------------------------------------------------
// 08:01 run verdict: painting at PresentInternal ENTRY still leaves windows
// unwarped - PresentInternal composes the window dirty rects AFTER its entry
// and before the real flip, so the window regions of our paint are redrawn
// straight. The only point where the frame is final AND pre-scanout is the
// inner present call. Static disassembly of PresentInternal (0x6C6B0) shows
// TWO mutually exclusive branches that both feed the same continuation, which
// compares the return against 0x87A0001 (DXGI_STATUS_OCCLUDED):
//   path A: r15=[self+0x1a8]; rax=[r15]; rax=[rax+0x80]   -> SLOT 16 of [self+0x1a8]
//   path B: c=[[[self+0x10]+0x50]+0xf0]; rax=[c]; [rax+0x68] -> SLOT 13 of the chain
// 08:53 run: patching only slot 13 froze the paint (frames stuck at 18) - the
// live desktop took path A. Both slots are now patched with the same paint.
static HRESULT (STDMETHODCALLTYPE* g_flipTrampA)(void*, void*, void*, void*, void*,
                                                 void*, void*, void*, void*, void*) = nullptr;
static HRESULT (STDMETHODCALLTYPE* g_flipTrampB)(void*, void*, void*, void*, void*,
                                                 void*, void*, void*, void*, void*) = nullptr;
static volatile LONG g_flipTry   = 0;
static volatile LONG g_flipHooked = 0;
static HRESULT STDMETHODCALLTYPE IWN_FlipWrapA(void* self, void* a2, void* a3, void* a4, void* a5,
                                               void* a6, void* a7, void* a8, void* a9, void* a10);
static HRESULT STDMETHODCALLTYPE IWN_FlipWrapB(void* self, void* a2, void* a3, void* a4, void* a5,
                                               void* a6, void* a7, void* a8, void* a9, void* a10);

static HMODULE moduleFromPtr(const void* p) {
    HMODULE h = nullptr;
    if (!p) return nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &h)) return nullptr;
    return h;
}

// ---- round 75: swapchain wire buffers ---------------------------------------
// Round 74 proved: warp painted into ALL registered composition faces (both CF
// and PresentInternal entry points, RTV and copy modes) NEVER reaches the
// display, while the faces carry desktop pixels identical to CopyFromScreen.
// Conclusion: the faces are dwm's composition working set, but the present
// path blits dwm's OWN swapchain buffers (the LBIDX front=2 ring) to scanout.
// applyWireFromSC enumerates those buffers via QI(IDXGISwapChain) on the
// PresentInternal wrapper's `this` and replaces the paint target list with
// them - the classic back-buffer hook (the original's "robust DXGI hooking").
static void applyWireTargets(ID3D11Texture2D** bufs, UINT n) {
    char b[192];
    for (int i = 0; i < g_tgtN; i++) {
        if (g_rtvA[i]) { g_rtvA[i]->Release(); g_rtvA[i] = nullptr; }
        if (g_tgt[i])  { g_tgt[i]->Release();  g_tgt[i]  = nullptr; }
        g_copyMode[i] = FALSE;
        g_tgtDevOk[i] = FALSE;
    }
    g_tgtN = 0;
    for (UINT i = 0; i < n && g_tgtN < IWN_MAX_TGT; i++) {
        ID3D11Texture2D* t = bufs[i];
        if (!t) continue;
        D3D11_TEXTURE2D_DESC td = {};
        __try { t->GetDesc(&td); } __except (EXCEPTION_EXECUTE_HANDLER) { t->Release(); continue; }
        ID3D11Device* cdev = nullptr;
        t->GetDevice(&cdev);
        BOOL devOk = (cdev == g_dev);
        if (cdev) cdev->Release();
        if (!devOk) {
            wsprintfA(b, "dwm: WIRE buf[%u] CROSS-DEVICE - skipped", i);
            iwnHookLog(b);
            t->Release();
            continue;
        }
        ID3D11RenderTargetView* rv = nullptr;
        HRESULT hr2 = g_dev->CreateRenderTargetView(t, nullptr, &rv);
        BOOL rtvOk = SUCCEEDED(hr2) && rv;
        t->AddRef();
        g_tgt[g_tgtN]      = t;
        g_rtvA[g_tgtN]     = rtvOk ? rv : nullptr;
        g_copyMode[g_tgtN] = !rtvOk;
        g_tgtDevOk[g_tgtN] = TRUE;
        g_tgtN++;
        wsprintfA(b, "dwm: WIRE tgt[%d] = buf[%u] %ux%u bind=0x%X misc=0x%X %s",
                  g_tgtN - 1, i, (unsigned)td.Width, (unsigned)td.Height,
                  (unsigned)td.BindFlags, (unsigned)td.MiscFlags,
                  rtvOk ? "RTV mode" : "COPY mode (RTV refused)");
        iwnHookLog(b);
    }
    if (g_tgtN > 0) {
        g_target = g_tgt[0];   // borrowed ref: twin source + save/restore subject
        D3D11_TEXTURE2D_DESC td0 = {};
        __try { g_target->GetDesc(&td0); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (td0.Width != g_rtW || td0.Height != g_rtH) {
            wsprintfA(b, "dwm: WIRE size %ux%u != face %ux%u - twin/restore may mismatch",
                      (unsigned)td0.Width, (unsigned)td0.Height,
                      (unsigned)g_rtW, (unsigned)g_rtH);
            iwnHookLog(b);
        }
        iwnHookLog("dwm: PAINT TARGETS = dwm swapchain wire buffers");
    } else {
        iwnHookLog("dwm: WIRE registration produced no targets - faces kept");
    }
}

static void applyWireFromSC(IDXGISwapChain* sc) {
    char b[224];
    if (!sc) return;
    DXGI_SWAP_CHAIN_DESC dsc = {};
    if (FAILED(sc->GetDesc(&dsc))) { iwnHookLog("dwm: WIRE GetDesc failed"); return; }
    const char* se = dsc.SwapEffect == 0 ? "DISCARD(blt)" :
                     dsc.SwapEffect == 1 ? "SEQUENTIAL(blt)" :
                     dsc.SwapEffect == 3 ? "FLIP_SEQUENTIAL" :
                     dsc.SwapEffect == 4 ? "FLIP_DISCARD" : "?";
    wsprintfA(b, "dwm: WIRE SC %ux%u bufcnt=%u swap=%s windowed=%u fmt=%u",
              (unsigned)dsc.BufferDesc.Width, (unsigned)dsc.BufferDesc.Height,
              (unsigned)dsc.BufferCount, se, (unsigned)dsc.Windowed,
              (unsigned)dsc.BufferDesc.Format);
    iwnHookLog(b);
    ID3D11Texture2D* wire[4] = {}; UINT wireN = 0;
    for (UINT i = 0; i < dsc.BufferCount && i < 4; i++) {   // back buffers only
        ID3D11Texture2D* t = nullptr;
        HRESULT hb = sc->GetBuffer(i, __uuidof(ID3D11Texture2D), (void**)&t);
        if (SUCCEEDED(hb) && t) {
            D3D11_TEXTURE2D_DESC td = {};
            t->GetDesc(&td);
            wsprintfA(b, "dwm: WIRE buf[%u] %ux%u bind=0x%X misc=0x%X ptr=%p",
                      i, (unsigned)td.Width, (unsigned)td.Height,
                      (unsigned)td.BindFlags, (unsigned)td.MiscFlags, (void*)t);
            iwnHookLog(b);
            if (td.Width == g_rtW && td.Height == g_rtH &&
                (td.BindFlags & D3D11_BIND_RENDER_TARGET) && wireN < 4) {
                wire[wireN++] = t;      // ref handed to applyWireTargets
            } else {
                t->Release();
            }
        } else {
            wsprintfA(b, "dwm: WIRE buf[%u] GetBuffer hr=0x%08X", i, (unsigned)hb);
            iwnHookLog(b);
        }
    }
    if (wireN > 0) applyWireTargets(wire, wireN);
    else iwnHookLog("dwm: WIRE no usable back buffers");
}

// ---- flip-object inspection (round 68, wired into the wrapper in round 75) ---
// The front surf must be named by dwm's OWN present path, not guessed among
// the scanned candidates. Called once from IWN_PresentWrap: `self` there is
// the object dwm actually presents through. QI for IDXGISwapChain (self, then
// the [0x1A8] member the buffer accessors index through), GetDesc reveals the
// real buffer count and swap effect, GetBuffer(i) enumerates the wire buffers.
// round 78: one shared wire-vtable walker (was duplicated in both QI branches).
// Enumerates 96 slots (round 77 mapped 0..63; dwm's present overrides may sit
// beyond), logs each module-relative RVA and AUTO-HOOKS by RVA fingerprint:
//   dxgi+0x6BAA0 CDXGISwapChainDWMLegacy::Present    -> paint hook (exclusive)
//   dxgi+0x1AE30 CDXGISwapChainDWMLegacy::PresentDWM -> observe-only hook
// Each hook installs exactly once (guarded by its own flag) so a re-inspection
// can never patch a slot that already points at us (self-loop).
static void enumWireVtableAndHook(void* obj) {
    char b2[160];
    void** vt = *(void***)obj;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!vt || IsBadReadPtr(vt, 96 * 8)) {
        iwnHookLog("dwm: WIREVT vtable unreadable (96 slots)");
        return;
    }
    for (int s = 0; s < 96; s++) {
        if (IsBadReadPtr(&vt[s], 8)) break;
        void* fp = vt[s];
        HMODULE m = moduleFromPtr(fp);
        const char* mark = "";
        if (m) {
            unsigned long rva = (unsigned long)((char*)fp - (char*)m);
            if (m == dxgi && rva == 0x6BAA0 && !g_dwmPresHooked) {
                // round 81: hook install DISABLED (strategic retreat to the
                // round-63 recipe). r78 proved this slot gets zero calls.
                mark = "  <== Legacy::Present (hook skipped r81)";
            } else if (m == dxgi && rva == 0x1AE30 && !g_pdwmObsHooked) {
                mark = "  <== PresentDWM (hook skipped r81)";
            } else if (m == dxgi && (rva == 0x1AE30 || rva == 0x9CA0 || rva == 0x6BAA0)) {
                mark = "  <== PresentDWM";
            }
            wsprintfA(b2, "dwm: WIREVT slot%-2d = mod+0x%05lX%s", s, rva, mark);
        } else {
            wsprintfA(b2, "dwm: WIREVT slot%-2d = %p (no module)", s, fp);
        }
        iwnHookLog(b2);
    }
}

static void IWN_FlipInspect(void* self) {
    char b[280];
    if (!self) { iwnHookLog("dwm: FLIPSC self=null"); return; }
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = E_NOINTERFACE;
    __try { hr = ((IUnknown*)self)->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sc = nullptr; hr = 0xE0000001; }
    if (SUCCEEDED(hr) && sc) {
        iwnHookLog("dwm: FLIPSC QI OK on self - enumerating wire buffers");
        // round 77/78: full vtable walk + RVA-fingerprint auto-hook
        enumWireVtableAndHook(sc);
        // round 81: applyWireFromSC DISABLED - it replaced the texscan composed
        // faces (round-63 visible recipe) with the wire back buffers (black at
        // wrapper entry), which is what killed all paint modes since r75.
        iwnHookLog("dwm: WIREDIS applyWireFromSC skipped (round 81 - keep composed faces)");
        g_flipSC = sc;   // keep the AddRef from QueryInterface
        return;
    }
    // round 75: the [0x1A8] member is what the buffer accessors index through -
    // the swapchain COM object may live there rather than on self
    void* oA = IsBadReadPtr((char*)self + 0x1a8, 8) ? nullptr
                                                    : *(void**)((char*)self + 0x1a8);
    if (oA) {
        __try { hr = ((IUnknown*)oA)->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { sc = nullptr; hr = 0xE0000002; }
        if (SUCCEEDED(hr) && sc) {
            iwnHookLog("dwm: FLIPSC QI OK on self+1A8 - enumerating wire buffers");
            enumWireVtableAndHook(sc);
            iwnHookLog("dwm: WIREDIS applyWireFromSC skipped (round 81 - keep composed faces)");
            g_flipSC = sc;
            return;
        }
    }
    wsprintfA(b, "dwm: FLIPSC QI hr=0x%08X (self and +1A8) - not a DXGI swapchain, scanning object", (unsigned)hr);
    iwnHookLog(b);
    for (UINT off = 0; off < 0x1C0; off += 8) {
        if (IsBadReadPtr((char*)self + off, 8)) break;
        void* p = *(void**)((char*)self + off);
        if (!p) continue;
        for (int j = 0; j < g_tgtN; j++) {
            if (p == g_tgt[j]) {
                if (g_frontTgt < 0) g_frontTgt = j;
                wsprintfA(b, "dwm: FLIPOBJ tgt[%d] pointer at self+0x%X", j, (unsigned)off);
                iwnHookLog(b);
            }
        }
    }
}

// Shared paint body: fresh clean snapshot -> black clear -> keystone paint.
// Returns TRUE when draws happened (caller then defers the restore).
static BOOL flipPaint() {
    if (!(g_ready && (LONG)g_stage == IWN_PH_PRESNT && g_ctx)) return FALSE;
    DWORD t = GetTickCount();
    if (t == g_lastPaintTick) return FALSE;      // one paint per compose
    g_lastPaintTick = t;
    InterlockedIncrement(&g_frames);
    __try {
        g_ctx->CopyResource(g_twin, g_target);   // clean COMPLETE frame
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (int i = 0; i < g_tgtN; i++) {
            if (g_rtvA[i]) g_ctx->ClearRenderTargetView(g_rtvA[i], black);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        iwnHookLog("dwm: warp save EXCEPTION (flip)");
    }
    int before = (int)g_draws;
    paintWarp();
    if (g_flipCalls < 1800) drawWireMarkers();   // ~30s identification window
    if ((int)g_draws == before) {
        if (g_fail < 40) iwnHookLog("dwm: warp paint produced no Draw (flip)");
        return FALSE;
    }
    if (!g_proofWarp) {
        g_proofWarp = 1;
        readbackPixel(0, "warp-flip");
    }
    return TRUE;
}

// ---- round 78: dwm-own-present paint hook ------------------------------------
// Body mirrors the proven wrapper-entry paint: snapshot g_target (= the wire
// back buffer buf0, RTV mode) into the twin, clear the registered RTVs, paint
// the warp, forward to the real CDXGISwapChainDWMLegacy::Present (which does
// the back->front blit), then defer the restore to the next ConfirmFrame.
// The one-shot TWIN-D/WARPRT-D proof settles round 76's open question: if
// TWIN-D holds desktop pixels, dwm's compose->backbuffer copy happens BEFORE
// this point and entry-paint is the valid point; if TWIN-D is black the copy
// happens inside the trampoline and the paint must move after it.
static HRESULT STDMETHODCALLTYPE IWN_DWMPresentWrap(void* self, void* a2, void* a3, void* a4, void* a5,
                                                    void* a6, void* a7, void* a8, void* a9, void* a10) {
    InterlockedIncrement(&g_dwmPresCalls);
    if (!g_dwmPresentTramp) return E_FAIL;
    BOOL painted = FALSE;
    if (g_ready && g_ctx && !g_quiet) {
        DWORD t = GetTickCount();
        if (t != g_lastPaintTick) {           // one paint per compose (exclusive owner)
            g_lastPaintTick = t;
            InterlockedIncrement(&g_frames);
            __try {
                g_ctx->CopyResource(g_twin, g_target); // frame BEFORE our paint
                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                for (int i = 0; i < g_tgtN; i++) {
                    if (g_rtvA[i]) g_ctx->ClearRenderTargetView(g_rtvA[i], black);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&g_fail);
                iwnHookLog("dwm: warp save EXCEPTION (dwm-present)");
            }
            int before = (int)g_draws;
            paintWarp();
            if ((int)g_draws != before) {
                painted = TRUE;
                static BOOL s_proofD = FALSE;     // POD static - no TLS guard
                if (!s_proofD) {
                    s_proofD = TRUE;
                    readbackTex(g_twin,   "TWIN-D");
                    readbackTex(g_warpRT, "WARPRT-D");
                    readbackPixel(0, "warp-dwm");
                }
            }
        }
    }
    HRESULT hr = g_dwmPresentTramp(self, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (painted) g_pendingRestore = 1;   // restore at next CF
    return hr;
}

// observe-only wrapper for PresentDWM (0x1AE30) - counts traffic, never paints.
// Proves whether the legacy present path is (or is not) on the hot path.
static HRESULT STDMETHODCALLTYPE IWN_LegacyObsWrap(void* self, void* a2, void* a3, void* a4, void* a5,
                                                   void* a6, void* a7, void* a8, void* a9, void* a10) {
    InterlockedIncrement(&g_pdwmObsCalls);
    if (!g_pdwmObsTramp) return E_FAIL;
    return g_pdwmObsTramp(self, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

#define FLIP_BODY(TRAMP)                                                       \
    InterlockedIncrement(&g_flipCalls);                                        \
    if (InterlockedCompareExchange(&g_flipInspec, 1, 0) == 0)                  \
        IWN_FlipInspect(self);                                                 \
    BOOL painted = FALSE;                                                      \
    if (g_ready && g_flipHooked && TRAMP && !g_quiet && !g_dwmPresHooked)      \
        painted = flipPaint();                                                 \
    HRESULT hr = TRAMP(self, a2, a3, a4, a5, a6, a7, a8, a9, a10);             \
    if (painted) g_pendingRestore = 1;   /* restore at next CF */              \
    return hr;

static HRESULT STDMETHODCALLTYPE IWN_FlipWrapA(void* self, void* a2, void* a3, void* a4, void* a5,
                                               void* a6, void* a7, void* a8, void* a9, void* a10) {
    FLIP_BODY(g_flipTrampA)
}

static HRESULT STDMETHODCALLTYPE IWN_FlipWrapB(void* self, void* a2, void* a3, void* a4, void* a5,
                                               void* a6, void* a7, void* a8, void* a9, void* a10) {
    FLIP_BODY(g_flipTrampB)
}

// patch one vtable slot; returns the original target or nullptr
static void* patchVtableSlot(void* obj, int slot, void* hook, const char* tag) {
    if (!obj || IsBadReadPtr(obj, 8)) return nullptr;
    void** vt = *(void***)obj;
    if (!vt || IsBadReadPtr(vt, (slot + 1) * 8)) return nullptr;
    void* orig = vt[slot];
    HMODULE om = moduleFromPtr(orig);
    DWORD op = 0;
    if (!VirtualProtect(&vt[slot], 8, PAGE_READWRITE, &op)) {
        char b[128];
        wsprintfA(b, "dwm: flip hook VP failed (%s) gle=%lu", tag, GetLastError());
        iwnHookLog(b);
        return nullptr;
    }
    vt[slot] = hook;
    DWORD op2 = 0;
    VirtualProtect(&vt[slot], 8, op, &op2);
    char b[160];
    wsprintfA(b, "dwm: FLIP HOOK %s installed (vtable %p slot%d prev=%p mod=%p)",
              tag, (void*)vt, slot, orig, (void*)om);
    iwnHookLog(b);
    return orig;
}

static void tryInstallFlipHook(void* self) {
    if (InterlockedCompareExchange(&g_flipTry, 1, 0) != 0) return;
    char* s = (char*)self;
    // path A object: direct member
    void* objA = nullptr;
    if (!IsBadReadPtr(s + 0x1a8, 8)) objA = *(void**)(s + 0x1a8);
    void* oA = patchVtableSlot(objA, 16, (void*)&IWN_FlipWrapA, "A[slot16]");
    if (oA) {
        g_flipTrampA = (HRESULT (STDMETHODCALLTYPE*)(void*, void*, void*, void*, void*,
                                                     void*, void*, void*, void*, void*))oA;
        g_flipHooked = 1;
    }
    // path B object: three-hop member chain
    void* c1 = nullptr;
    void* c2 = nullptr;
    void* objB = nullptr;
    if (!IsBadReadPtr(s + 0x10, 8))                       c1 = *(void**)(s + 0x10);
    if (c1 && !IsBadReadPtr((char*)c1 + 0x50, 8))         c2 = *(void**)((char*)c1 + 0x50);
    if (c2 && !IsBadReadPtr((char*)c2 + 0xf0, 8))         objB = *(void**)((char*)c2 + 0xf0);
    void* oB = patchVtableSlot(objB, 13, (void*)&IWN_FlipWrapB, "B[slot13]");
    if (oB) {
        g_flipTrampB = (HRESULT (STDMETHODCALLTYPE*)(void*, void*, void*, void*, void*,
                                                     void*, void*, void*, void*, void*))oB;
        g_flipHooked = 1;
    }
    if (!g_flipHooked) {
        iwnHookLog("dwm: flip hook NOT found (neither branch object patched)");
        return;
    }
    g_flipInstallTick = GetTickCount();
    // one-time map of path A's vtable so the log tells us which class it is
    if (objA && !IsBadReadPtr(objA, 8)) {
        void** vt = *(void***)objA;
        for (int i = 0; vt && !IsBadReadPtr(vt, 18 * 8) && i < 18; i++) {
            HMODULE m = moduleFromPtr(vt[i]);
            char b[128];
            if (m) wsprintfA(b, "dwm: flipA vt[%d] = %p (mod %p rva 0x%lX)",
                             i, vt[i], (void*)m, (unsigned long)((char*)vt[i] - (char*)m));
            else   wsprintfA(b, "dwm: flipA vt[%d] = %p (no module)", i, vt[i]);
            iwnHookLog(b);
        }
    }
}
// Paints the warp into all candidates while dwm is inside PresentInternal,
// i.e. after its render is final and before the real present runs.
// ---- round 71: dwm names its own buffers ------------------------------------
// PDW (legacy PresentDWM) is hooked but gets ZERO calls on this machine - 1909
// presents through CDWMSwapChain::PresentInternal, which we already wrap. The
// wrap's `this` IS a CDWMSwapChain, and dwmcore.pdb gives its own buffer
// accessors (disasm verified: both read this->[0x1A8] then vtable slot 31 of
// that object; back takes an extra int in edx - pass 0):
//   0xC1580 CDWMSwapChain::GetLogicalBackBufferIndex
//   0xC6DD0 CDWMSwapChain::GetLogicalFrontBufferIndex
// Calling them inside the wrapper makes dwm itself name the front surf every
// frame - no offsets, no guessing. Paired with a one-shot scan of the object
// for pointers to our registered faces (index -> texture mapping).
typedef UINT (STDMETHODCALLTYPE* PFN_LBIDX1)(void*);
typedef UINT (STDMETHODCALLTYPE* PFN_LBIDX2)(void*, UINT);
static void* g_lbbiFn = nullptr;   // dwmcore + 0xC1580
static void* g_lfbiFn = nullptr;   // dwmcore + 0xC6DD0

static HRESULT STDMETHODCALLTYPE IWN_PresentWrap(void* self, void* a2, void* a3, void* a4, UINT64 a5) {
    // round 71: dwm names its own buffers (see LBIDX block comment)
    if (g_lfbiFn && self) {
        static volatile LONG s_lbCalls = 0;   // POD statics - no TLS guard
        static volatile LONG s_lbScanSeen = -1;
        __try {
            UINT front = ((PFN_LBIDX1)g_lfbiFn)(self);
            UINT back  = 0xFFFFFFFFu;
            if (g_lbbiFn) back = ((PFN_LBIDX2)g_lbbiFn)(self, 0);
            g_lbFrontLast = (LONG)front;      // read by dumpFacesTick (benign race)
            g_lbBackLast  = (LONG)back;
            LONG c = InterlockedIncrement(&s_lbCalls);
            if (c == 1 || (c & 255) == 0 || (g_quiet && (c & 63) == 0)) {
                char b[128];
                wsprintfA(b, "LBIDX: front=%lu back=%lu self=%p",
                          (unsigned long)front, (unsigned long)back, self);
                iwnHookLog(b);
            }
            // re-arming face-pointer scan: runs whenever the registered target
            // count changes (the first call usually precedes engine attach).
            // Scans self AND the [0x1A8] object (what the accessors index
            // through) 0x800 bytes each, logging EVERY hit with its offset -
            // buffer arrays would show the faces at fixed strides.
            if (g_tgtN > 0 && s_lbScanSeen != g_tgtN) {
                s_lbScanSeen = g_tgtN;
                char b[160];
                int hits = 0;
                for (int t = 0; t < g_tgtN; t++) {
                    if (!g_tgt[t]) continue;
                    for (DWORD off = 0; off + 8 <= 0x800; off += 8) {
                        if (!IsBadReadPtr((char*)self + off, 8) &&
                            *(void* volatile*)((char*)self + off) == (void*)g_tgt[t]) {
                            wsprintfA(b, "LBIDX: tgt%d texture at self+0x%lX",
                                      t, (unsigned long)off);
                            iwnHookLog(b);
                            hits++;
                        }
                    }
                }
                void* oA = IsBadReadPtr((char*)self + 0x1a8, 8)
                         ? nullptr : *(void**)((char*)self + 0x1a8);
                UINT vA = IsBadReadPtr((char*)self + 0x1a0, 4)
                        ? 0 : *(UINT*)((char*)self + 0x1a0);
                wsprintfA(b, "LBIDX: self+1A8=%p self+1A0=%08X", oA, vA);
                iwnHookLog(b);
                if (oA && !IsBadReadPtr(oA, 8)) {
                    for (int t = 0; t < g_tgtN; t++) {
                        if (!g_tgt[t]) continue;
                        for (DWORD off = 8; off + 8 <= 0x800; off += 8) {
                            if (!IsBadReadPtr((char*)oA + off, 8) &&
                                *(void* volatile*)((char*)oA + off) == (void*)g_tgt[t]) {
                                wsprintfA(b, "LBIDX: tgt%d texture at objA+0x%lX",
                                          t, (unsigned long)off);
                                iwnHookLog(b);
                                hits++;
                            }
                        }
                    }
                }
                if (!hits) iwnHookLog("LBIDX: no tgt pointers in self/objA 0x800 window");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    // One-time: try to hook the inner swapchain Present. Success -> the paint
    // moves to IWN_FlipWrap (after the window compose, before the flip).
    // Failure -> keep the legacy entry paint below (wallpaper warp only; the
    // window regions of it are dirty-redrawn straight inside the trampoline).
    // round 75: one-shot wire inspection at wrapper entry. The flip hooks never
    // fire, so FLIP_BODY's inspection never ran - do it here where we KNOW we
    // are inside dwm's real present path. applyWireFromSC replaces the paint
    // target list with dwm's own swapchain buffers (the scanout source).
    if ((LONG)g_stage == IWN_PH_PRESNT && !g_flipInspec) {
        if (InterlockedCompareExchange(&g_flipInspec, 1, 0) == 0)
            IWN_FlipInspect(self);
    }
    // round 83: the r76/r78 dxgi vtable hooks are PROVEN dead on this blt
    // path (slot8 / slot54 / PresentDWM: zero calls over 10 minutes each) and
    // worse, installing them sets g_scHooked which SILENTLY DISABLED the
    // wrapper-entry paint for the whole run (r82: frames=0 draws=0 all 150s).
    // They are no longer installed; PresentInternal entry is the only point
    // dwm actually executes before submitting to scanout.
    BOOL painted = FALSE;
    if (g_ready && g_presentTramp && (LONG)g_stage == IWN_PH_PRESNT && g_ctx && !g_quiet) {
        DWORD t = GetTickCount();
        if (t != g_lastPaintTick) {            // one paint per compose
            g_lastPaintTick = t;
            InterlockedIncrement(&g_frames);
            // round 82: one-shot forensic - which wire buffer holds the desktop?
            iwnProbeWireBuffers(self);
            iwnIdentifySubmit(self);
            __try {
                g_ctx->CopyResource(g_twin, g_target); // fresh COMPLETE frame
                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                for (int i = 0; i < g_tgtN; i++) {
                    if (g_rtvA[i]) g_ctx->ClearRenderTargetView(g_rtvA[i], black);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&g_fail);
                iwnHookLog("dwm: warp save EXCEPTION (present)");
            }
            int before = (int)g_draws;
            paintWarp();
            if ((int)g_draws == before) {
                if (g_fail < 40) iwnHookLog("dwm: warp paint produced no Draw (present)");
            } else {
                painted = TRUE;
                // per-face proof + round 74: TWIN/WARPRT proof for this mode
                static BOOL s_proofB = FALSE;     // POD static - no TLS guard
                if (!s_proofB) {
                    s_proofB = TRUE;
                    readbackTex(g_twin,   "TWIN-B");
                    readbackTex(g_warpRT, "WARPRT-B");
                }
                static BOOL s_proofDone[IWN_MAX_TGT];   // POD zero-init
                for (int i = 0; i < g_tgtN && i < IWN_MAX_TGT; i++) {
                    if (!s_proofDone[i]) {
                        s_proofDone[i] = TRUE;
                        readbackPixel(i, "warp-present");
                    }
                }
                // round 82: hand the warped frame to the buffer dwm is about to
                // submit - resolved with dwm's OWN accessor + logical index.
                iwnBlitToScanout(self);
                // round 86: and to the bind-info resources the kernel
                // compositor actually consumes.
                iwnBlitToBindInfo(self);
            }
        }
    }
    if (painted) iwnForensicsBefore(self);
    HRESULT hr = g_presentTramp(self, a2, a3, a4, a5);
    if (painted) iwnForensicsAfter(self);
    // Immediate post-present restore is INVISIBLE (07:42/08:01 runs: the flip
    // has not scanned out when Present returns). Defer to the next
    // ConfirmFrame, the proven policy: dwm draws window content AFTER CF, so
    // the restore at CF(N+1) cannot clobber fresh renders.
    if (painted) g_pendingRestore = 1;
    return hr;
}

static void restoreAllSurfaces(const char* why) {
    __try {
        for (int i = 0; i < g_tgtN; i++) {
            if (g_tgt[i] && g_tgtDevOk[i]) g_ctx->CopyResource(g_tgt[i], g_twin);
        }
        // This runs once per frame in steady mode; logging every call produced
        // 454 near-identical lines in the 06:41 run. Log every 128th.
        static volatile LONG rlog = 0;
        if ((InterlockedIncrement(&rlog) & 127) == 1) {
            char b[128];
            wsprintfA(b, "dwm: warp surfaces restored (%s) [log every 128]", why);
            iwnHookLog(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        iwnHookLog("dwm: restore EXCEPTION");
    }
}

// --- patch-time thread suspension -------------------------------------------
// The 12-byte inline patch is NOT atomic. A live dwm thread executing the first
// bytes of PresentInternal while we write can fetch a torn instruction stream,
// do `movabs rax,<garbage>; jmp rax` and AV in unknown memory (exactly what WER
// reported for the 00:40 and 00:59 deaths: 0xC0000005, IP 0x1c098070001).
// Suspending every other thread for the few microseconds of the write closes
// that window. A thread suspended while inside fn[0..11] would resume into the
// patch - an accepted residual risk of every inline detour; the window is 12
// bytes of a multi-kilobyte function.
static void suspendOtherThreads(int* suspended, HANDLE* handles, int cap) {
    *suspended = 0;
    DWORD self = GetCurrentThreadId();
    DWORD pid  = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == (DWORD)self) continue;
            if (*suspended >= cap) break;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) != (DWORD)-1) handles[(*suspended)++] = t;
            else CloseHandle(t);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// Returns TRUE when the wrapper detour is live (entry patched + verified).
BOOL DwmWarpInstallPresentHook() {
    if (InterlockedCompareExchange(&g_detourInstalled, 1, 0) != 0)
        return g_presentTramp != nullptr;
    HMODULE h = GetModuleHandleW(L"dwmcore.dll");
    if (!h) { iwnHookLog("dwm: present hook skipped (no dwmcore)"); return FALSE; }
    g_lbbiFn = (BYTE*)h + 0xC1580;    // CDWMSwapChain::GetLogicalBackBufferIndex
    g_lfbiFn = (BYTE*)h + 0xC6DD0;    // CDWMSwapChain::GetLogicalFrontBufferIndex
    BYTE* fn = (BYTE*)h + 0x6C6B0;

    // Trampoline = the 28 displaced prologue bytes + jmp back to fn+0x1C. It is
    // called AS AN ORDINARY FUNCTION POINTER from IWN_PresentWrap, so it must
    // live in RWX memory (00:40 lesson: .data is not executable).
    BYTE* blk = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!blk) { iwnHookLog("dwm: present hook skipped (no RWX mem)"); return FALSE; }
    BYTE* tr = blk;

    CopyMemory(tr, fn, 0x1C);
    {
        BYTE* p = tr + 0x1C;
        *p++ = 0x48; *p++ = 0xB8;
        UINT64 back = (UINT64)(fn + 0x1C);
        CopyMemory(p, &back, 8); p += 8;
        *p++ = 0xFF; *p++ = 0xE0;
    }
    // Verify the trampoline tail before going live: movabs rax,imm64 must hold
    // exactly the return address (the 01:38 run caught a wrong operand only
    // because of byte verification - keep verifying everything handwritten).
    BOOL trOk = tr[0x1C] == 0x48 && tr[0x1D] == 0xB8 &&
                *(UINT64*)(tr + 0x1E) == (UINT64)(fn + 0x1C) &&
                tr[0x26] == 0xFF && tr[0x27] == 0xE0;
    if (!trOk) { iwnHookLog("dwm: present hook skipped (trampoline bytes bad)"); return FALSE; }
    g_presentTramp = (HRESULT (STDMETHODCALLTYPE*)(void*, void*, void*, void*, UINT64))tr;
    g_trampMem = tr;
    FlushInstructionCache(GetCurrentProcess(), tr, 0x28);

    // ---- patch with every other thread suspended ----
    HANDLE th[128]; int ns = 0;
    suspendOtherThreads(&ns, th, 128);
    char b[160];
    wsprintfA(b, "dwm: present hook patching with %d threads suspended", ns);
    iwnHookLog(b);

    BYTE orig[12];
    CopyMemory(orig, fn, 12);
    DWORD old = 0;
    if (!VirtualProtect(fn, 32, PAGE_EXECUTE_READWRITE, &old)) {
        iwnHookLog("dwm: present hook skipped (VirtualProtect denied)");
        for (int i = ns - 1; i >= 0; i--) { ResumeThread(th[i]); CloseHandle(th[i]); }
        return FALSE;
    }
    BYTE patch[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
    UINT64 sa = (UINT64)&IWN_PresentWrap;
    CopyMemory(patch + 2, &sa, 8);
    CopyMemory(fn, patch, 12);
    VirtualProtect(fn, 32, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);

    // verify the patch landed intact
    BYTE now[12];
    CopyMemory(now, fn, 12);
    BOOL patchOk = memcmp(now, patch, 12) == 0;
    for (int i = ns - 1; i >= 0; i--) { ResumeThread(th[i]); CloseHandle(th[i]); }
    wsprintfA(b, "dwm: present hook patch %s (first byte %02X->%02X)",
              patchOk ? "verified" : "MISMATCH", orig[0], now[0]);
    iwnHookLog(b);
    if (!patchOk) {
        // roll back: a torn patch left in place is exactly the crash we fixed
        VirtualProtect(fn, 32, PAGE_EXECUTE_READWRITE, &old);
        CopyMemory(fn, orig, 12);
        VirtualProtect(fn, 32, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 12);
        g_presentTramp = nullptr;
        iwnHookLog("dwm: present hook rolled back (patch did not verify)");
        return FALSE;
    }

    // Restore CFG call-target validity for the patched page. VirtualProtect
    // cycles on a CFG image page can drop the valid-target bitmap; the patched
    // entry is reached through COM vtable indirect calls. Marking every
    // 16-aligned offset on the page valid restores the pre-patch state.
    typedef struct { ULONG Offset; ULONG Flags; } IWN_CFG_TARGET;
    typedef BOOL (WINAPI *PFN_SVCT)(HANDLE, PVOID, SIZE_T, ULONG, IWN_CFG_TARGET*);
    PFN_SVCT svct = (PFN_SVCT)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                             "SetProcessValidCallTargets");
    if (svct) {
        uintptr_t page = (uintptr_t)fn & ~(uintptr_t)0xFFF;
        IWN_CFG_TARGET info[256];
        for (int i = 0; i < 256; i++) { info[i].Offset = (ULONG)(i * 16); info[i].Flags = 1; }
        BOOL ok = svct(GetCurrentProcess(), (PVOID)page, 0x1000, 256, info);
        wsprintfA(b, "dwm: CFG call targets re-validated for page %p (ok=%ld gle=%ld)",
                  (void*)page, (long)ok, (long)GetLastError());
        iwnHookLog(b);
    }
    iwnHookLog("dwm: PresentInternal WRAPPER installed @0x6C6B0 (paint pre-present, restore post-present)");
    return TRUE;
}

// ------------------------------------------------------------ MPO/DirectFlip off
// CMD windows (and video / fullscreen apps) render through hardware overlay
// planes that the GPU scans out directly - they never pass through the
// composite surface we paint, so they stay unwarped. Measured 06:41 run: the
// wallpaper warped, the cmd.exe window did not. dwm_lut's answer, adopted
// verbatim: force COverlayContext::OverlaysEnabled and
// COverlayContext::IsCandidateDirectFlipCompatbile to return FALSE while the
// warp is up. Both are plain predicates, so instead of a detour we overwrite
// the prologue with "xor eax,eax; ret" (3 bytes) and restore the saved
// original bytes at shutdown - no trampoline, no calling convention risk.
// RVAs from dwmcore.pdb (18363, SizeOfImage 0x367000):
//   0x185B6C COverlayContext::OverlaysEnabled
//   0x184D2C COverlayContext::IsCandidateDirectFlipCompatbile
struct MpoPatch { const char* name; DWORD rva; BYTE orig[8]; BOOL installed; };
static MpoPatch g_mpoPatches[2] = {
    { "OverlaysEnabled",                 0x185B6C, {0}, FALSE },
    { "IsCandidateDirectFlipCompatbile", 0x184D2C, {0}, FALSE },
};

static void DwmWarpSetMpoSuppressed(BOOL on) {
    HMODULE h = GetModuleHandleW(L"dwmcore.dll");
    if (!h) { iwnHookLog("dwm: mpo toggle skipped (no dwmcore)"); return; }
    for (int i = 0; i < 2; i++) {
        MpoPatch& m = g_mpoPatches[i];
        if (on == m.installed) continue;              // already in the wanted state
        BYTE* fn = (BYTE*)h + m.rva;
        HANDLE th[128]; int ns = 0;
        suspendOtherThreads(&ns, th, 128);
        DWORD old = 0;
        BOOL ok = VirtualProtect(fn, 16, PAGE_EXECUTE_READWRITE, &old) != 0;
        if (ok && on) {
            CopyMemory(m.orig, fn, 8);                // save before first patch
            const BYTE ret0[3] = { 0x31, 0xC0, 0xC3 };// xor eax,eax ; ret
            CopyMemory(fn, ret0, 3);
            FlushInstructionCache(GetCurrentProcess(), fn, 3);
            m.installed = TRUE;
        } else if (ok) {
            CopyMemory(fn, m.orig, 8);                // exact original bytes back
            FlushInstructionCache(GetCurrentProcess(), fn, 8);
            m.installed = FALSE;
        }
        if (ok) VirtualProtect(fn, 16, old, &old);
        for (int k = ns - 1; k >= 0; k--) { ResumeThread(th[k]); CloseHandle(th[k]); }
        char b[128];
        wsprintfA(b, "dwm: MPO %s %s", m.name,
                  ok ? (on ? "-> forced false" : "-> restored") : "-> VirtualProtect FAILED");
        iwnHookLog(b);
    }
}

static void scRelease();          // fwd: defined with the round-69 PDW block
static void restoreBaseline(const char* why) {
    DwmWarpSetMpoSuppressed(FALSE);   // overlays back on, whatever brought us here
    scRelease();                      // round 69: drop swapchain probe refs
    __try {
        for (int i = 0; i < g_tgtN; i++) {
            if (g_tgt[i]) g_ctx->CopyResource(g_tgt[i], g_twin);
        }
        g_ctx->Flush();
        char b[128];
        wsprintfA(b, "dwm: warp baseline restored to all %d surfaces (%s)", g_tgtN, why);
        iwnHookLog(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        iwnHookLog("dwm: baseline restore EXCEPTION");
    }
}

static void paintAtConfirmFrame(LONG ph) {
    __try {
        // Deferred mode (B) owes a restore from the previous submit; pay it
        // before the fresh snapshot (this clobbers dwm's fresh dirty renders -
        // that is the known 23:18 behaviour, kept as the reference phase).
        if (g_pendingRestore) {
            g_pendingRestore = 0;
            restoreAllSurfaces("deferred");
        }
        // round 78: once the slot54 paint hook owns painting, the CF path only
        // pays the deferred restore - painting here would fight the shared
        // g_lastPaintTick gate and pollute the experiment.
        if (g_dwmPresHooked) return;
        g_ctx->CopyResource(g_twin, g_target); // fresh frame, pre-warp
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (int i = 0; i < g_tgtN; i++) {
            if (g_rtvA[i]) g_ctx->ClearRenderTargetView(g_rtvA[i], black);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        iwnHookLog("dwm: warp save EXCEPTION");
        return;
    }
    int before = (int)g_draws;
    paintWarp();
    if ((int)g_draws == before) {
        if (g_fail < 40) iwnHookLog("dwm: warp paint produced no Draw");
    } else {
        // round 74: first CF-mode paint proof - TWIN (mesh input) and WARPRT
        // (mesh output) split "draw produces nothing" from "input was black"
        static BOOL s_proofA = FALSE;         // POD static - no TLS guard
        if (!s_proofA) {
            s_proofA = TRUE;
            readbackTex(g_twin,   "TWIN-A");
            readbackTex(g_warpRT, "WARPRT-A");
            readbackPixel(0, "warp");
        }
    }
}

// ---- round 69: dxgi CDXGISwapChainDWMLegacy::PresentDWM hook ----------------
// fanxiushu 2025-10 article method, ported to 1909. On 22H2 the composition
// present is COverlayContext::Present (dwmcore) and IDXGISwapChain must be dug
// out of IOverlaySwapChain at offset -280. That function does not exist on
// 18363 (verified against dwmcore.pdb - no COverlayContext::Present symbol),
// but dxgi.pdb (18363) has the legacy path the article also describes:
//   0x1AE30  CDXGISwapChainDWMLegacy::PresentDWM
//   0x20610  CDXGISwapChainDWMLegacy::GetCurrentBackBufferIndex
// On the legacy blt path the wrapper's `this` IS the DXGI swapchain dwm
// composes the desktop into: GetDevice (slot 7) and GetBuffer (slot 9) - same
// positions as IDXGISwapChain, per the article - hand us the very image being
// sent to the display. No offset guessing at all.
// Install: the COM vtables of CDXGISwapChainDWMLegacy are static pointer
// arrays in dxgi.dll's .rdata shared by every instance. Scan .rdata for qwords
// equal to the PresentDWM VA and repoint each at our wrapper - one patch per
// vtable hooks ALL swapchains. Aligned 8-byte store = atomic, same pattern as
// the proven flip-hook vtable patches.
#define IWN_SC_MAX 4
struct IWN_ScInfo {                    // all POD - no dynamic static init
    void*                 self;        // first presenting swapchain instance
    ID3D11Device*         dev;         // owning device (AddRef via GetDevice)
    ID3D11DeviceContext*  ctx;
    ID3D11Texture2D*      tex[IWN_SC_MAX];
    ID3D11Texture2D*      stage;       // CPU_READ readback target
    ID3D11Texture2D*      marker;      // 8x8 marker source on the SC device
    UINT                  w, h;
    int                   n;
};
static IWN_ScInfo        g_sc;                 // zero-initialized
static volatile LONG     g_scInited = 0;
static volatile LONG     g_pdwCalls = 0;
static volatile LONG     g_pdwMarks = 0;
static volatile LONG     g_pdwHooked = 0;
static void*             g_pdwTramp = nullptr; // original PresentDWM
static void*             g_pdwBackIdxFn = nullptr; // dxgi+0x20610
typedef HRESULT (STDMETHODCALLTYPE* PFN_PDW10)(void*, void*, void*, void*, void*,
                                               void*, void*, void*, void*, void*);
typedef ULONG   (STDMETHODCALLTYPE* PFN_SCBACKIDX)(void*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SCGETDEV)(void*, const IID&, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SCGETBUF)(void*, UINT, const IID&, void**);

// One pixel sample out of the live back buffer: THE capture proof - the
// composed desktop (windows, taskbar, clock) read from dwm's own present path.
static void scReadback(ULONG cur, const char* tag) {
    if (!g_sc.stage || (int)cur >= g_sc.n || !g_sc.tex[cur] || !g_sc.ctx) return;
    __try {
        g_sc.ctx->CopyResource(g_sc.stage, g_sc.tex[cur]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_sc.ctx->Map(g_sc.stage, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            BYTE* p = (BYTE*)m.pData;
            unsigned tl  = *(const unsigned*)(p + 4);
            unsigned mid = *(const unsigned*)(p + (size_t)(g_sc.h / 2) * m.RowPitch
                                                + (size_t)(g_sc.w / 2) * 4);
            unsigned br  = *(const unsigned*)(p + (size_t)(g_sc.h - 8) * m.RowPitch
                                                + (size_t)(g_sc.w - 8) * 4);
            char b[160];
            wsprintfA(b, "PDW: %s back=%lu mid=%08X tl=%08X br=%08X", tag,
                      (unsigned long)cur, mid, tl, br);
            iwnHookLog(b);
            g_sc.ctx->Unmap(g_sc.stage, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// One-time per process: identify the swapchain, its device and every buffer.
static void scInspect(void* self) {
    char b[192];
    g_sc.self = self;
    void** vt = *(void***)self;
    if (!vt || IsBadReadPtr(vt, 16 * 8)) { iwnHookLog("PDW: inspect failed (bad vtable)"); return; }
    for (int i = 0; i < 16; i++) {
        HMODULE m = moduleFromPtr(vt[i]);
        if (m) wsprintfA(b, "PDW: vt[%d]=%p mod=%p rva=0x%lX", i, vt[i], (void*)m,
                         (unsigned long)((char*)vt[i] - (char*)m));
        else   wsprintfA(b, "PDW: vt[%d]=%p (no module)", i, vt[i]);
        iwnHookLog(b);
    }
    PFN_SCGETDEV getDev = (PFN_SCGETDEV)vt[7];
    PFN_SCGETBUF getBuf = (PFN_SCGETBUF)vt[9];
    HRESULT hr = getDev(self, __uuidof(ID3D11Device), (void**)&g_sc.dev);
    if (FAILED(hr) || !g_sc.dev) {
        wsprintfA(b, "PDW: GetDevice FAILED hr=0x%lX", (unsigned long)hr);
        iwnHookLog(b);
        return;
    }
    g_sc.dev->GetImmediateContext(&g_sc.ctx);
    wsprintfA(b, "PDW: device=%p ctx=%p (g_dev=%p %s)", (void*)g_sc.dev, (void*)g_sc.ctx,
              (void*)g_dev, g_sc.dev == g_dev ? "SAME" : "DIFFERENT");
    iwnHookLog(b);
    for (UINT i = 0; i < IWN_SC_MAX; i++) {
        ID3D11Texture2D* t = nullptr;
        hr = getBuf(self, i, __uuidof(ID3D11Texture2D), (void**)&t);
        if (FAILED(hr) || !t) break;
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        g_sc.tex[i] = t;              // owned ref
        g_sc.n++;
        wsprintfA(b, "PDW: buf[%u] %ux%u fmt=%d bind=%X misc=%X",
                  (unsigned)i, d.Width, d.Height, d.Format, d.BindFlags, d.MiscFlags);
        iwnHookLog(b);
        if (i == 0) { g_sc.w = d.Width; g_sc.h = d.Height; }
    }
    if (g_pdwBackIdxFn) {
        __try {
            ULONG bi = ((PFN_SCBACKIDX)g_pdwBackIdxFn)(self);
            wsprintfA(b, "PDW: GetCurrentBackBufferIndex=%lu", (unsigned long)bi);
            iwnHookLog(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            iwnHookLog("PDW: GetCurrentBackBufferIndex EXCEPTION");
        }
    }
    // marker source + staging readback, both on the SC device
    D3D11_TEXTURE2D_DESC md = {};
    md.Width = 8; md.Height = 8; md.MipLevels = 1; md.ArraySize = 1;
    md.Format = DXGI_FORMAT_B8G8R8A8_UNORM; md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DEFAULT;
    if (SUCCEEDED(g_sc.dev->CreateTexture2D(&md, nullptr, &g_sc.marker))) {
        D3D11_TEXTURE2D_DESC sd = md;
        sd.Width  = g_sc.w;
        sd.Height = g_sc.h;
        sd.Usage  = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags      = 0;
        if (FAILED(g_sc.dev->CreateTexture2D(&sd, nullptr, &g_sc.stage))) {
            iwnHookLog("PDW: staging create failed");
        }
    } else {
        iwnHookLog("PDW: marker create failed");
    }
    ULONG cur = 0;
    if (g_pdwBackIdxFn) {
        __try { cur = ((PFN_SCBACKIDX)g_pdwBackIdxFn)(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    }
    if ((int)cur < g_sc.n) scReadback(cur, "READBACK");
    wsprintfA(b, "PDW: inspect done n=%d %ux%u", g_sc.n, g_sc.w, g_sc.h);
    iwnHookLog(b);
}

// 8x8 colour block onto the live back buffer. Colour names the buffer:
// 0 RED / 1 LIME / 2 BLUE / 3 YELLOW. Whichever shows up in an external
// screenshot is the buffer dwm really blts to the display. dwm redraws the
// back buffer every present, so no restore is needed - the marker lives one
// frame unless repainted.
static void scMarkBack(ULONG cur) {
    if (!g_sc.marker || !g_sc.ctx || (int)cur >= g_sc.n || !g_sc.tex[cur]) return;
    static const UINT cols[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu };
    UINT px = cols[cur & 3];
    g_sc.ctx->UpdateSubresource(g_sc.marker, 0, nullptr, &px, 32, 0);
    D3D11_BOX bx = { 0, 0, 0, 8, 8, 1 };
    __try {
        g_sc.ctx->CopySubresourceRegion(g_sc.tex[cur], 0, 4, 4, 0, g_sc.marker, 0, &bx);
        InterlockedIncrement(&g_pdwMarks);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_fail); }
}

static HRESULT STDMETHODCALLTYPE IWN_PresentDWMWrap(void* self, void* a2, void* a3, void* a4,
                                                    void* a5, void* a6, void* a7, void* a8,
                                                    void* a9, void* a10) {
    LONG calls = InterlockedIncrement(&g_pdwCalls);
    if (calls == 1) {
        char b[96];
        wsprintfA(b, "PDW: FIRST CALL self=%p tramp=%p", self, g_pdwTramp);
        iwnHookLog(b);
    }
    if (self != g_sc.self && g_sc.self) {
        static volatile LONG s_dupLogged = 0;
        if (InterlockedCompareExchange(&s_dupLogged, 1, 0) == 0) {
            char b[128];
            wsprintfA(b, "PDW: second swapchain self=%p (first=%p) - probe keeps first",
                      self, g_sc.self);
            iwnHookLog(b);
        }
    }
    if (InterlockedCompareExchange(&g_scInited, 1, 0) == 0) scInspect(self);
    ULONG cur = 0;
    if (g_pdwBackIdxFn && g_sc.self == self) {
        __try { cur = ((PFN_SCBACKIDX)g_pdwBackIdxFn)(self); }
        __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    }
    // marker only while the warp session is actually running
    if (g_ready && (LONG)g_stage >= IWN_PH_CF_DEF) scMarkBack(cur);
    if ((calls & 255) == 0) scReadback(cur, "tick");
    return ((PFN_PDW10)g_pdwTramp)(self, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

static void scRelease() {
    if (g_sc.marker) { g_sc.marker->Release(); g_sc.marker = nullptr; }
    if (g_sc.stage)  { g_sc.stage->Release();  g_sc.stage  = nullptr; }
    for (int i = 0; i < IWN_SC_MAX; i++) {
        if (g_sc.tex[i]) { g_sc.tex[i]->Release(); g_sc.tex[i] = nullptr; }
    }
    if (g_sc.ctx) { g_sc.ctx->Release(); g_sc.ctx = nullptr; }
    if (g_sc.dev) { g_sc.dev->Release(); g_sc.dev = nullptr; }
    g_sc.n = 0; g_sc.self = nullptr;
    g_scInited = 0;
}

static void installLegacyPresentHook() {
    char b[160];
    if (g_pdwHooked) return;
    HMODULE h = GetModuleHandleW(L"dxgi.dll");
    if (!h) { iwnHookLog("PDW: install skipped (no dxgi.dll)"); return; }
    BYTE* fn = (BYTE*)h + 0x1AE30;                 // CDXGISwapChainDWMLegacy::PresentDWM
    g_pdwBackIdxFn = (BYTE*)h + 0x20610;           // ..::GetCurrentBackBufferIndex
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)h + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* rs = nullptr; DWORD rsn = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".rdata", 6) == 0) {
            rs = (BYTE*)h + sec[i].VirtualAddress;
            rsn = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (!rs || !rsn) { iwnHookLog("PDW: install skipped (no .rdata)"); return; }
    // patching a live vtable: suspend-free is fine (atomic aligned store), but
    // do it with the same write-verify discipline as every other patch here.
    int patched = 0;
    for (DWORD off = 0; off + 8 <= rsn && patched < 4; off += 8) {
        void** slot = (void**)(rs + off);
        if (IsBadReadPtr(slot, 8) || *slot != (void*)fn) continue;
        DWORD op = 0;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &op)) continue;
        *slot = (void*)&IWN_PresentDWMWrap;
        DWORD op2 = 0;
        VirtualProtect(slot, 8, op, &op2);
        if (*(void* volatile*)slot == (void*)&IWN_PresentDWMWrap) {
            wsprintfA(b, "PDW: vtable slot patched @dxgi+rdata+0x%lX (prev=%p)",
                      (unsigned long)off, fn);
            iwnHookLog(b);
            patched++;
        }
    }
    if (patched) {
        g_pdwTramp = fn;
        g_pdwHooked = 1;
        wsprintfA(b, "PDW: LEGACY PRESENT HOOK LIVE (%d vtable slot%s)", patched,
                  patched == 1 ? "" : "s");
        iwnHookLog(b);
    } else {
        iwnHookLog("PDW: no .rdata slot pointed at PresentDWM - hook NOT installed");
    }
}

// ---- round 76: wire swapchain Present hook ----------------------------------
// Round 75 proved the wire back buffers are BLACK at PresentInternal entry,
// which means dwm's compose->backbuffer copy happens INSIDE the trampoline.
// The only paint point after that copy and before the back->front blit is
// IDXGISwapChain::Present itself (vtable slot 8) - the fanxiushu article's
// exact paint point, and we already hold the wire COM object (g_flipSC).
typedef HRESULT (STDMETHODCALLTYPE* PFN_SCPRESENT)(void*, UINT, UINT);
static PFN_SCPRESENT g_scPresentTramp = nullptr;

static HRESULT STDMETHODCALLTYPE IWN_SCPresentWrap(void* self, UINT sync, UINT flags) {
    InterlockedIncrement(&g_scCalls);
    BOOL painted = FALSE;
    if (g_ready && (LONG)g_stage >= IWN_PH_CF_DEF && g_ctx && !g_quiet && !g_dwmPresHooked) {
        DWORD t = GetTickCount();
        if (t != g_lastPaintTick) {           // one paint per compose
            g_lastPaintTick = t;
            InterlockedIncrement(&g_frames);
            __try {
                g_ctx->CopyResource(g_twin, g_target); // frame BEFORE our paint
                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                for (int i = 0; i < g_tgtN; i++) {
                    if (g_rtvA[i]) g_ctx->ClearRenderTargetView(g_rtvA[i], black);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&g_fail);
                iwnHookLog("dwm: warp save EXCEPTION (sc-present)");
            }
            int before = (int)g_draws;
            paintWarp();
            if ((int)g_draws != before) {
                painted = TRUE;
                static BOOL s_proofC = FALSE;     // POD static - no TLS guard
                if (!s_proofC) {
                    s_proofC = TRUE;
                    readbackTex(g_twin,   "TWIN-C");
                    readbackTex(g_warpRT, "WARPRT-C");
                    readbackPixel(0, "warp-sc");
                }
            }
        }
    }
    HRESULT hr = g_scPresentTramp(self, sync, flags);
    if (painted) g_pendingRestore = 1;   // restore at next CF
    return hr;
}

static void installSCPresentHook() {
    if (g_scHooked) return;
    if (!g_flipSC) { iwnHookLog("dwm: SC hook skipped (no wire swapchain)"); return; }
    void* o = patchVtableSlot((void*)g_flipSC, 8, (void*)&IWN_SCPresentWrap, "SC[slot8]");
    if (o) {
        g_scPresentTramp = (PFN_SCPRESENT)o;
        g_scHooked = 1;
        iwnHookLog("dwm: WIRE PRESENT HOOK LIVE - paint point = back buffer at Present");
    }
}

void DwmWarpBeforeSubmit() {
    if (!g_ready) return;
    DWORD el = GetTickCount() - g_t0;
    if (el > 150000) {
        g_ready = 0;
        g_stage = -1;
        // Hand the desktop back: dwm only redraws dirty regions, so the last
        // warped frame would otherwise sit on screen until a manual refresh.
        restoreBaseline("150s budget spent");
        iwnHookLog("dwm: WARP auto-off (150s budget spent)");
        return;
    }
    LONG ph;
    // round 74 mode-comparison timeline (each mode gets a 30s visibility window
    // judged by the external screenshots):
    //   A  8-30s   CF_DEF paint (23:18-run visible reference), NO wrapper
    //   B 30-60s   PresentInternal wrapper entry paint (round-63 recipe)
    //   C 60-150s  back to CF_DEF (wrapper stays installed but passive)
    // round 82: back to dwm's OWN internal functions (fanxiushu model).
    // The call graph was finally read out of dwmcore:
    //   CComposition::Present (0x77538)
    //     -> CRenderTargetManager::Present (0x7782C)
    //          -> [rt+0x40]->vt[8] = CHwndRenderTarget::Present (0x5B2A0)
    //               -> CHwndRenderTarget::InternalPresent (0x5B328)
    //     -> CDWMSwapChain::PresentInternal (0x6C6B0)      <-- submit point
    //          [self+0x1A8] = the wire DXGI swapchain
    //          link lookup: [[self+0x10]+0x50]+0xF0, vtable slot 13 = Present
    // So PresentInternal entry IS "render finished, not yet scanned out".
    // The buffer to paint is resolved by dwm itself:
    //   idx  = CDWMSwapChain::GetLogicalBackBufferIndex (0xC1580)
    //          = (wire->GetCurrentBackBufferIndex() + off) % [this+0xA0]
    //   tex  = CDWMSwapChain::GetBuffer (0xE6060) -> wire->vt[9](idx, IID_Tex2D)
    // Every earlier round painted either a fingerprint-scanned render target
    // or a blind GetBuffer(0) - never the buffer dwm is about to hand over.
    if (el < 1000) ph = IWN_PH_IDLE;
    else           ph = IWN_PH_PRESNT;

    if (ph != g_lastLoggedStage) {
        g_lastLoggedStage = ph;
        g_stage = ph;
        char b[160];
        const char* what = "?";
        switch (ph) {
            case IWN_PH_IDLE:   what = "passthrough (must look unchanged)"; break;
            case IWN_PH_CF_IMM: what = "CF paint + immediate restore (forcePreserveBB)"; break;
            case IWN_PH_CF_DEF: what = "CF paint + deferred restore (mode A/C)"; break;
            case IWN_PH_PRESNT: what = "present-wrapper paint (mode B, frame final)"; break;
        }
        wsprintfA(b, "dwm: WARP phase %ld @%lums - %s", ph, (unsigned long)el, what);
        iwnHookLog(b);
        // Steady-warp entry: suppress MPO/DirectFlip so overlay-direct windows
        // (cmd.exe, video, fullscreen) are routed back through the composite
        // surface we paint. Restored on every shutdown path via restoreBaseline.
        if ((ph == IWN_PH_CF_DEF || ph == IWN_PH_PRESNT) && !g_presntTried) {
            // r82: MPO/direct-flip suppression is required in BOTH modes - it is
            // what routes overlay windows back through the surface dwm presents.
            DwmWarpSetMpoSuppressed(TRUE);
            installLegacyPresentHook();
        }
        if (ph == IWN_PH_PRESNT && !g_presntTried) {
            // mode B entry: install the wrapper once
            g_presntTried = 1;
            if (DwmWarpInstallPresentHook()) {
                iwnHookLog("dwm: mode B - paint point moved to PresentInternal wrapper");
            } else {
                g_stage = IWN_PH_CF_DEF;
                iwnHookLog("dwm: present wrapper NOT installed - staying on CF deferred-restore mode");
            }
        }
        if (ph == IWN_PH_CF_DEF && g_presntTried) {
            iwnHookLog("dwm: mode C - back to CF paint (wrapper passive)");
        }
    }

    if (ph == IWN_PH_IDLE) return;

    // round 79: context-method observer heartbeat (patch alive + counters).
    // Kept on the common path so it also runs inside the PRESNT window (the
    // stage branch below returns early).
    cpyObserverHeartbeat();

    // round 73/81: periodic cursor position log - pairs the face dumps with
    // the script's cursor walk (is the cursor composed into the surface?)
    // r81: moved to the common path (PRESNT stage no longer entered).
    {
        static DWORD s_curLog = 0;           // POD static - no TLS guard
        DWORD tc = GetTickCount();
        if (tc - s_curLog > 5000) {
            s_curLog = tc;
            POINT p;
            GetCursorPos(&p);
            char b[96];
            wsprintfA(b, "CURSOR x=%ld y=%ld frames=%ld draws=%ld",
                      (long)p.x, (long)p.y, (long)g_frames, (long)g_draws);
            iwnHookLog(b);
        }
    }

    // round 72: quiet dump window. Round 74: opens at el=120s (after the mode
    // comparison); dumpFacesTick itself is scheduled independently so rounds
    // r1/r2 fire inside modes A/B while the warp is live.
    if (g_dwin == 0 && el > 120000) {
        g_dwin = 1;
        g_quiet = 1;
        iwnHookLog("FACE DUMP window open (paint quiet until el=140s)");
    }
    dumpFacesTick(el);

    if ((LONG)g_stage == IWN_PH_PRESNT) {
        // Deferred restore from the previous present (mirrors phase B). Done
        // at CF because dwm draws window content after CF - nothing fresh is
        // clobbered here. Read the corner once before restoring: if our warp
        // is STILL on the surface at the next CF, the present did not consume
        // it and we are painting the wrong texture.
        if (g_pendingRestore) {
            g_pendingRestore = 0;
            if (!g_proofFlip) {
                g_proofFlip = 1;
                readbackPixel(0, "post-flip");
            }
            restoreAllSurfaces("post-flip");
        }
        return;   // painted by the wrapper now
    }

    if (g_pdwHooked) {
        // PDW watchdog: proves the hook sees (or does not see) traffic
        static DWORD s_pdwLastTick = 0;    // POD static - no TLS guard
        DWORD t = GetTickCount();
        if (t - s_pdwLastTick > 10000) {
            s_pdwLastTick = t;
            char pb[128];
            wsprintfA(pb, "PDW: watchdog calls=%ld marks=%ld", (long)g_pdwCalls,
                      (long)g_pdwMarks);
            iwnHookLog(pb);
        }
    }

    // round 78: slot54 paint-hook / PresentDWM observe-hook watchdog. This is
    // the load-bearing signal of the round: dwm=%ld must tick at ~60/s if the
    // legacy present override really is dwm's per-frame present path.
    if (g_dwmPresHooked || g_pdwmObsHooked) {
        static DWORD s_dwmLastTick = 0;    // POD static - no TLS guard
        DWORD t3 = GetTickCount();
        if (t3 - s_dwmLastTick > 10000) {
            s_dwmLastTick = t3;
            char db[128];
            wsprintfA(db, "DWMP: paint-slot=%d calls=%ld obs-slot=%d obs=%ld frames=%ld",
                      g_dwmPresSlot, (long)g_dwmPresCalls, g_pdwmObsSlot,
                      (long)g_pdwmObsCalls, (long)g_frames);
            iwnHookLog(db);
        }
    }

    // round 79: context-method observer heartbeat moved to the common path
    // above (must run inside the PRESNT window too).

    // round 70: catch device removal the moment it happens, with the reason
    if (g_dev && g_ready) {
        static DWORD s_rmLast = 0;         // POD statics - no TLS guard
        static volatile LONG s_rmCnt = 0;
        DWORD t2 = GetTickCount();
        if (t2 - s_rmLast > 2000) {
            s_rmLast = t2;
            __try {
                HRESULT rr = g_dev->GetDeviceRemovedReason();
                if (FAILED(rr) && InterlockedCompareExchange(&s_rmCnt, 1, 0) == 0) {
                    char rb[128];
                    wsprintfA(rb, "dwm: g_dev REMOVED reason=0x%08X (watchdog)", (unsigned)rr);
                    iwnHookLog(rb);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }

    InterlockedIncrement(&g_frames);
    if (!g_quiet) paintAtConfirmFrame(ph);

    if (g_fail > 30) {
        g_ready = 0;
        g_stage = -1;
        restoreBaseline("repeated failures");
        iwnHookLog("dwm: WARP DISABLED after repeated failures - baseline restored");
    }
}

void DwmWarpAfterSubmit() {
    if (!g_ready) return;
    LONG st = g_stage;
    if (st == IWN_PH_CF_IMM) {
        // Hand the surfaces back clean BEFORE dwm renders the next frame's
        // dirty regions, so nothing we painted can feed back into itself.
        restoreAllSurfaces("immediate");
    } else if (st == IWN_PH_CF_DEF) {
        g_pendingRestore = 1;   // restore at next CF: the warp survives one
                                // submit - visible on screen, no accumulation
    }
}

int DwmWarpStage() { return (int)g_stage; }

bool DwmWarpAttach(ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& d) {
    IWNWarpTarget t;
    t.tex = target;
    t.desc = d;
    return DwmWarpAttachTargets(&t, 1);
}

void DwmWarpStatus(char* out, int cap) {
    if (!out || cap < 8) return;
    char a[20], c[20], e[20], g[20];
    f2a(a, g_cTLu); f2a(c, g_cTRu); f2a(e, g_cBLu); f2a(g, g_cBRu);
    wsprintfA(out, "warp stage=%ld frames=%ld draws=%ld fail=%ld surf=%d %ux%u keepTL=%s TR=%s BL=%s BR=%s",
              g_stage, g_frames, g_draws, g_fail, g_tgtN,
              (unsigned)g_rtW, (unsigned)g_rtH, a, c, e, g);
}
