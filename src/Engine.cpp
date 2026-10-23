#include "Engine.h"
#include <d3dcompiler.h>
#include <shlobj.h>
#include <wincodec.h>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

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
    float pad0;
    float2 imgFit;          // uv scale for aspect-preserving letterbox
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
    float2 uv = (i.src - 0.5) * imgFit + 0.5;   // letterbox: keep source aspect
    if (flags.x > 0 && flags.y < 1) {
        if (any(uv < 0) || any(uv > 1)) c = float3(0, 0, 0);
        else c = tex.Sample(samp, uv).rgb;
    } else c = testPattern(i.nom);
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
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 1e9f;
    // 0 = trilinear, 1/2 = anisotropic (warped meshes sample at steep angles,
    // anisotropy keeps projected grid lines crisp)
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    m_dev->CreateSamplerState(&sd, &m_samplers[0]);
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 4;
    m_dev->CreateSamplerState(&sd, &m_samplers[1]);
    sd.MaxAnisotropy = 8;
    m_dev->CreateSamplerState(&sd, &m_samplers[2]);
    m_sampIdx.store(0);

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

// ---------------- image source (WIC + built-in test library) ----------------
static void dbgLog(const std::string& line);   // defined below

void Engine::setSampling(int q) {
    if (q < 0) q = 0;
    if (q > 2) q = 2;
    m_sampIdx.store(q);
}

void Engine::clearImage() {    if (m_imgSRV) m_imgGarbageSRV.push_back(m_imgSRV);
    if (m_imgTex) m_imgGarbageTex.push_back(m_imgTex);
    m_imgSRV = nullptr; m_imgTex = nullptr; m_imgW = m_imgH = 0;
    m_testImg = -1;
}

// upload a BGRA buffer as the active image source. The previous image is
// retired (not released) so the render thread can never touch a freed
// object mid-frame.
bool Engine::uploadImage(UINT w, UINT h, const std::vector<BYTE>& bgra, const char* tag) {
    if (!m_dev || !w || !h || bgra.size() < (size_t)w * h * 4) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = bgra.data();
    sd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_dev->CreateTexture2D(&td, &sd, &tex))) { dbgLog("img: CreateTexture2D failed"); return false; }
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(m_dev->CreateShaderResourceView(tex, nullptr, &srv))) {
        tex->Release(); dbgLog("img: CreateSRV failed"); return false;
    }
    if (m_imgSRV) m_imgGarbageSRV.push_back(m_imgSRV);
    if (m_imgTex) m_imgGarbageTex.push_back(m_imgTex);
    m_imgSRV = srv; m_imgTex = tex; m_imgW = (int)w; m_imgH = (int)h;
    dbgLog(std::string("img: loaded ") + tag + " " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

// Decodes PNG / JPG / BMP / TIFF / GIF / ICO into a BGRA texture. Empty path
// clears the image (falls back to desktop capture / test pattern).
bool Engine::loadImage(const std::wstring& path) {
    if (!m_dev) return false;
    if (path.empty()) { clearImage(); return true; }

    static bool comInit = false;
    if (!comInit) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); comInit = true; }

    IWICImagingFactory* fac = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), (void**)&fac);
    if (FAILED(hr)) { dbgLog("img: WIC factory failed"); return false; }

    IWICBitmapDecoder* dec = nullptr;
    hr = fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr)) { fac->Release(); dbgLog("img: decode failed"); return false; }

    IWICBitmapFrameDecode* frm = nullptr;
    hr = dec->GetFrame(0, &frm);
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(hr)) hr = fac->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frm, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                              nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    std::vector<BYTE> buf;
    if (SUCCEEDED(hr)) {
        hr = conv->GetSize(&w, &h);
        if (SUCCEEDED(hr) && w && h) {
            buf.resize((size_t)w * h * 4);
            hr = conv->CopyPixels(nullptr, w * 4, (UINT)buf.size(), buf.data());
        } else hr = E_FAIL;
    }
    if (conv) conv->Release();
    if (frm) frm->Release();
    if (dec) dec->Release();
    fac->Release();
    if (FAILED(hr)) { dbgLog("img: convert/copy failed"); return false; }

    m_testImg = -1;   // a real file replaces any built-in pattern
    return uploadImage(w, h, buf, "file");
}

// Built-in test image library, generated procedurally (no external files).
bool Engine::setTestImage(int id) {
    if (!m_dev) return false;
    if (id < 0) { clearImage(); return true; }
    if (id > 3) id = 3;

    const UINT W = 1280, H = 720;
    std::vector<BYTE> buf((size_t)W * H * 4);
    auto put = [&](UINT x, UINT y, BYTE r, BYTE g, BYTE b) {
        BYTE* p = &buf[((size_t)y * W + x) * 4];
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    };

    switch (id) {
    case 0: {  // color bars: 8 vertical bars + gray ramp bottom third + grid
        static const BYTE bars[8][3] = {
            {255,255,255},{255,255,0},{0,255,255},{0,255,0},
            {255,0,255},{255,0,0},{0,0,255},{0,0,0} };
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) {
                BYTE r, g, b;
                if (y < H * 2 / 3) {
                    int k = x * 8 / W; if (k > 7) k = 7;
                    r = bars[k][0]; g = bars[k][1]; b = bars[k][2];
                } else {
                    int vi = x * 256 / W; if (vi > 255) vi = 255;
                    r = g = b = (BYTE)vi;
                }
                if (x % 80 == 0 || y % 80 == 0) { r = g = b = 255; }
                if (x > W / 2 - 2 && x < W / 2 + 2 && y > H / 2 - 40 && y < H / 2 + 40) { r = 255; g = 60; b = 60; }
                if (y > H / 2 - 2 && y < H / 2 + 2 && x > W / 2 - 40 && x < W / 2 + 40) { r = 255; g = 60; b = 60; }
                if (x < 2 || y < 2 || x > W - 3 || y > H - 3) { r = 0; g = 255; b = 0; }
                put(x, y, r, g, b);
            }
        break;
    }
    case 1: {  // checkerboard 80px black/white + green border
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) {
                bool w = ((x / 80) + (y / 80)) % 2 == 0;
                if (x < 2 || y < 2 || x > W - 3 || y > H - 3) put(x, y, 0, 255, 0);
                else put(x, y, w ? 255 : 0, w ? 255 : 0, w ? 255 : 0);
            }
        break;
    }
    case 2: {  // gray ramp + black/white reference blocks
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) {
                BYTE v = 0;
                int vi = x * 256 / W; if (vi > 255) vi = 255;
                v = (BYTE)vi;
                if (y < H / 6 && x < W / 8) v = 0;
                if (y < H / 6 && x > W - W / 8) v = 255;
                put(x, y, v, v, v);
            }
        break;
    }
    default: {  // identity grid: 10x12 control grid, center crosshair, border
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++) {
                BYTE r = 30, g = 30, b = 30;
                float cx = (float)x * 12 / W, cy = (float)y * 10 / H;
                if (std::abs(cx - std::round(cx)) < 0.02f ||
                    std::abs(cy - std::round(cy)) < 0.02f) { r = 255; g = 255; b = 255; }
                if (std::abs(cx - std::round(cx)) < 0.05f &&
                    std::abs(cy - std::round(cy)) < 0.05f) { r = 80; g = 160; b = 255; }
                if (x > W / 2 - 2 && x < W / 2 + 2) { r = 255; g = 60; b = 60; }
                if (y > H / 2 - 2 && y < H / 2 + 2) { r = 255; g = 60; b = 60; }
                if (x < 2 || y < 2 || x > W - 3 || y > H - 3) { r = 0; g = 255; b = 0; }
                put(x, y, r, g, b);
            }
        break;
    }
    }
    static const char* names[4] = { "colorbars", "checkerboard", "grayramp", "grid" };
    if (!uploadImage(W, H, buf, names[id])) return false;
    m_testImg = id;
    return true;
}

bool engineLoadImage(const std::wstring& path) { return Engine::inst().loadImage(path); }
bool engineHasImage() { return Engine::inst().hasImage(); }
bool engineSetTestImage(int id) { return Engine::inst().setTestImage(id); }

// ---------------- debug log (%APPDATA%\ImmersiveWarpNative\debug.log) ----------------
static void dbgLog(const std::string& line) { dbgLogLine(line); }

void dbgLogLine(const std::string& line) {
    static std::ofstream* f = nullptr;
    if (!f) {
        wchar_t path[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
        std::wstring dir = std::wstring(path) + L"\\ImmersiveWarpNative";
        CreateDirectoryW(dir.c_str(), nullptr);
        f = new std::ofstream(dir + L"\\debug.log", std::ios::app);
    }
    if (*f) {
        SYSTEMTIME st{}; GetLocalTime(&st);
        char ts[40];
        sprintf_s(ts, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        *f << ts << line << "\n";
        f->flush();
    }
}
static std::string rectStr(const RECT& r) {
    char b[96];
    sprintf_s(b, "(%ld,%ld)-(%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
        r.right - r.left, r.bottom - r.top);
    return b;
}

bool Engine::attachOutput(int displayIdx, HWND hwnd) {
    auto displays = enumerateDisplays();
    if (displayIdx < 0 || displayIdx >= (int)displays.size()) return false;
    auto di = displays[displayIdx];
    auto oc = std::make_shared<OutputCtx>();
    oc->hwnd = hwnd;
    oc->displayIdx = displayIdx;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    oc->w = rc.right - rc.left;
    oc->h = rc.bottom - rc.top;
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hmon, &mi);
    dbgLog("attach: displayIdx=" + std::to_string(displayIdx) +
           " client=" + rectStr(rc) + " monitor=" + rectStr(mi.rcMonitor));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = oc->w; sd.BufferDesc.Height = oc->h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(m_factory->CreateSwapChain(m_dev, &sd, &oc->swap))) return false;
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

    createDuplication(*oc);
    dbgLog(std::string("attach res: swap=") + (oc->swap ? "ok" : "NULL") +
           " backBuf=" + (oc->backBuf ? "ok" : "NULL") +
           " rtv=" + (oc->rtv ? "ok" : "NULL") +
           " frameCB=" + (oc->frameCB ? "ok" : "NULL") +
           " overlayVB=" + (oc->overlayVB ? "ok" : "NULL"));
    {
        std::lock_guard<std::mutex> lk(m_outsMutex);
        m_outs.push_back(oc);
    }
    return true;
}

// Duplicate the desktop of the first display that is NOT itself an enabled
// output. Capturing our own fullscreen output window would create an infinite
// feedback loop (recursion), so on a single-display setup there is no capture
// at all and the output falls back to the test pattern / black.
// Requires g_cfgMutex to be held by the caller.
bool Engine::createDuplication(OutputCtx& oc) {
    auto displays = enumerateDisplays();
    const DisplayInfo* src = nullptr;
    auto isOutputDisplay = [](const DisplayInfo& d) {
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == d.deviceName) return true;
        return false;
    };
    if (g_previewMode.load() && oc.displayIdx >= 0 && oc.displayIdx < (int)displays.size()) {
        // windowed preview: the output window is small and marked
        // WDA_EXCLUDEFROMCAPTURE, so capturing its own display does NOT recurse
        if (g_previewNoCapture.load()) {
            dbgLog("dupl: preview mode but capture exclusion unavailable -> no capture");
            return false;
        }
        src = &displays[oc.displayIdx];
    }
    // explicit source from the console "Source:" combo / CMDNET SET source=
    if (!src && !g_cfg.sourceDev.empty()) {
        for (auto& d : displays) {
            if (!sameDisplayName(d.deviceName, g_cfg.sourceDev)) continue;
            if (isOutputDisplay(d)) {
                // capturing a display that also shows an output would film
                // ourselves -> fall back to auto instead of a feedback loop
                dbgLog("dupl: requested source is an enabled output -> auto instead");
                break;
            }
            src = &d;
            dbgLog("dupl: explicit source accepted, display index " +
                   std::to_string((int)(&d - displays.data())));
            break;
        }
        if (!src) dbgLog("dupl: requested source not found (or rejected), falling back to auto");
    }
    for (auto& d : displays) {
        if (src) break;
        if (!isOutputDisplay(d)) { src = &d; break; }
    }
    if (!src) {
        dbgLog("dupl: no source display available (all displays are outputs)");
        return false;
    }
    // The source may sit on any adapter (multi-GPU laptops): walk all of them
    // instead of assuming adapter 0.
    IDXGIOutput* out = nullptr;
    IDXGIAdapter1* srcAdapter = nullptr;
    for (UINT a = 0; !out && a < 8; a++) {
        IDXGIAdapter1* ad = nullptr;
        if (m_factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT i = 0;; i++) {
            IDXGIOutput* o = nullptr;
            if (ad->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            if (d.DeviceName == src->deviceName) { out = o; srcAdapter = ad; break; }
            o->Release();
        }
        if (!srcAdapter) ad->Release();
    }
    if (!out) return false;
    IDXGIOutput1* o1 = nullptr;
    if (FAILED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1))) {
        out->Release(); srcAdapter->Release(); return false;
    }
    HRESULT hr = o1->DuplicateOutput(m_dev, &oc.dupl);
    oc.output1 = o1;
    out->Release();
    srcAdapter->Release();
    dbgLog("dupl: source ok hr=0x" + std::to_string((unsigned long)hr));
    return SUCCEEDED(hr);
}

void Engine::resizeSwapChain(OutputCtx& oc, UINT w, UINT h) {
    if (oc.rtv) { oc.rtv->Release(); oc.rtv = nullptr; }
    if (oc.backBuf) { oc.backBuf->Release(); oc.backBuf = nullptr; }
    if (SUCCEEDED(oc.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        oc.w = w; oc.h = h;
        if (SUCCEEDED(oc.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&oc.backBuf)))
            m_dev->CreateRenderTargetView(oc.backBuf, nullptr, &oc.rtv);
        dbgLog("swapchain resized to " + std::to_string(w) + "x" + std::to_string(h));
    } else {
        dbgLog("swapchain ResizeBuffers FAILED " + std::to_string(w) + "x" + std::to_string(h));
    }
}

void Engine::detachAll() {
    // shared_ptr: each ctx is released once the render thread's in-flight
    // frame snapshot drops its reference — never while it is being drawn
    std::lock_guard<std::mutex> lk(m_outsMutex);
    m_outs.clear();
}

void Engine::shutdown() {
    detachAll();
    for (auto* s : m_imgGarbageSRV) s->Release();
    m_imgGarbageSRV.clear();
    for (auto* t : m_imgGarbageTex) t->Release();
    m_imgGarbageTex.clear();
    if (m_imgSRV) { m_imgSRV->Release(); m_imgSRV = nullptr; }
    if (m_imgTex) { m_imgTex->Release(); m_imgTex = nullptr; }
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
    if (m_psOv) m_psOv->Release();
    if (m_vsOv) m_vsOv->Release();
    if (m_layout) m_layout->Release();
    if (m_layoutOv) m_layoutOv->Release();
    if (m_samplers[0]) m_samplers[0]->Release();
    if (m_samplers[1]) m_samplers[1]->Release();
    if (m_samplers[2]) m_samplers[2]->Release();
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
    // Ctrl+F3 snapshot: stop acquiring so the last frame stays on screen.
    // Alignment on a frozen image is far steadier than on live desktop motion.
    if (g_snapshot.load() && oc.frameValid) return;
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* res = nullptr;
    HRESULT hr = oc.dupl->AcquireNextFrame(0, &info, &res);
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        oc.dupl->Release(); oc.dupl = nullptr;
        oc.frameValid = false;
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        createDuplication(oc);   // re-pick source (topology may have changed)
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
    // defensive: fall back to identity grid if points vector is missing/short
    size_t need = (size_t)c.rows * c.cols * 2;
    if (c.points.size() < need) {
        float rr = (c.rows > 1) ? (float)cidx(r, c.rows) / (c.rows - 1) : 0.5f;
        float cc = (c.cols > 1) ? (float)cidx(col, c.cols) / (c.cols - 1) : 0.5f;
        return axis == 0 ? cc : rr;
    }
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
                // Catmull-Rom: outer lerp must return ROW ri at tr=0 and ROW ri+1
                // at tr=1, so the four rows are (ri-1, ri, ri+1, ri+2)
                float c0 = catmull(pt(cfg, ri - 1, ci - 1, 0), pt(cfg, ri - 1, ci, 0), pt(cfg, ri - 1, ci + 1, 0), pt(cfg, ri - 1, ci + 2, 0), tc);
                float c1 = catmull(pt(cfg, ri, ci - 1, 0), pt(cfg, ri, ci, 0), pt(cfg, ri, ci + 1, 0), pt(cfg, ri, ci + 2, 0), tc);
                float c2 = catmull(pt(cfg, ri + 1, ci - 1, 0), pt(cfg, ri + 1, ci, 0), pt(cfg, ri + 1, ci + 1, 0), pt(cfg, ri + 1, ci + 2, 0), tc);
                float c3 = catmull(pt(cfg, ri + 2, ci - 1, 0), pt(cfg, ri + 2, ci, 0), pt(cfg, ri + 2, ci + 1, 0), pt(cfg, ri + 2, ci + 2, 0), tc);
                dx = catmull(c0, c1, c2, c3, tr);
                float r0 = catmull(pt(cfg, ri - 1, ci - 1, 1), pt(cfg, ri - 1, ci, 1), pt(cfg, ri - 1, ci + 1, 1), pt(cfg, ri - 1, ci + 2, 1), tc);
                float r1 = catmull(pt(cfg, ri, ci - 1, 1), pt(cfg, ri, ci, 1), pt(cfg, ri, ci + 1, 1), pt(cfg, ri, ci + 2, 1), tc);
                float r2 = catmull(pt(cfg, ri + 1, ci - 1, 1), pt(cfg, ri + 1, ci, 1), pt(cfg, ri + 1, ci + 1, 1), pt(cfg, ri + 1, ci + 2, 1), tc);
                float r3 = catmull(pt(cfg, ri + 2, ci - 1, 1), pt(cfg, ri + 2, ci, 1), pt(cfg, ri + 2, ci + 1, 1), pt(cfg, ri + 2, ci + 2, 1), tc);
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

    // diagnostics: dest-coordinate extremes pin down any fill/offset bug
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (auto& v : vtx) {
        minX = std::min(minX, v.dx); maxX = std::max(maxX, v.dx);
        minY = std::min(minY, v.dy); maxY = std::max(maxY, v.dy);
    }
    dbgLog("mesh: rows=" + std::to_string(cfg.rows) + " cols=" + std::to_string(cfg.cols) +
           " sub=" + std::to_string(g_cfg.subdivide) + " pts=" + std::to_string(cfg.points.size()) +
           " destX=[" + std::to_string(minX) + "," + std::to_string(maxX) +
           "] destY=[" + std::to_string(minY) + "," + std::to_string(maxY) + "]");
    if (cfg.points.size() >= 4) {
        char pb[128];
        sprintf_s(pb, "mesh: pt[0]=(%.4f,%.4f) pt[1]=(%.4f,%.4f) pt[last]=(%.4f,%.4f)",
            cfg.points[0], cfg.points[1], cfg.points[2], cfg.points[3],
            cfg.points[cfg.points.size() - 2], cfg.points[cfg.points.size() - 1]);
        dbgLog(pb);
    }
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
    std::vector<OvVtx> lines, tris;
    bool blendMode = g_calibMode.load() == 1;
    if (!blendMode) {
        // geometrical correction: blue mesh lines + green control points
        for (int r = 0; r < cfg.rows; r++)
            for (int c = 0; c + 1 < cfg.cols; c++) {
                float x0 = cfg.points[((size_t)r * cfg.cols + c) * 2], y0 = cfg.points[((size_t)r * cfg.cols + c) * 2 + 1];
                float x1 = cfg.points[((size_t)r * cfg.cols + c + 1) * 2], y1 = cfg.points[((size_t)r * cfg.cols + c + 1) * 2 + 1];
                ovLine(lines, x0, y0, x1, y1, 0.4f, 0.75f, 1.0f);
            }
        for (int c = 0; c < cfg.cols; c++)
            for (int r = 0; r + 1 < cfg.rows; r++) {
                float x0 = cfg.points[((size_t)r * cfg.cols + c) * 2], y0 = cfg.points[((size_t)r * cfg.cols + c) * 2 + 1];
                float x1 = cfg.points[((size_t)(r + 1) * cfg.cols + c) * 2], y1 = cfg.points[((size_t)(r + 1) * cfg.cols + c) * 2 + 1];
                ovLine(lines, x0, y0, x1, y1, 0.4f, 0.75f, 1.0f);
            }
        float half = 6.0f / std::max((float)oc.w, 1.0f);
        size_t nPts = cfg.points.size() / 2;
        for (size_t p = 0; p < nPts; p++) {
            float x = cfg.points[p * 2], y = cfg.points[p * 2 + 1];
            ovQuad(tris, x, y, half, 0.2f, 1.0f, 0.4f);
        }
    }
    // blend handles: visible in blend mode even when parked at the edge;
    // in warp mode only shown when actually configured (fade > 0)
    float half = 6.0f / std::max((float)oc.w, 1.0f);
    for (int sIdx = 0; sIdx < 4; sIdx++) {
        // an edge switched off with F12 is neither blended nor shown, so the
        // user can see exactly which edges are active for this projector
        if (blendMode && !(cfg.edges & (1 << sIdx))) continue;
        auto& adv = cfg.adv[sIdx];
        if (g_cfg.advancedBlend && !adv.empty()) {
            for (auto& p : adv) {
                float x, y;
                if (sIdx == 0) { x = p.fade; y = p.pos; }
                else if (sIdx == 1) { x = 1 - p.fade; y = p.pos; }
                else if (sIdx == 2) { x = p.pos; y = p.fade; }
                else { x = p.pos; y = 1 - p.fade; }
                ovQuad(tris, x, y, half, 0.3f, 0.55f, 1.0f);
            }
        } else {
            auto& b = cfg.blend[sIdx];
            if (blendMode || b.fade > 0.001f) {
                float x, y;
                if (sIdx == 0) { x = b.fade; y = 0.5f; }
                else if (sIdx == 1) { x = 1 - b.fade; y = 0.5f; }
                else if (sIdx == 2) { x = 0.5f; y = b.fade; }
                else { x = 0.5f; y = 1 - b.fade; }
                // blend-width indicator line across the edge
                float lx0, ly0, lx1, ly1;
                if (sIdx == 0) { lx0 = b.fade; ly0 = 0; lx1 = b.fade; ly1 = 1; }
                else if (sIdx == 1) { lx0 = 1 - b.fade; ly0 = 0; lx1 = 1 - b.fade; ly1 = 1; }
                else if (sIdx == 2) { lx0 = 0; ly0 = b.fade; lx1 = 1; ly1 = b.fade; }
                else { lx0 = 0; ly0 = 1 - b.fade; lx1 = 1; ly1 = 1 - b.fade; }
                ovLine(lines, lx0, ly0, lx1, ly1, 1.0f, 0.45f, 0.2f);
                ovQuad(tris, x, y, half, 1.0f, 0.3f, 0.3f);
            }
        }
    }
    // layout: [line verts][triangle verts] — drawn with separate topologies
    std::vector<OvVtx> all;
    all.reserve(lines.size() + tris.size());
    all.insert(all.end(), lines.begin(), lines.end());
    all.insert(all.end(), tris.begin(), tris.end());
    D3D11_MAPPED_SUBRESOURCE mp{};
    m_ctx->Map(oc.overlayVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    UINT n = (UINT)std::min(all.size(), (size_t)256 * 1024);
    memcpy(mp.pData, all.data(), n * sizeof(OvVtx));
    m_ctx->Unmap(oc.overlayVB, 0);
    oc.overlayLineVtx = (UINT)std::min(lines.size(), (size_t)n);
    oc.overlayTriVtx = (UINT)std::min(tris.size(), (size_t)std::max(0, (int)n - (int)lines.size()));
}

// ---------------- constant buffer ----------------
struct FrameCBData {
    float LR[4]; float TB[4];
    float advL[8][4]; float advR[8][4]; float advT[8][4]; float advB[8][4];
    int advCounts[4]; int flags[4];
    float masks[8][4];
    float color[4];
    float contrast; float pad0; float imgFit[2];
};
static_assert(sizeof(FrameCBData) % 16 == 0, "cb must be 16-byte aligned");

void Engine::updateFrameCB(OutputCtx& oc, const OutputCfg& cfg) {
    FrameCBData d{};
    // edges masked off by F12 contribute zero fade => nothing is blended there
    d.LR[0] = (cfg.edges & 1) ? cfg.blend[0].fade : 0.0f; d.LR[1] = cfg.blend[0].gamma;
    d.LR[2] = (cfg.edges & 2) ? cfg.blend[1].fade : 0.0f; d.LR[3] = cfg.blend[1].gamma;
    d.TB[0] = (cfg.edges & 4) ? cfg.blend[2].fade : 0.0f; d.TB[1] = cfg.blend[2].gamma;
    d.TB[2] = (cfg.edges & 8) ? cfg.blend[3].fade : 0.0f; d.TB[3] = cfg.blend[3].gamma;
    for (int s = 0; s < 4; s++) {
        auto& src = cfg.adv[s];
        float(*dst)[4] = s == 0 ? d.advL : s == 1 ? d.advR : s == 2 ? d.advT : d.advB;
        d.advCounts[s] = (cfg.edges & (1 << s)) ? (int)std::min(src.size(), (size_t)MAX_ADV) : 0;
        for (int i = 0; i < d.advCounts[s]; i++) {
            dst[i][0] = src[i].pos; dst[i][1] = src[i].fade; dst[i][2] = src[i].gamma; dst[i][3] = 0;
        }
    }
    bool useImg = g_cfg.useImage && m_imgSRV != nullptr;
    d.flags[0] = (useImg || oc.frameValid) ? 1 : 0;
    // test pattern overrides capture, but a loaded image keeps priority so that
    // geometry can be aligned on real content. Calibration no longer forces the
    // pattern on: aligning against live desktop content is the whole point of
    // the snapshot-freeze workflow, so the checkbox alone decides.
    d.flags[1] = (!useImg && g_cfg.testPattern) ? 1 : 0;
    // aspect-preserving letterbox: uv scale = output extent / drawn image extent
    d.imgFit[0] = 1.0f; d.imgFit[1] = 1.0f;
    if (useImg && m_imgW > 0 && m_imgH > 0 && oc.w && oc.h) {
        float w0 = (float)oc.w, h0 = (float)oc.h;
        float s = std::min(w0 / (float)m_imgW, h0 / (float)m_imgH);
        if (s > 0) { d.imgFit[0] = w0 / ((float)m_imgW * s); d.imgFit[1] = h0 / ((float)m_imgH * s); }
    }
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
        ID3D11ShaderResourceView* srvs[] = {
            (g_cfg.useImage && m_imgSRV) ? m_imgSRV : oc.srv };
        m_ctx->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samps[] = { m_samplers[m_sampIdx.load()] };
        m_ctx->PSSetSamplers(0, 1, samps);
        UINT stride = sizeof(MeshVtx), off = 0;
        ID3D11Buffer* vbs[] = { oc.meshVB };
        m_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        m_ctx->IASetIndexBuffer(oc.meshIB, DXGI_FORMAT_R32_UINT, 0);
        m_ctx->IASetInputLayout(m_layout);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->DrawIndexed(oc.indexCount, 0, 0);
    }
    if (g_calib.load() && (oc.overlayLineVtx || oc.overlayTriVtx)) {
        m_ctx->VSSetShader(m_vsOv, nullptr, 0);
        m_ctx->PSSetShader(m_psOv, nullptr, 0);
        UINT stride = sizeof(OvVtx), off = 0;
        ID3D11Buffer* vbs[] = { oc.overlayVB };
        m_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        m_ctx->IASetInputLayout(m_layoutOv);
        if (oc.overlayLineVtx) {
            m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_ctx->Draw(oc.overlayLineVtx, 0);
        }
        if (oc.overlayTriVtx) {
            m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_ctx->Draw(oc.overlayTriVtx, oc.overlayLineVtx);
        }
    }
    // SyncInterval=0: vsync Present(1) latches into DXGI's occluded skip-mode
    // when the window is briefly not composited (e.g. during startup or a
    // transient overlay) and never recovers -> permanent black output. The
    // render thread is already throttled by Sleep(6), so vsync is not needed.
    HRESULT phr = oc.swap->Present(0, 0);
    if (phr == (HRESULT)0x087A0001 /* DXGI_STATUS_OCCLUDED */) {
        // force a non-vsync blit so DWM re-attaches the swapchain, then pause
        // briefly so the occluded retry does not spin hot
        Sleep(20);
        phr = oc.swap->Present(0, 0);
    }
    if ((++m_frameCount % 180) == 1) {
        char b[200];
        sprintf_s(b, "draw: out=%ux%u idx=%u rtv=%p meshVB=%p hr=0x%08lX useImg=%d imgSRV=%p",
                  oc.w, oc.h, oc.indexCount, (void*)oc.rtv, (void*)oc.meshVB,
                  (unsigned long)phr, (int)(g_cfg.useImage ? 1 : 0), (void*)m_imgSRV);
        dbgLog(b);
    }
}

void Engine::renderAll() {
    // snapshot the output list under lock; entries stay alive via shared_ptr
    // even if the UI thread destroys outputs while this frame is drawing
    std::vector<std::shared_ptr<OutputCtx>> outs;
    {
        std::lock_guard<std::mutex> lk(m_outsMutex);
        outs = m_outs;
    }
    for (auto& ocS : outs) {
        OutputCtx* oc = ocS.get();
        if (!IsWindow(oc->hwnd)) continue;   // window already destroyed this frame
        OutputCfg* cfg = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            auto displays = enumerateDisplays();
            if (oc->displayIdx >= (int)displays.size()) continue;
            for (auto& o : g_cfg.outputs)
                if (o.enabled && o.deviceName == displays[oc->displayIdx].deviceName) { cfg = &o; break; }
        }
        if (!cfg) continue;

        // ---- geometry self-heal: snap window to its monitor and fit the
        //      swapchain to the client area, so output always fills the screen.
        //      Skipped in windowed preview mode, where the window is intentionally
        //      smaller than the monitor and freely movable.
        if (!g_previewMode.load()) {
            RECT wr{};
            GetWindowRect(oc->hwnd, &wr);
            HMONITOR hmon = MonitorFromWindow(oc->hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(hmon, &mi);
            if (wr.left != mi.rcMonitor.left || wr.top != mi.rcMonitor.top ||
                wr.right != mi.rcMonitor.right || wr.bottom != mi.rcMonitor.bottom) {
                dbgLog("heal: window " + rectStr(wr) + " -> monitor " + rectStr(mi.rcMonitor));
                SetWindowPos(oc->hwnd, HWND_TOPMOST,
                    mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                notifyOutputRaised();
            }
        }
        RECT crc{};
        GetClientRect(oc->hwnd, &crc);
        UINT cw = crc.right - crc.left, ch = crc.bottom - crc.top;
        if (cw && ch && (cw != oc->w || ch != oc->h))
            resizeSwapChain(*oc, cw, ch);

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
