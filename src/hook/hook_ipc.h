// Immersive Warp Native - host <-> hook DLL IPC (scanout pipeline Phase 2)
//
// The control console (ImmersiveWarpNative.exe) publishes warp parameters
// for the injected WarpHook.dll through a named file mapping. Layout is fixed
// by contract - keep both sides (host HookIPC.cpp, hook WarpHook.cpp) in
// sync with this header. No CRT dependencies so the DLL stays lean.
//
// Phase 2 adds:
//  - full control-point mesh export (row-major grid, same Catmull-Rom /
//    bilinear math as the host's own Engine::buildMesh), replacing the
//    Phase-1 four-corner PoC as the primary warp model;
//  - per-edge blend-band constants (fade/gamma/gain/slope) so the DLL's
//    pixel shader reproduces the host's parametric edge curve;
//  - target monitor binding (per-swapchain output selection).

#pragma once
#include <windows.h>

#define IWN_HOOK_MAPPING_NAME L"IWN_WarpHook_v2"
#define IWN_HOOK_MAGIC        0x32485749u   // 'IWH2'

// mesh capacity: rows/cols up to 12 with subdivide 8 fits (73*89 = 6497);
// the host falls back to corner mode if the grid is larger
#define IWN_MAX_MESH_VERTS    8192

#pragma pack(push, 8)
struct IWNMeshVtx { float sx, sy, dx, dy; };   // src uv + dest position (0..1 space)

struct IWNHookCfg {
    DWORD  magic;            // IWN_HOOK_MAGIC
    DWORD  size;             // sizeof(IWNHookCfg), sanity check
    LONG   enabled;          // 0 = passthrough, 1 = warp every Present
    LONG   gridN;            // corner-model mesh resolution per axis (>=2)
    float  corner[4][2];     // corner model NDC offsets: TL, TR, BL, BR
    LONG   frameSkip;        // log pacing
    DWORD  reserved[3];      // [0]=Present detour hit count, [1]=first-chance
                             // exception code seen by the hook VEH, [2]=tid of
                             // the thread that raised it (crash telemetry)
    // ---- v2 ----
    LONG   vertCount;        // grid vertex count (0 = corner model)
    LONG   mode;             // 0 = corner model, 1 = full mesh below
    LONG   meshRows;         // grid vertex rows (RV)
    LONG   meshCols;         // grid vertex cols (CV)
    float  blend[16];        // per side (0=L 1=R 2=T 3=B), 4 floats each:
                             //   fade width (uv fraction), gamma, gain, slope
    float  chanBlend[3];     // per-channel blend gamma R,G,B (edge 0's values)
    LONG   edgesMask;        // bit0=left bit1=right bit2=top bit3=bottom
    LONG   meshRev;          // bumped on every mesh publish (DLL rebuild trigger)
    LONG   overlay;          // 0 = none, 1 = draw the warp mesh lines on top
    LONG   overlayPad;       // keep 8-byte alignment before the vertex array
    wchar_t monitor[32];     // target monitor DeviceName (\\.\DISPLAYn);
                             // empty = apply to every hooked swapchain
    IWNMeshVtx verts[IWN_MAX_MESH_VERTS];
};
#pragma pack(pop)

static_assert(offsetof(IWNHookCfg, verts) == 236, "hook IPC v2 header drift");
static_assert(sizeof(IWNHookCfg) >= 236 + IWN_MAX_MESH_VERTS * sizeof(IWNMeshVtx),
              "hook IPC v2 layout drift");
