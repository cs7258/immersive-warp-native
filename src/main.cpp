#include "Common.h"
#include "Engine.h"
#include <thread>

static void renderLoop() {
    while (g_running.load()) {
        Engine::inst().renderAll();
        Sleep(6);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    (void)nCmdShow;
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

    if (!Engine::inst().init()) {
        MessageBoxW(nullptr, L"D3D11 device creation failed.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    registerAndCreateControlWindow(hInst);

    if (g_cfg.enabled) createOutputWindows();

    std::thread worker(renderLoop);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running.store(false);
    worker.join();
    saveConfig(L"");
    Engine::inst().shutdown();
    return 0;
}
