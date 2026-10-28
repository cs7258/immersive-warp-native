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
#define IWN_MAX_TGT 5          // dwm keeps several same-size RT surfaces (rotation)

struct IWNVtx { float x, y, u, v; };

static ID3D11Device*              g_dev = nullptr;      // owned ref (dwm's device)
static ID3D11DeviceContext*       g_ctx = nullptr;      // dwm's immediate context
static ID3D11Texture2D*           g_target = nullptr;   // first candidate (twin source)
static ID3D11Texture2D*           g_tgt[IWN_MAX_TGT];   // ALL screen-size RT candidates
static ID3D11RenderTargetView*    g_rtvA[IWN_MAX_TGT];  // one RTV per candidate
static int                        g_tgtN = 0;           // surfaces painted per frame
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
static DWORD          g_t0 = 0;
static LONG           g_lastLoggedStage = -1;

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
    if (g_rs) { g_rs->Release(); g_rs = nullptr; }
    if (g_bs) { g_bs->Release(); g_bs = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    // g_target is a borrowed alias of g_tgt[0] - its ref is owned by g_tgt[0]
    g_target = nullptr;
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
        for (int i = 0; i < n && g_tgtN < IWN_MAX_TGT; i++) {
            if (!list[i].tex) continue;
            if (!(list[i].desc.BindFlags & D3D11_BIND_RENDER_TARGET)) continue;
            ID3D11RenderTargetView* rv = nullptr;
            HRESULT hr2 = g_dev->CreateRenderTargetView(list[i].tex, nullptr, &rv);
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
    for (int i = 0; i < n && g_tgtN < IWN_MAX_TGT; i++) {
        if (!list[i].tex) continue;
        if (!(list[i].desc.BindFlags & D3D11_BIND_RENDER_TARGET)) continue;
        if (list[i].desc.SampleDesc.Count != 1) continue;
        ID3D11RenderTargetView* rv = nullptr;
        HRESULT hr2 = g_dev->CreateRenderTargetView(list[i].tex, nullptr, &rv);
        if (FAILED(hr2) || !rv) {
            wsprintfA(b, "dwm: warp RTV[%d] hr=0x%08X (skipped)", i, (unsigned)hr2);
            iwnHookLog(b);
            continue;
        }
        list[i].tex->AddRef();
        g_rtvA[g_tgtN] = rv;
        g_tgt[g_tgtN]  = list[i].tex;
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
    wsprintfA(b, "dwm: WARP READY %ux%u fmt=%u - staged rollout starting",
              (unsigned)g_rtW, (unsigned)g_rtH, (unsigned)g_desc.Format);
    iwnHookLog(b);
    return true;
}

// ---------------------------------------------------------------- paint
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
        for (int i = 0; i < g_tgtN; i++) {
            if (!g_rtvA[i]) continue;
            g_ctx->OMSetRenderTargets(1, &g_rtvA[i], nullptr);
            g_ctx->Draw(g_vertCount, 0);
            InterlockedIncrement(&g_draws);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_fail);
        iwnHookLog("dwm: warp paint EXCEPTION");
    }

    stateRestore(&st);
}

// ---------------------------------------------------------------- diagnostics
// One pixel out of a candidate surface, via a staging copy. Never call this from
// the reporter thread: the immediate context is not free-threaded.
static void readbackPixel(int i, const char* tag) {
    if (!g_stageTex || !g_ctx || i < 0 || i >= g_tgtN || !g_tgt[i]) return;
    __try {
        g_ctx->CopyResource(g_stageTex, g_tgt[i]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_ctx->Map(g_stageTex, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            size_t off = (size_t)(g_rtH / 2) * m.RowPitch + (size_t)(g_rtW / 2) * 4;
            unsigned mid = *(const unsigned*)((const BYTE*)m.pData + off);
            // The centre samples nearly the same source uv before and after the
            // warp (corner uv average = centre), so it proves nothing. The
            // top-left corner does: after the warp it shows the desktop pixel
            // from uv~(0.15,0.02), which is a completely different colour.
            size_t off2 = (size_t)32 * m.RowPitch + 32 * 4;
            // "near" is still a legacy MSVC keyword - do not name a variable that
            unsigned px2 = *(const unsigned*)((const BYTE*)m.pData + off2);
            g_ctx->Unmap(g_stageTex, 0);
            char b[192];
            wsprintfA(b, "dwm: READBACK %s tgt=%d mid=%08X corner=%08X", tag, i, mid, px2);
            iwnHookLog(b);
        } else {
            iwnHookLog("dwm: READBACK map failed");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("dwm: READBACK EXCEPTION");
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

// Paints the warp into all candidates while dwm is inside PresentInternal,
// i.e. after its render is final and before the real present runs.
static HRESULT STDMETHODCALLTYPE IWN_PresentWrap(void* self, void* a2, void* a3, void* a4, UINT64 a5) {
    BOOL painted = FALSE;
    if (g_ready && g_presentTramp && (LONG)g_stage == IWN_PH_PRESNT && g_ctx) {
        DWORD t = GetTickCount();
        if (t != g_lastPaintTick) {            // one paint per compose
            g_lastPaintTick = t;
            InterlockedIncrement(&g_frames);
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
                if (!g_proofWarp) {
                    g_proofWarp = 1;
                    readbackPixel(0, "warp-present");
                }
            }
        }
    }
    HRESULT hr = g_presentTramp(self, a2, a3, a4, a5);
    if (painted) restoreAllSurfaces("post-present");
    return hr;
}

static void restoreAllSurfaces(const char* why) {
    __try {
        for (int i = 0; i < g_tgtN; i++) {
            if (g_tgt[i]) g_ctx->CopyResource(g_tgt[i], g_twin);
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

static void restoreBaseline(const char* why) {
    DwmWarpSetMpoSuppressed(FALSE);   // overlays back on, whatever brought us here
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
    } else if (!g_proofWarp) {
        g_proofWarp = 1;
        readbackPixel(0, "warp");
    }
}

void DwmWarpBeforeSubmit() {
    if (!g_ready) return;
    DWORD el = GetTickCount() - g_t0;
    if (el > 75000) {
        g_ready = 0;
        g_stage = -1;
        // Hand the desktop back: dwm only redraws dirty regions, so the last
        // warped frame would otherwise sit on screen until a manual refresh.
        restoreBaseline("75s budget spent");
        iwnHookLog("dwm: WARP auto-off (75s budget spent)");
        return;
    }
    LONG ph;
    // Timeline: passthrough -> CF immediate-restore sanity phase -> steady.
    // At steady entry the PresentInternal WRAPPER is installed; on success the
    // paint point moves there (frame is FINAL there - windows included - which
    // the CF point can never be, 07:00 run: windows stay unwarped because dwm
    // dirty-rect redraws them after our CF paint).
    if (el < 1500)       ph = IWN_PH_IDLE;
    else if (el < 8000)  ph = IWN_PH_CF_IMM;
    else                 ph = IWN_PH_CF_DEF;

    if (ph != g_lastLoggedStage) {
        g_lastLoggedStage = ph;
        g_stage = ph;
        char b[160];
        const char* what = "?";
        switch (ph) {
            case IWN_PH_IDLE:   what = "passthrough (must look unchanged)"; break;
            case IWN_PH_CF_IMM: what = "CF paint + immediate restore (forcePreserveBB)"; break;
            case IWN_PH_CF_DEF: what = "CF paint + deferred restore (fallback mode)"; break;
            case IWN_PH_PRESNT: what = "present-wrapper paint (frame final)"; break;
        }
        wsprintfA(b, "dwm: WARP phase %ld @%lums - %s", ph, (unsigned long)el, what);
        iwnHookLog(b);
        // Steady-warp entry: suppress MPO/DirectFlip so overlay-direct windows
        // (cmd.exe, video, fullscreen) are routed back through the composite
        // surface we paint. Restored on every shutdown path via restoreBaseline.
        if (ph == IWN_PH_CF_DEF) {
            DwmWarpSetMpoSuppressed(TRUE);
            if (DwmWarpInstallPresentHook()) {
                g_stage = IWN_PH_PRESNT;
                iwnHookLog("dwm: paint point moved to PresentInternal wrapper");
            } else {
                iwnHookLog("dwm: present wrapper NOT installed - staying on CF deferred-restore mode");
            }
        }
    }

    if (ph == IWN_PH_IDLE) return;
    if ((LONG)g_stage == IWN_PH_PRESNT) return;   // painted by the wrapper now

    InterlockedIncrement(&g_frames);
    paintAtConfirmFrame(ph);

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
