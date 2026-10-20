#include "Engine.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

Engine& Engine::inst() { static Engine e; return e; }

// ---------------- shaders ----------------
static const char* g_vsSrc = R"(
struct VIn  { float2 src : SRC; float2 dest : DEST; float2 nom : NOM; };
struct VOut { float4 pos : SV_Position; float2 src : TEXCOORD0; float2 nom : TEXCOORD1; };
VOut main(VIn i) {
    VOut o;
    o.pos = float4(i.dest.x * 2 - 1, 1 - i.dest.y * 2, 0, 1);
    o.src = i.src;
    o.nom = i.nom;
    return o;
}
)";

static const char* g_psSrc = R"(
struct VOut { float4 pos : SV_Position; float2 src : TEXCOORD0; float2 nom : TEXCOORD1; };
SamplerState samp : register(s0);
Texture2D tex : register(t0);
cbuffer FrameCB : register(b0) {
    float4 LR;              // leftFade, leftGamma, rightFade, rightGamma
    float4 TB;              // topFade, topGamma, bottomFade, bottomGamma
    float4 advL[8];
    float4 advR[8];
    float4 advT[8];
    float4 advB[8];
    int4 advCounts;         // left, right, top, bottom
    int4 flags;             // useTex, testPattern, maskCount, unused
    float4 masks[8];        // x,y,w,h
    float4 color;           // gammaR, gammaG, gammaB, bright
    float contrast;
    float3 pad;
}
// piecewise interpolation over advanced edge points: x=pos, y=fade, z=gamma
void edgeParams(float4 pts[8], int count, float c, float baseF, float baseG,
                out float fw, out float g) {
    fw = baseF; g = baseG;
    if (count <= 0) return;
    if (c <= pts[0].x) { fw = pts[0].y; g = pts[0].z; return; }
    for (int i = 0; i < count - 1; i++) {
        if (c <= pts[i + 1].x) {
            float t = (c - pts[i].x) / max(pts[i + 1].x - pts[i].x, 1e-5);
            fw = lerp(pts[i].y, pts[i + 1].y, t);
            g = lerp(pts[i].z, pts[i + 1].z, t);
            return;
        }
    }
    fw = pts[count - 1].y; g = pts[count - 1].z;
}
float3 testPattern(float2 p) {
    float2 g = frac(p * float2(12, 8));
    float chk = step(0.5, fmod(floor(p.x * 24) + floor(p.y * 16), 2));
    float3 col = lerp(float3(0.06, 0.06, 0.06), float3(0.38, 0.38, 0.38), chk);
    if (g.x < 0.015 || g.y < 0.015) col = float3(0.85, 0.85, 0.85);
    float2 b = min(p, 1 - p);
    if (min(b.x, b.y) < 0.004) col = float3(1, 0.25, 0.25);
    if (abs(p.x - 0.5) < 0.0015) col = float3(0.15, 1, 0.15);
    return col;
}
float4 main(VOut i) : SV_Target {
    float a = 1;
    float fw, g;
    edgeParams(advL, advCounts.x, i.nom.y, LR.x, LR.y, fw, g);
    if (fw > 0.001 && i.nom.x < fw) a *= pow(saturate(i.nom.x / fw), g);
    edgeParams(advR, advCounts.y, i.nom.y, LR.z, LR.w, fw, g);
    if (fw > 0.001 && i.nom.x > 1 - fw) a *= pow(saturate((1 - i.nom.x) / fw), g);
    edgeParams(advT, advCounts.z, i.nom.x, TB.x, TB.y, fw, g);
    if (fw > 0.001 && i.nom.y < fw) a *= pow(saturate(i.nom.y / fw), g);
    edgeParams(advB, advCounts.w, i.nom.x, TB.z, TB.w, fw, g);
    if (fw > 0.001 && i.nom.y > 1 - fw) a *= pow(saturate((1 - i.nom.y) / fw), g);

    float3 c;
    if (flags.x > 0 && flags.y < 1) c = tex.Sample(samp, i.src).rgb;
    else c = testPattern(i.nom);
    c = pow(saturate(c), color.rgb);
    c = (c - 0.5) * contrast + 0.5;
    c *= color.w;
    for (int k = 0; k < 8; k++) {
        if (k >= flags.z) break;
        float2 d = i.nom - masks[k].xy;
        if (d.x >= 0 && d.y >= 0 && d.x <= masks[k].z && d.y <= masks[k].w) c = 0;
    }
    return float4(saturate(c) * a, 1);
}
)";

static const char* g_vsOvSrc = R"(
struct OIn { float2 pos : POSITION; float4 col : COLOR; };
struct OOut { float4 pos : SV_Position; float4 col : COLOR; };
OOut main(OIn i) { OOut o; o.pos = float4(i.pos, 0, 1); o.col = i.col; return o; }
)";
static const char* g_psOvSrc = R"(
struct OOut { float4 pos : SV_Position; float4 col : COLOR; };
float4 main(OOut i) : SV_Target { return i.col; }
)";

bool Engine::init() {
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&m_factory))) return false;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(m_factory->EnumAdapters1(0, &adapter))) return false;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &m_dev, &fl, &m_ctx);
    adapter->Release();
    if (FAILED(hr)) return false;
    if (!compileShaders()) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 1e9f;
    m_dev->CreateSamplerState(&sd, &m_sampler);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    m_dev->CreateRasterizerState(&rd, &m_raster);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_dev->CreateBlendState(&bd, &m_blendOpaque);
    return true;
}

bool Engine::compileShaders() {
    ID3DBlob* vs = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(g_vsSrc, strlen(g_vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &err)))
        { if (err) err->Release(); return false; }
    m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "SRC",  0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEST", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NOM",  0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_dev->CreateInputLayout(il, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &m_layout);
    vs->Release();

    ID3DBlob* ps = nullptr;
    if (FAILED(D3DCompile(g_psSrc, strlen(g_psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, &err)))
        { if (err) err->Release(); return false; }
    m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
    ps->Release();

    ID3DBlob* vso = nullptr;
    if (FAILED(D3DCompile(g_vsOvSrc, strlen(g_vsOvSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vso, &err)))
        { if (err) err->Release(); return false; }
    m_dev->CreateVertexShader(vso->GetBufferPointer(), vso->GetBufferSize(), nullptr, &m_vsOv);
    D3D11_INPUT_ELEMENT_DESC ilo[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    m_dev->CreateInputLayout(ilo, 2, vso->GetBufferPointer(), vso->GetBufferSize(), &m_layoutOv);
    vso->Release();

    ID3DBlob* pso = nullptr;
    if (FAILED(D3DCompile(g_psOvSrc, strlen(g_psOvSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pso, &err)))
        { if (err) err->Release(); return false; }
    m_dev->CreatePixelShader(pso->GetBufferPointer(), pso->GetBufferSize(), nullptr, &m_psOv);
    pso->Release();
    return true;
}

bool Engine::attachOutput(int displayIdx, HWND hwnd) {
    auto displays = enumerateDisplays();
    if (displayIdx < 0 || displayIdx >= (int)displays.size()) return false;
    auto di = displays[displayIdx];
    auto* oc = new OutputCtx();
    oc->hwnd = hwnd;
    oc->displayIdx = displayIdx;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    oc->w = rc.right - rc.left;
    oc->h = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = oc->w; sd.BufferDesc.Height = oc->h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(m_factory->CreateSwapChain(m_dev, &sd, &oc->swap))) { delete oc; return false; }
    oc->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&oc->backBuf);
    m_dev->CreateRenderTargetView(oc->backBuf, nullptr, &oc->rtv);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16 * 64; // 1KB, >= sizeof(FrameCBData)
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_dev->CreateBuffer(&cb, nullptr, &oc->frameCB);

    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = sizeof(float) * 6 * 256 * 1024;
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_dev->CreateBuffer(&vb, nullptr, &oc->overlayVB);

    createDuplication(*oc, di);
    m_outs.push_back(oc);
    return true;
}

bool Engine::createDuplication(OutputCtx& oc, const DisplayInfo& di) {
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(m_factory->EnumAdapters1(0, &adapter))) return false;
    IDXGIOutput* out = nullptr;
    bool found = false;
    for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_OUTPUT_DESC d{};
        out->GetDesc(&d);
        if (d.DeviceName == di.deviceName) { found = true; break; }
        out->Release(); out = nullptr;
    }
    adapter->Release();
    if (!found || !out) return false;
    IDXGIOutput1* o1 = nullptr;
    if (FAILED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1))) { out->Release(); return false; }
    HRESULT hr = o1->DuplicateOutput(m_dev, &oc.dupl);
    oc.output1 = o1;
    out->Release();
    return SUCCEEDED(hr);
}

void Engine::detachAll() {
    for (auto* oc : m_outs) {
        if (oc->dupl) oc->dupl->Release();
        if (oc->output1) oc->output1->Release();
        if (oc->srv) oc->srv->Release();
        if (oc->frameTex) oc->frameTex->Release();
        if (oc->rtv) oc->rtv->Release();
        if (oc->backBuf) oc->backBuf->Release();
        if (oc->swap) oc->swap->Release();
        if (oc->meshVB) oc->meshVB->Release();
        if (oc->meshIB) oc->meshIB->Release();
        if (oc->frameCB) oc->frameCB->Release();
        if (oc->overlayVB) oc->overlayVB->Release();
        delete oc;
    }
    m_outs.clear();
}

void Engine::shutdown() {
    detachAll();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
    if (m_psOv) m_psOv->Release();
    if (m_vsOv) m_vsOv->Release();
    if (m_layout) m_layout->Release();
    if (m_layoutOv) m_layoutOv->Release();
    if (m_sampler) m_sampler->Release();
    if (m_blendOpaque) m_blendOpaque->Release();
    if (m_raster) m_raster->Release();
    if (m_ctx) m_ctx->Release();
    if (m_dev) m_dev->Release();
    if (m_factory) m_factory->Release();
    m_dev = nullptr; m_ctx = nullptr; m_factory = nullptr;
}

// ---------------- capture ----------------
void Engine::captureFrame(OutputCtx& oc) {
    if (!oc.dupl) return;
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* res = nullptr;
    HRESULT hr = oc.dupl->AcquireNextFrame(0, &info, &res);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        oc.dupl->Release(); oc.dupl = nullptr;
        oc.frameValid = false;
        auto displays = enumerateDisplays();
        if (oc.displayIdx < (int)displays.size())
            createDuplication(oc, displays[oc.displayIdx]);
        return;
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (FAILED(hr)) { oc.frameValid = false; return; }
    if (info.AccumulatedFrames > 0 || info.LastPresentTime.QuadPart != 0 || !oc.frameValid) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
            if (oc.frameTex) {
                D3D11_TEXTURE2D_DESC d1, d2;
                oc.frameTex->GetDesc(&d1); tex->GetDesc(&d2);
                if (d1.Width != d2.Width || d1.Height != d2.Height) {
                    if (oc.srv) { oc.srv->Release(); oc.srv = nullptr; }
                    oc.frameTex->Release(); oc.frameTex = nullptr;
                }
            }
            if (!oc.frameTex) {
                D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
                if (SUCCEEDED(m_dev->CreateTexture2D(&d, nullptr, &oc.frameTex)))
                    m_dev->CreateShaderResourceView(oc.frameTex, nullptr, &oc.srv);
            }
            if (oc.frameTex) { m_ctx->CopyResource(oc.frameTex, tex); oc.frameValid = true; }
            tex->Release();
        }
    }
    oc.dupl->ReleaseFrame();
    res->Release();
}

// ---------------- mesh ----------------
static float catmull(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}
static int cidx(int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }
static float pt(const OutputCfg& c, int r, int col, int axis) {
    return c.points[((size_t)cidx(r, c.rows) * c.cols + cidx(col, c.cols)) * 2 + axis];
}

struct MeshVtx { float sx, sy, dx, dy, nx, ny; };

void Engine::buildMesh(OutputCtx& oc, const OutputCfg& cfg) {
    int rows = cfg.rows, cols = cfg.cols, s = std::max(1, g_cfg.subdivide);
    int RV = (rows - 1) * s + 1, CV = (cols - 1) * s + 1;
    std::vector<MeshVtx> vtx((size_t)RV * CV);
    std::vector<uint32_t> idx;
    idx.reserve((size_t)(RV - 1) * (CV - 1) * 6);
    for (int i = 0; i < RV; i++) {
        float fr = (float)i / s;
        int ri = std::min((int)fr, std::max(rows - 2, 0));
        float tr = rows > 1 ? fr - ri : 0;
        for (int j = 0; j < CV; j++) {
            float fc = (float)j / s;
            int ci = std::min((int)fc, std::max(cols - 2, 0));
            float tc = cols > 1 ? fc - ci : 0;
            float dx, dy;
            if (rows >= 4 && cols >= 4) {
                float c0 = catmull(pt(cfg, ri, ci - 1, 0), pt(cfg, ri, ci, 0), pt(cfg, ri, ci + 1, 0), pt(cfg, ri, ci + 2, 0), tc);
                float c1 = catmull(pt(cfg, ri + 1, ci - 1, 0), pt(cfg, ri + 1, ci, 0), pt(cfg, ri + 1, ci + 1, 0), pt(cfg, ri + 1, ci + 2, 0), tc);
                float c2 = catmull(pt(cfg, ri + 2, ci - 1, 0), pt(cfg, ri + 2, ci, 0), pt(cfg, ri + 2, ci + 1, 0), pt(cfg, ri + 2, ci + 2, 0), tc);
                float c3 = catmull(pt(cfg, ri + 3, ci - 1, 0), pt(cfg, ri + 3, ci, 0), pt(cfg, ri + 3, ci + 1, 0), pt(cfg, ri + 3, ci + 2, 0), tc);
                dx = catmull(c0, c1, c2, c3, tr);
                float r0 = catmull(pt(cfg, ri, ci - 1, 1), pt(cfg, ri, ci, 1), pt(cfg, ri, ci + 1, 1), pt(cfg, ri, ci + 2, 1), tc);
                float r1 = catmull(pt(cfg, ri + 1, ci - 1, 1), pt(cfg, ri + 1, ci, 1), pt(cfg, ri + 1, ci + 1, 1), pt(cfg, ri + 1, ci + 2, 1), tc);
                float r2 = catmull(pt(cfg, ri + 2, ci - 1, 1), pt(cfg, ri + 2, ci, 1), pt(cfg, ri + 2, ci + 1, 1), pt(cfg, ri + 2, ci + 2, 1), tc);
                float r3 = catmull(pt(cfg, ri + 3, ci - 1, 1), pt(cfg, ri + 3, ci, 1), pt(cfg, ri + 3, ci + 1, 1), pt(cfg, ri + 3, ci + 2, 1), tc);
                dy = catmull(r0, r1, r2, r3, tr);
            } else {
                float a = pt(cfg, ri, ci, 0), b = pt(cfg, ri, ci + 1, 0);
                float c = pt(cfg, ri + 1, ci, 0), d = pt(cfg, ri + 1, ci + 1, 0);
                dx = a + (b - a) * tc + (c - a) * tr + (a - b - c + d) * tc * tr;
                float e = pt(cfg, ri, ci, 1), f = pt(cfg, ri, ci + 1, 1);
                float g2 = pt(cfg, ri + 1, ci, 1), h = pt(cfg, ri + 1, ci + 1, 1);
                dy = e + (f - e) * tc + (g2 - e) * tr + (e - f - g2 + h) * tc * tr;
            }
            auto& v = vtx[(size_t)i * CV + j];
            v.sx = (cols > 1) ? fc / (cols - 1) : 0.5f;
            v.sy = (rows > 1) ? fr / (rows - 1) : 0.5f;
            v.dx = clampf(dx, -0.5f, 1.5f);
            v.dy = clampf(dy, -0.5f, 1.5f);
            v.nx = v.sx;
            v.ny = v.sy;
        }
    }
    for (int i = 0; i < RV - 1; i++)
        for (int j = 0; j < CV - 1; j++) {
            uint32_t a = (uint32_t)(i * CV + j), b = a + 1, c = a + CV, d = c + 1;
            uint32_t tri[6] = { a, b, c, b, d, c };
            idx.insert(idx.end(), tri, tri + 6);
        }

    if (!oc.meshVB) {
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = (UINT)(vtx.size() * sizeof(MeshVtx));
        m_dev->CreateBuffer(&bd, nullptr, &oc.meshVB);
        D3D11_BUFFER_DESC ibd = bd;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ibd.ByteWidth = (UINT)(idx.size() * sizeof(uint32_t));
        m_dev->CreateBuffer(&ibd, nullptr, &oc.meshIB);
    }
    D3D11_MAPPED_SUBRESOURCE mp{};
    m_ctx->Map(oc.meshVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    memcpy(mp.pData, vtx.data(), vtx.size() * sizeof(MeshVtx));
    m_ctx->Unmap(oc.meshVB, 0);
    m_ctx->Map(oc.meshIB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    memcpy(mp.pData, idx.data(), idx.size() * sizeof(uint32_t));
    m_ctx->Unmap(oc.meshIB, 0);
    oc.indexCount = (UINT)idx.size();
}

// ---------------- overlay (calibration points) ----------------
struct OvVtx { float x, y; float r, g, b, a; };
static void ovQuad(std::vector<OvVtx>& v, float cx, float cy, float half, float r, float g, float b) {
    float x0 = cx * 2 - 1 - half * 2, x1 = cx * 2 - 1 + half * 2;
    float y0 = 1 - cy * 2 - half * 2, y1 = 1 - cy * 2 + half * 2;
    v.push_back({ x0, y0, r, g, b, 1 }); v.push_back({ x1, y0, r, g, b, 1 }); v.push_back({ x0, y1, r, g, b, 1 });
    v.push_back({ x1, y0, r, g, b, 1 }); v.push_back({ x1, y1, r, g, b, 1 }); v.push_back({ x0, y1, r, g, b, 1 });
}
static void ovLine(std::vector<OvVtx>& v, float x0, float y0, float x1, float y1, float r, float g, float b) {
    v.push_back({ x0 * 2 - 1, 1 - y0 * 2, r, g, b, 1 });
    v.push_back({ x1 * 2 - 1, 1 - y1 * 2, r, g, b, 1 });
}

void Engine::buildOverlay(OutputCtx& oc, const OutputCfg& cfg) {
    std::vector<OvVtx> v;
    for (int r = 0; r < cfg.rows; r++)
        for (int c = 0; c + 1 < cfg.cols; c++) {
            float x0 = cfg.points[((size_t)r * cfg.cols + c) * 2], y0 = cfg.points[((size_t)r * cfg.cols + c) * 2 + 1];
            float x1 = cfg.points[((size_t)r * cfg.cols + c + 1) * 2], y1 = cfg.points[((size_t)r * cfg.cols + c + 1) * 2 + 1];
            ovLine(v, x0, y0, x1, y1, 0.4f, 0.75f, 1.0f);
        }
    for (int c = 0; c < cfg.cols; c++)
        for (int r = 0; r + 1 < cfg.rows; r++) {
            float x0 = cfg.points[((size_t)r * cfg.cols + c) * 2], y0 = cfg.points[((size_t)r * cfg.cols + c) * 2 + 1];
            float x1 = cfg.points[((size_t)(r + 1) * cfg.cols + c) * 2], y1 = cfg.points[((size_t)(r + 1) * cfg.cols + c) * 2 + 1];
            ovLine(v, x0, y0, x1, y1, 0.4f, 0.75f, 1.0f);
        }
    float half = 6.0f / std::max((float)oc.w, 1.0f);
    size_t nPts = cfg.points.size() / 2;
    for (size_t p = 0; p < nPts; p++) {
        float x = cfg.points[p * 2], y = cfg.points[p * 2 + 1];
        ovQuad(v, x, y, half, 0.2f, 1.0f, 0.4f);
    }
    for (int sIdx = 0; sIdx < 4; sIdx++) {
        auto& adv = cfg.adv[sIdx];
        if (g_cfg.advancedBlend && !adv.empty()) {
            for (auto& p : adv) {
                float x, y;
                if (sIdx == 0) { x = p.fade; y = p.pos; }
                else if (sIdx == 1) { x = 1 - p.fade; y = p.pos; }
                else if (sIdx == 2) { x = p.pos; y = p.fade; }
                else { x = p.pos; y = 1 - p.fade; }
                ovQuad(v, x, y, half, 0.3f, 0.55f, 1.0f);
            }
        } else {
            auto& b = cfg.blend[sIdx];
            if (b.fade > 0.001f) {
                float x, y;
                if (sIdx == 0) { x = b.fade; y = 0.5f; }
                else if (sIdx == 1) { x = 1 - b.fade; y = 0.5f; }
                else if (sIdx == 2) { x = 0.5f; y = b.fade; }
                else { x = 0.5f; y = 1 - b.fade; }
                ovQuad(v, x, y, half, 1.0f, 0.3f, 0.3f);
            }
        }
    }
    D3D11_MAPPED_SUBRESOURCE mp{};
    m_ctx->Map(oc.overlayVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    UINT n = (UINT)std::min(v.size(), (size_t)256 * 1024);
    memcpy(mp.pData, v.data(), n * sizeof(OvVtx));
    m_ctx->Unmap(oc.overlayVB, 0);
    oc.overlayVtx = n;
}

// ---------------- constant buffer ----------------
struct FrameCBData {
    float LR[4]; float TB[4];
    float advL[8][4]; float advR[8][4]; float advT[8][4]; float advB[8][4];
    int advCounts[4]; int flags[4];
    float masks[8][4];
    float color[4];
    float contrast; float pad[3];
};
static_assert(sizeof(FrameCBData) % 16 == 0, "cb must be 16-byte aligned");

void Engine::updateFrameCB(OutputCtx& oc, const OutputCfg& cfg) {
    FrameCBData d{};
    d.LR[0] = cfg.blend[0].fade; d.LR[1] = cfg.blend[0].gamma;
    d.LR[2] = cfg.blend[1].fade; d.LR[3] = cfg.blend[1].gamma;
    d.TB[0] = cfg.blend[2].fade; d.TB[1] = cfg.blend[2].gamma;
    d.TB[2] = cfg.blend[3].fade; d.TB[3] = cfg.blend[3].gamma;
    for (int s = 0; s < 4; s++) {
        auto& src = cfg.adv[s];
        float(*dst)[4] = s == 0 ? d.advL : s == 1 ? d.advR : s == 2 ? d.advT : d.advB;
        d.advCounts[s] = (int)std::min(src.size(), (size_t)MAX_ADV);
        for (int i = 0; i < d.advCounts[s]; i++) {
            dst[i][0] = src[i].pos; dst[i][1] = src[i].fade; dst[i][2] = src[i].gamma; dst[i][3] = 0;
        }
    }
    d.flags[0] = oc.frameValid ? 1 : 0;
    d.flags[1] = (g_cfg.testPattern || g_calib.load()) ? 1 : 0;
    d.flags[2] = (int)std::min(cfg.masks.size(), (size_t)MAX_MASKS);
    for (int i = 0; i < d.flags[2]; i++) {
        d.masks[i][0] = cfg.masks[i].x; d.masks[i][1] = cfg.masks[i].y;
        d.masks[i][2] = cfg.masks[i].w; d.masks[i][3] = cfg.masks[i].h;
    }
    d.color[0] = cfg.color.gammaR; d.color[1] = cfg.color.gammaG;
    d.color[2] = cfg.color.gammaB; d.color[3] = cfg.color.bright;
    d.contrast = cfg.color.contrast;

    D3D11_MAPPED_SUBRESOURCE mp{};
    m_ctx->Map(oc.frameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    memcpy(mp.pData, &d, sizeof(d));
    m_ctx->Unmap(oc.frameCB, 0);
}

// ---------------- frame ----------------
void Engine::drawOutput(OutputCtx& oc, const OutputCfg& cfg) {
    float clr[4] = { 0, 0, 0, 1 };
    m_ctx->ClearRenderTargetView(oc.rtv, clr);
    D3D11_VIEWPORT vp{ 0, 0, (FLOAT)oc.w, (FLOAT)oc.h, 0, 1 };
    m_ctx->RSSetState(m_raster);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, &oc.rtv, nullptr);
    m_ctx->OMSetBlendState(m_blendOpaque, nullptr, 0xffffffff);

    if (oc.indexCount) {
        m_ctx->VSSetShader(m_vs, nullptr, 0);
        m_ctx->PSSetShader(m_ps, nullptr, 0);
        ID3D11Buffer* cbs[] = { oc.frameCB };
        m_ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* srvs[] = { oc.srv };
        m_ctx->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samps[] = { m_sampler };
        m_ctx->PSSetSamplers(0, 1, samps);
        UINT stride = sizeof(MeshVtx), off = 0;
        ID3D11Buffer* vbs[] = { oc.meshVB };
        m_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        m_ctx->IASetIndexBuffer(oc.meshIB, DXGI_FORMAT_R32_UINT, 0);
        m_ctx->IASetInputLayout(m_layout);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->DrawIndexed(oc.indexCount, 0, 0);
    }
    if (g_calib.load() && oc.overlayVtx) {
        m_ctx->VSSetShader(m_vsOv, nullptr, 0);
        m_ctx->PSSetShader(m_psOv, nullptr, 0);
        UINT stride = sizeof(OvVtx), off = 0;
        ID3D11Buffer* vbs[] = { oc.overlayVB };
        m_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        m_ctx->IASetInputLayout(m_layoutOv);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->Draw(oc.overlayVtx, 0);
    }
    oc.swap->Present(g_calib.load() ? 0 : 1, 0);
}

void Engine::renderAll() {
    for (auto* oc : m_outs) {
        OutputCfg* cfg = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            auto displays = enumerateDisplays();
            if (oc->displayIdx >= (int)displays.size()) continue;
            for (auto& o : g_cfg.outputs)
                if (o.enabled && o.deviceName == displays[oc->displayIdx].deviceName) { cfg = &o; break; }
        }
        if (!cfg) continue;
        captureFrame(*oc);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            if (oc->meshRev != g_rev.load()) {
                buildMesh(*oc, *cfg);
                oc->meshRev = g_rev.load();
            }
            updateFrameCB(*oc, *cfg);
            if (g_calib.load()) buildOverlay(*oc, *cfg);
        }
        drawOutput(*oc, *cfg);
    }
}
