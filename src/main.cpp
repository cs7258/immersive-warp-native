#include "Common.h"
#include "Engine.h"
#include <thread>

// vectored exception handler: silent deaths during test runs (frequent output
// rebuilds) left no trace at all - no WER event, no log line. This writes the
// exception code + offset-in-module into debug.log so a failing build can be
// diagnosed from the test machine alone.
static LONG WINAPI iwnExcHandler(EXCEPTION_POINTERS* ep) {
    // 0x406D1388 is the MSVC thread-name exception (std::thread debug helper):
    // harmless, never a failure
    if (ep->ExceptionRecord->ExceptionCode == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&iwnExcHandler, &m);
    uintptr_t off = m ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)m : 0;
    dbgLogLine("FATAL exc=0x" +
               [](DWORD c) {
                   char b[16];
                   sprintf_s(b, "%08lX", (unsigned long)c);
                   return std::string(b);
               }(ep->ExceptionRecord->ExceptionCode) +
               " @module+0x" +
               [](uintptr_t v) {
                   char b[24];
                   sprintf_s(b, "%llX", (unsigned long long)v);
                   return std::string(b);
               }(off) +
               " tid=" + std::to_string((long)GetCurrentThreadId()));
    return EXCEPTION_CONTINUE_SEARCH;
}

static void renderLoop() {
    dbgLogLine("renderLoop tid=" + std::to_string((long)GetCurrentThreadId()));
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
    AddVectoredExceptionHandler(1, iwnExcHandler);
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
    if (g_cfg.osc) oscStart(g_cfg.oscPort);

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
            else if (msg.wParam == 7) toggleInplaceOverlay(); // Ctrl+F9: overlay safety switch
            else if (msg.wParam == 8) toggleWindowWarping();  // F4: start/stop outputs
            else if (msg.wParam == 9) toggleInplaceOverlay(); // Ctrl+F4: desktop warping (same switch)
            else if (msg.wParam >= 10 && msg.wParam <= 17)
                loadPresetByIndex((int)msg.wParam - 10);      // Ctrl+Shift+1..8: presets
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running.store(false);
    worker.join();
    saveConfig(L"");
    cmdnetStop();
    oscStop();
    Engine::inst().shutdown();
    return 0;
}
