#include "Common.h"
#include "Engine.h"
#include <thread>

static void renderLoop() {
    while (g_running.load()) {
        Engine::inst().renderAll();
        Sleep(6);
    }
}

static void enableDpiAwareness() {
    // physical-pixel coordinates everywhere: prevents DWM virtualization from
    // offsetting/scaling the fullscreen output window on scaled displays
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
    auto fn = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    (void)nCmdShow;
    enableDpiAwareness();
    // initial config: one entry per display, all disabled
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        auto displays = enumerateDisplays();
        for (auto& d : displays) {
            OutputCfg o;
            o.deviceName = d.deviceName;
            o.label = d.label;
            o.initIdentity();
            g_cfg.outputs.push_back(o);
        }
    }
    loadConfig(L""); // override from saved file if present
    if (g_cfg.cmdnet) cmdnetStart(g_cfg.cmdnetPort);

    if (!Engine::inst().init()) {
        MessageBoxW(nullptr, L"D3D11 device creation failed.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    // restore saved image source / preview mode before any window is created
    if (g_cfg.testImg >= 0) Engine::inst().setTestImage(g_cfg.testImg);
    else if (!g_cfg.imagePath.empty()) Engine::inst().loadImage(g_cfg.imagePath);
    g_previewMode.store(g_cfg.previewMode);

    registerAndCreateControlWindow(hInst);

    if (g_cfg.enabled) createOutputWindows();

    std::thread worker(renderLoop);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == 1) toggleCalibration();       // F3
            else if (msg.wParam == 2) switchSubModeAll();   // F10
            else if (msg.wParam == 3) toggleCalibration();  // Esc (registered while calibrating)
            else if (msg.wParam == 4) cycleBlendEdges();    // F12
            else if (msg.wParam == 5) toggleSnapshot();     // Ctrl+F3
            else if (msg.wParam == 6) openColorDialogEx();  // D (registered while calibrating)
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running.store(false);
    worker.join();
    saveConfig(L"");
    cmdnetStop();
    Engine::inst().shutdown();
    return 0;
}
