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

// Called immediately BEFORE the original NtDCompositionConfirmFrame.
// No-op (cheap) when the engine is off / not ready / already failed out.
void DwmWarpBeforeSubmit();

// Called immediately AFTER the original NtDCompositionConfirmFrame returns.
void DwmWarpAfterSubmit();

// 0 = off, 1..4 = staged self-diagnosis (see .cpp). Negative = permanently off.
int  DwmWarpStage();

// One-line status for the reporter thread log (stage, frames, failures).
void DwmWarpStatus(char* out, int cap);
