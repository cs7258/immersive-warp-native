// Immersive Warp Native - host <-> hook DLL IPC (scanout pipeline Phase 2+)
//
// The control console (ImmersiveWarpNative.exe) publishes warp parameters
// for the injected WarpHook.dll through a named file mapping. Layout is fixed
// by contract - keep both sides (host HookIPC.cpp, hook WarpHook.cpp) in
// sync with this header. No CRT dependencies so the DLL stays lean.
//
// v2 added: full control-point mesh export, per-edge blend-band constants,
// target monitor binding (per-swapchain output selection).
//
// v3 adds: PER-OUTPUT mesh slots. Every enabled host output publishes its own
// grid + blend constants + monitor DeviceName into one of IWN_MAX_OUTPUTS
// slots; the DLL matches the swapchain's containing output against the slot
// monitors, so a multi-projector rig warps each screen with its own mesh.
// The legacy corner model (hooktest PoC) stays as a global override.

#pragma once
#include <windows.h>

#define IWN_HOOK_MAPPING_NAME L"IWN_WarpHook_v3"
#define IWN_HOOK_MAGIC        0x33485749u   // 'IWH3'

// mesh capacity per output: rows/cols up to 12 with subdivide 8 fits
// (73*89 = 6497); the host skips publishing larger grids for that slot
#define IWN_MAX_MESH_VERTS    8192
#define IWN_MAX_OUTPUTS       4

#pragma pack(push, 8)
struct IWNMeshVtx { float sx, sy, dx, dy; };   // src uv + dest position (0..1 space)

// one enabled output's warp parameters
struct IWNOutSlot {
    LONG   active;         // 1 = this slot carries a published mesh
    LONG   meshRows;       // grid vertex rows (RV)
    LONG   meshCols;       // grid vertex cols (CV); verts = rows*cols
    float  blend[16];      // per side (0=L 1=R 2=T 3=B), 4 floats each:
                           //   fade width (uv fraction), gamma, gain, slope
    float  chanBlend[3];   // per-channel blend gamma R,G,B (edge 0's values)
    LONG   edgesMask;      // bit0=left bit1=right bit2=top bit3=bottom
    wchar_t monitor[32];   // target monitor DeviceName (\\.\DISPLAYn);
                           // empty = catch-all for unmatched swapchains
    IWNMeshVtx verts[IWN_MAX_MESH_VERTS];
};

struct IWNHookCfg {
    DWORD  magic;            // IWN_HOOK_MAGIC
    DWORD  size;             // sizeof(IWNHookCfg), sanity check
    LONG   enabled;          // 0 = passthrough, 1 = warp on every Present
    LONG   gridN;            // corner-model mesh resolution per axis (>=2)
    float  corner[4][2];     // corner model NDC offsets: TL, TR, BL, BR
    LONG   frameSkip;        // log pacing
    DWORD  reserved[3];      // [0]=Present detour hit count, [1]=first-chance
                             // exception code seen by the hook VEH, [2]=tid of
                             // the thread that raised it (crash telemetry)
    LONG   gmode;            // 0 = global corner model, 1 = per-output slots
    LONG   outCount;         // valid slot count (0..IWN_MAX_OUTPUTS)
    LONG   meshRev;          // bumped on every publish (DLL rebuild trigger)
    LONG   overlay;          // 0 = none, 1 = draw the warp mesh lines on top
    LONG   pad[3];
    IWNOutSlot outs[IWN_MAX_OUTPUTS];
};
#pragma pack(pop)

static_assert(offsetof(IWNOutSlot, verts) == 156, "hook IPC v3 slot drift");
static_assert(sizeof(IWNOutSlot) == 156 + IWN_MAX_MESH_VERTS * sizeof(IWNMeshVtx),
              "hook IPC v3 slot layout drift");
static_assert(offsetof(IWNHookCfg, outs) == 92, "hook IPC v3 header drift");
static_assert(sizeof(IWNHookCfg) == 92 + IWN_MAX_OUTPUTS * sizeof(IWNOutSlot),
              "hook IPC v3 total layout drift");
