// Immersive Warp Native - host side of the hook IPC (scanout pipeline Phase 1)
//
// Publishes IWNHookCfg through the named file mapping the injected WarpHook.dll
// reads. The mapping is created at startup with enabled=0; CMDNET `SET hook`
// / `SET hooktest` flip the fields live - the DLL picks them up at Present time.

#include "Common.h"
#include "hook/hook_ipc.h"
#include <mutex>
#include <cstring>

static HANDLE g_hookMap = nullptr;
static IWNHookCfg* g_hookView = nullptr;
static std::mutex g_hookMutex;

bool hookIpcInit() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (g_hookView) return true;
    g_hookMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(IWNHookCfg), IWN_HOOK_MAPPING_NAME);
    if (!g_hookMap) return false;
    g_hookView = (IWNHookCfg*)MapViewOfFile(g_hookMap, FILE_MAP_WRITE, 0, 0, sizeof(IWNHookCfg));
    if (!g_hookView) return false;
    ZeroMemory(g_hookView, sizeof(IWNHookCfg));
    g_hookView->magic = IWN_HOOK_MAGIC;
    g_hookView->size = sizeof(IWNHookCfg);
    g_hookView->enabled = 0;
    g_hookView->gridN = 16;
    g_hookView->frameSkip = 0;
    return true;
}

void hookIpcSet(bool enabled, const float corner[4][2], int gridN) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView) return;
    g_hookView->enabled = enabled ? 1 : 0;
    if (corner) memcpy(g_hookView->corner, corner, sizeof(g_hookView->corner));
    if (gridN >= 2 && gridN <= 64) g_hookView->gridN = gridN;
}

bool hookIpcEnabled() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView && g_hookView->enabled;
}

unsigned hookIpcPassCount() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? g_hookView->reserved[0] : 0;
}

unsigned hookIpcExcCode() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? g_hookView->reserved[1] : 0;
}

unsigned hookIpcExcTid() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? g_hookView->reserved[2] : 0;
}
