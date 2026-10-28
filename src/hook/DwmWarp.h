// Immersive Warp Native - dwm composite-surface warp engine (Stage 1)
//
// Stage 0 (DwmProbe.cpp) proved we can *find* dwm's composed desktop surface
// and read real desktop pixels out of it. This header is the write-back half:
// it takes that surface and paints a warped + blended copy of it back onto
// itself, inside the dwm submit boundary.
//
// Why inside the submit boundary: the screen shows whatever dwm hands to the
// kernel compositor, and the capture path (PrintScreen / Snipping Tool) reads
// the same composed output. Painting before the submit is the only place where
// "screen shows warp" and "capture still gets the frame dwm produced" can be
// separated - which is exactly the original product's forcePreserveBB trick.
//
// Ownership: Attach() AddRef()s the surface. On failure nothing is retained.
#pragma once
#include <windows.h>
#include <d3d11.h>

// Hand the located desktop surface to the engine. Builds twin texture, RTV,
// SRV, sampler, pre-compiled-via-D3DCompile shaders and the warp grid.
// Returns false on any failure - caller keeps its own reference either way.
bool DwmWarpAttach(ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& d);

// Multi-surface variant. dwm keeps several same-size RT|SRV surfaces (buffer
// rotation / redirection), and the 2026-10-28 20:22 run proved painting ONE of
// them changes nothing on screen - so every candidate gets painted. Re-attach
// while running refreshes the RTV set (twin survives if size/format match).
struct IWNWarpTarget {
    ID3D11Texture2D*        tex;
    D3D11_TEXTURE2D_DESC    desc;
};
bool DwmWarpAttachTargets(const IWNWarpTarget* list, int n);

// Called immediately BEFORE the original NtDCompositionConfirmFrame.
// No-op (cheap) when the engine is off / not ready / already failed out.
void DwmWarpBeforeSubmit();

// Called immediately AFTER the original NtDCompositionConfirmFrame returns.
void DwmWarpAfterSubmit();

// Install a 12-byte prologue detour on dwmcore!CDWMSwapChain::PresentInternal
// (RVA 0x6C6B0). The detour is inert until the engine reaches its present
// phase; the callback then paints the warp after dwm's render is final and
// before the real present runs. Idempotent.
void DwmWarpInstallPresentHook();

// 0 = off, 1..4 = staged self-diagnosis (see .cpp). Negative = permanently off.
int  DwmWarpStage();

// One-line status for the reporter thread log (stage, frames, failures).
void DwmWarpStatus(char* out, int cap);
