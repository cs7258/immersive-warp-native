// Immersive Warp Native - scanout pipeline Phase 1: host <-> hook DLL IPC
//
// The control console (ImmersiveWarpNative.exe) publishes warp parameters
// for the injected hook DLL through a named file mapping. Layout is fixed
// by contract - keep both sides (host HookIPC.cpp, hook WarpHook.cpp) in
// sync with this header. No CRT dependencies so the DLL stays lean.

#pragma once
#include <windows.h>

#define IWN_HOOK_MAPPING_NAME L"IWN_WarpHook_v1"
#define IWN_HOOK_MAGIC        0x31485749u   // 'IWH1'

#pragma pack(push, 8)
struct IWNHookCfg {
    DWORD  magic;            // IWN_HOOK_MAGIC
    DWORD  size;             // sizeof(IWNHookCfg), sanity check
    LONG   enabled;          // 0 = passthrough, 1 = warp every Present
    LONG   gridN;            // mesh resolution per axis (>=2)
    float  corner[4][2];     // NDC offsets: TL, TR, BL, BR (x, y)
    LONG   frameSkip;        // log pacing
    DWORD  reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(IWNHookCfg) == 64, "hook IPC layout drift");
