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
        if (!lstrcmpW(argv[i], L"--secs") && i + 1 < argc)
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

    float t = 0;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        t += 0.01f;
        float c[4] = {0.5f + 0.5f * sinf(t),
                      0.5f + 0.5f * sinf(t + 2.1f),
                      0.5f + 0.5f * sinf(t + 4.2f), 1.0f};
        ctx->ClearRenderTargetView(rtv, c);
        sw->Present(1, 0);
    }

    if (rtv) rtv->Release();
    if (sw) sw->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    printf("probe: exit\n");
    return 0;
}
