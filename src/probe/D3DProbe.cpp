// Immersive Warp Native - IWND3DProbe.exe (external D3D11 injection target)
//
// Minimal D3D11 app: a 640x480 window with a flip-model swapchain and an
// animated clear-color Present loop. Exists so injecttest can prove the
// scanout pipeline hooks EXTERNAL swapchains end-to-end:
//
//   INJECT LAUNCH IWND3DProbe.exe --secs 20
//     -> hook.log  "swapchain hooked ..."
//     -> GET hook  pass= counter grows (the probe's Presents land in the
//        shared IWNHookCfg mapping owned by the running console)
//
// Options:  --secs N   auto-exit after N seconds (regression friendly;
//                      without it, run until Esc / window close)

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <cstdio>
#include <cwchar>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_TIMER:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int wmain(int argc, wchar_t** argv) {
    int secs = 0;
    for (int i = 1; i < argc; i++)
        // case-insensitive: CMDNET dispatch uppercases the whole command
        // line, so "--secs" arrives as "--SECS"
        if (!lstrcmpiW(argv[i], L"--secs") && i + 1 < argc)
            secs = (int)wcstoul(argv[++i], nullptr, 10);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"IWND3DProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) {
        printf("probe: RegisterClass failed gle=%u\n", GetLastError());
        return 1;
    }
    RECT r = {0, 0, 640, 480};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowW(L"IWND3DProbe", L"IWN D3D Probe", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) {
        printf("probe: CreateWindow failed gle=%u\n", GetLastError());
        return 1;
    }
    ShowWindow(wnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sw = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd,
                                               &sw, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("probe: D3D11CreateDeviceAndSwapChain failed hr=0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        dev->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
    }
    if (!rtv) {
        printf("probe: render target view failed\n");
        return 1;
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, 640, 480, 0, 1};
    ctx->RSSetViewports(1, &vp);
    printf("probe: running (swapchain=%p)%s\n", (void*)sw, secs ? " auto-exit" : "");
    if (secs) SetTimer(wnd, 1, secs * 1000, nullptr);

    bool running = true;
    MSG msg;

    // ---- calibration pattern (visually verifiable warp target) ----
    // White background, 40 px grid, 160 px major grid, red center cross,
    // checker quadrant, and a bottom RGB bar that cycles each frame (proves
    // Present is live and gives a visual latency cue). Uploaded via
    // UpdateSubresource + CopyResource - no shaders needed.
    const int PW = 640, PH = 480;
    unsigned int* img = (unsigned int*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 (size_t)PW * PH * 4);
    if (!img) { printf("probe: alloc failed\n"); return 1; }
#define PX(x, y, rgb) img[(y) * PW + (x)] = (rgb)
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            unsigned int c = 0x00FFFFFF;               // white base
            if (x % 40 == 0 || y % 40 == 0) c = 0x00C8C8C8;   // minor grid
            if (x % 160 < 2 || y % 160 < 2) c = 0x00787878;   // major grid
            if ((x / 80 + y / 80) % 2 && x < 320 && y < 320)  // checker corner
                c = (c == 0x00FFFFFF) ? 0x00E8E8E8 : c;
            int cx = x - PW / 2, cy = y - PH / 2;
            if (cy > -2 && cy < 2 && cx > -40 && cx < 40) c = 0x000000C8;  // cross H
            if (cx > -2 && cx < 2 && cy > -40 && cy < 40) c = 0x00C80000;  // cross V (red)
            PX(x, y, c);
        }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = PW; td.Height = PH;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* pat = nullptr;
    D3D11_SUBRESOURCE_DATA init = {img, PW * 4, 0};
    if (FAILED(dev->CreateTexture2D(&td, &init, &pat))) {
        printf("probe: pattern texture failed\n"); return 1;
    }
    // CPU-side row buffer for the animated bottom bar; UpdateSubresource
    // pushes it straight into the default-usage pattern texture (a DYNAMIC
    // texture would reject initial data and is not needed here).
    unsigned int row[8][PW] = {};
#undef PX

    // PeekMessage, not GetMessage: a blocking pump renders nothing while no
    // messages arrive, so the Present counter would stall (and WM_TIMER
    // auto-exit would still fire - the render loop is the bug, not the timer).
    unsigned long frame = 0;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        frame++;
        // bottom bar: hue cycles with the frame count (visual Present liveness)
        for (int x = 0; x < PW; x++) {
            double h = 6.0 * ((x / 80.0) + frame / 90.0);
            int s = (int)h % 6;
            double f = h - (int)h, v = 1.0;
            double r = 0, g = 0, b = 0;
            switch (s) {
            case 0: r = v; g = f; break;
            case 1: r = 1 - f; g = v; break;
            case 2: g = v; b = f; break;
            case 3: g = 1 - f; b = v; break;
            case 4: r = f; b = v; break;
            default: r = v; b = 1 - f; break;
            }
            unsigned int rgb = 0x00FFFFFF & ((unsigned int)(r * 255) << 16 |
                                             (unsigned int)(g * 255) << 8 |
                                             (unsigned int)(b * 255));
            for (int y = 0; y < 8; y++) row[y][x] = rgb;
        }
        D3D11_BOX dst = {0, PH - 8, 0, PW, PH, 1};   // bottom bar rows only
        ctx->UpdateSubresource(pat, 0, &dst, row, PW * 4, 0);
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            ctx->CopyResource(back, pat);
            back->Release();
        }
        sw->Present(1, 0);
    }

    if (pat) pat->Release();
    HeapFree(GetProcessHeap(), 0, img);
    if (rtv) rtv->Release();
    if (sw) sw->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    printf("probe: exit\n");
    return 0;
}
