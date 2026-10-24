// Immersive Warp Native - host side of the hook IPC (scanout pipeline Phase 2)
//
// Publishes IWNHookCfg through the named file mapping the injected WarpHook.dll
// reads. The mapping is created at startup with enabled=0; CMDNET `SET hook`
// / `SET hooktest` flip the fields live, and Engine::buildMesh publishes the
// first enabled output's full control-point grid through hookIpcPublishMesh.

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
    g_hookView->mode = 0;      // corner model until a mesh is published
    return true;
}

void hookIpcSet(bool enabled, const float corner[4][2], int gridN) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView) return;
    g_hookView->enabled = enabled ? 1 : 0;
    if (corner) memcpy(g_hookView->corner, corner, sizeof(g_hookView->corner));
    if (gridN >= 2 && gridN <= 64) g_hookView->gridN = gridN;
    // SET hooktest switches to the corner trapezoid model; a later mesh
    // publish (rev bump) switches back automatically
    if (enabled && corner) g_hookView->mode = 0;
}

void hookIpcPublishMesh(const float* src, int srcStride, int rows, int cols,
                        const float blend[16], const float chan[3],
                        int edgesMask, const wchar_t* monitor) {
    if (!src || rows < 2 || cols < 2) return;
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView) return;
    if ((LONG)rows * cols > IWN_MAX_MESH_VERTS) {
        // grid too large for the mapping - keep the corner model
        g_hookView->mode = 0;
        g_hookView->vertCount = 0;
        return;
    }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const float* v = src + ((size_t)r * cols + c) * srcStride;
            IWNMeshVtx& d = g_hookView->verts[(size_t)r * cols + c];
            d.sx = v[0]; d.sy = v[1]; d.dx = v[2]; d.dy = v[3];
        }
    g_hookView->vertCount = rows * cols;
    g_hookView->meshRows = rows;
    g_hookView->meshCols = cols;
    if (blend) memcpy(g_hookView->blend, blend, sizeof(g_hookView->blend));
    else ZeroMemory(g_hookView->blend, sizeof(g_hookView->blend));
    if (chan) memcpy(g_hookView->chanBlend, chan, sizeof(g_hookView->chanBlend));
    else { g_hookView->chanBlend[0] = g_hookView->chanBlend[1] = g_hookView->chanBlend[2] = 1.0f; }
    g_hookView->edgesMask = edgesMask;
    if (monitor) {
        wcsncpy_s(g_hookView->monitor, monitor, _TRUNCATE);
    } else {
        g_hookView->monitor[0] = L'\0';
    }
    g_hookView->mode = 1;
    g_hookView->meshRev++;     // DLL rebuild trigger
}

void hookIpcSetOverlay(bool on) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (g_hookView) g_hookView->overlay = on ? 1 : 0;
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

int hookIpcMeshVerts() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? (int)g_hookView->vertCount : 0;
}

int hookIpcMeshRev() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? (int)g_hookView->meshRev : 0;
}

bool hookIpcOverlay() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView && g_hookView->overlay;
}
