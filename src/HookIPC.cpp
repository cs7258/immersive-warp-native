// Immersive Warp Native - host side of the hook IPC (scanout pipeline Phase 2+)
//
// Publishes IWNHookCfg through the named file mapping the injected WarpHook.dll
// reads. The mapping is created at startup with enabled=0; CMDNET `SET hook`
// / `SET hooktest` flip the fields live, and Engine::buildMesh publishes every
// enabled output's full control-point grid into its own per-output slot
// (v3 multi-mesh: one slot per projector, matched by monitor DeviceName).

#include "Common.h"
#include "hook/hook_ipc.h"
#include <mutex>
#include <cstring>
#include <sddl.h>

static HANDLE g_hookMap = nullptr;
static IWNHookCfg* g_hookView = nullptr;
static std::mutex g_hookMutex;

// The mapping must be openable by dwm.exe (SYSTEM). A default DACL proved to
// deny access there (gle=5), so hand out an explicit Everyone-full DACL -
// pagefile-backed sections are invisible in Explorer and the config contains
// no secrets, so a world-readable/writable warp grid is acceptable.
static HANDLE createWorldMapping() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr) && sd) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
    }
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.nLength ? &sa : nullptr,
                                  PAGE_READWRITE, 0, sizeof(IWNHookCfg),
                                  IWN_HOOK_MAPPING_NAME);
    if (sd) LocalFree(sd);
    return h;
}

bool hookIpcInit() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (g_hookView) return true;
    g_hookMap = createWorldMapping();
    if (!g_hookMap) return false;
    g_hookView = (IWNHookCfg*)MapViewOfFile(g_hookMap, FILE_MAP_WRITE, 0, 0, sizeof(IWNHookCfg));
    if (!g_hookView) return false;
    ZeroMemory(g_hookView, sizeof(IWNHookCfg));
    g_hookView->magic = IWN_HOOK_MAGIC;
    g_hookView->size = sizeof(IWNHookCfg);
    g_hookView->enabled = 0;
    g_hookView->gridN = 16;
    g_hookView->frameSkip = 0;
    g_hookView->gmode = 0;     // corner model until meshes are published
    g_hookView->outCount = 0;
    return true;
}

void hookIpcSet(bool enabled, const float corner[4][2], int gridN) {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView) return;
    g_hookView->enabled = enabled ? 1 : 0;
    if (corner) memcpy(g_hookView->corner, corner, sizeof(g_hookView->corner));
    if (gridN >= 2 && gridN <= 64) g_hookView->gridN = gridN;
    // SET hooktest switches to the global corner trapezoid model; a later
    // mesh publish (rev bump) switches back to per-output slots automatically
    if (enabled && corner) g_hookView->gmode = 0;
}

void hookIpcPublishMesh(int slot, int total, const float* src, int srcStride,
                        int rows, int cols, const float blend[16], const float chan[3],
                        int edgesMask, const wchar_t* monitor) {
    if (!src || rows < 2 || cols < 2) return;
    if (slot < 0 || slot >= IWN_MAX_OUTPUTS) return;
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView) return;
    IWNOutSlot& s = g_hookView->outs[slot];
    if ((LONG)rows * cols > IWN_MAX_MESH_VERTS) {
        // grid too large for the slot - deactivate it, keep the rest
        s.active = 0;
        return;
    }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const float* v = src + ((size_t)r * cols + c) * srcStride;
            IWNMeshVtx& d = s.verts[(size_t)r * cols + c];
            d.sx = v[0]; d.sy = v[1]; d.dx = v[2]; d.dy = v[3];
        }
    s.meshRows = rows;
    s.meshCols = cols;
    if (blend) memcpy(s.blend, blend, sizeof(s.blend));
    else ZeroMemory(s.blend, sizeof(s.blend));
    if (chan) memcpy(s.chanBlend, chan, sizeof(s.chanBlend));
    else { s.chanBlend[0] = s.chanBlend[1] = s.chanBlend[2] = 1.0f; }
    s.edgesMask = edgesMask;
    if (monitor) wcsncpy_s(s.monitor, monitor, _TRUNCATE);
    else s.monitor[0] = L'\0';
    s.active = 1;
    int t = total < 0 ? 0 : (total > IWN_MAX_OUTPUTS ? IWN_MAX_OUTPUTS : total);
    g_hookView->outCount = t;
    g_hookView->gmode = 1;     // slots take over from the corner model
    g_hookView->meshRev++;     // DLL rebuild trigger (global - simple + correct)
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

int hookIpcOutCount() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView || g_hookView->gmode != 1) return 0;
    return (int)g_hookView->outCount;
}

int hookIpcMeshVerts() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_hookView || g_hookView->gmode != 1) return 0;
    int n = 0;
    for (int i = 0; i < IWN_MAX_OUTPUTS && i < (int)g_hookView->outCount; i++)
        if (g_hookView->outs[i].active)
            n += (int)(g_hookView->outs[i].meshRows * g_hookView->outs[i].meshCols);
    return n;
}

int hookIpcMeshRev() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView ? (int)g_hookView->meshRev : 0;
}

bool hookIpcOverlay() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    return g_hookView && g_hookView->overlay;
}
