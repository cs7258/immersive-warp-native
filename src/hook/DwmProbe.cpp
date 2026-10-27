// Immersive Warp Native - dwm.exe 桌面扭曲探针（Stage 0: 只观测，不改像素）
//
// 背景（2026-10-27 取证结论）：原版 ImmersiveDisplayPRO 的桌面扭曲由
// WSupport64.sys（微软交叉签名的注入器）把载荷注入 dwm.exe 实现，用户态载荷
// 自身不导入任何图形 API —— 真正的扭曲发生在 dwm 的合成管线内。窗口级叠加
// 方案在同屏场景必然形成捕获反馈环，已被证伪。
//
// 本文件只做"可信打点"，不做像素修改，风险最低：
//   1) IAT 挂钩 dwmcore.dll!d3d11.dll!D3D11CreateDevice
//      -> dwm 的合成设备是什么、何时创建（注入时机是否够早）
//   2) 该设备的立即上下文 vtable slot 33 (OMSetRenderTargets)
//      -> 合成帧最终渲染到哪个纹理（尺寸/格式）
//   3) IAT 挂钩 win32u.dll!NtDCompositionConfirmFrame / RetireFrame
//      -> 每帧提交节拍、以及提交时刻是否已绑定 RT
//
// 全部用 IAT 槽替换：不碰指令字节，不需要指令长度解码，退出即恢复。
// 计数写入 IPC reserved[]，日志走宿主 hook.log（dwm 下回退到 Windows\Temp）。
//
// 若第 3 步在 dwm 里恒为 0，说明注入时机晚于 dwm 合成初始化，需要改成
// 驱动级 LoadImageNotify 早期注入（原版 WSupport64.sys 的做法）。

#include <windows.h>
#include <d3d11.h>
#include "hook_ipc.h"

extern void iwnHookLog(const char* line);
extern void iwnHookCounters(LONG c0, LONG c1, LONG c2);

// ---------------------------------------------------------------- 计数器
static volatile LONG g_cntCreate = 0;    // D3D11CreateDevice 调用次数
static volatile LONG g_cntConfirm = 0;   // ConfirmFrame
static volatile LONG g_cntRetire = 0;    // RetireFrame
static volatile LONG g_cntRtset = 0;     // OMSetRenderTargets
static volatile LONG g_cntRtv = 0;       // 非空 RT 视图次数
static volatile LONG g_rtW = 0, g_rtH = 0, g_rtFmt = 0;
static volatile LONG g_devFeature = 0;
static volatile LONG g_installed = 0;

// ---------------------------------------------------------------- IAT 替换
// 在模块的导入表里找 (dll, func) 的 IAT 槽地址；找不到返回 nullptr。
static void** findIatSlot(HMODULE mod, const char* dll, const char* fn) {
    if (!mod) return nullptr;
    auto dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* name = (const char*)((BYTE*)mod + imp->Name);
        bool hit = false;
        for (const char* a = name, *b = dll; *a && *b; a++, b++) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            if (!a[1]) hit = true;
        }
        if (!hit) continue;
        auto oft = (IMAGE_THUNK_DATA*)((BYTE*)mod + (imp->OriginalFirstThunk ?
                  imp->OriginalFirstThunk : imp->FirstThunk));
        auto ft = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->FirstThunk);
        for (; oft->u1.Function; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + oft->u1.AddressOfData);
            const char* cand = (const char*)ibn->Name;
            const char* p = cand; const char* q = fn;
            while (*p && *q && *p == *q) { p++; q++; }
            if (*p == 0 && *q == 0) return (void**)&ft->u1.Function;
        }
    }
    return nullptr;
}

static bool iatSwap(void** slot, void* newFn, void** oldFn) {
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    if (oldFn) *oldFn = *slot;
    *slot = newFn;
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void*), old, &dummy);
    return true;
}

// ---------------------------------------------------------------- 前向声明
static HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
    HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT nLevels, UINT sdk,
    ID3D11Device** dev, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** ctx);
static HRESULT WINAPI HookOMSetRenderTargets(ID3D11DeviceContext* self, UINT num,
    ID3D11RenderTargetView* const* rtvs);
static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4);
static LONG WINAPI HookRetireFrame(void* a1, void* a2, void* a3, void* a4);

typedef HRESULT(WINAPI* PFN_CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_CreateDevice g_origCreate = nullptr;
static HRESULT(WINAPI* g_origOMSetRT)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*) = nullptr;
static PVOID g_origConfirm = nullptr;
static PVOID g_origRetire = nullptr;

// 合成帧的 RT 描述（只保留数值，不持有纹理引用，避免拖住 dwm 的资源）
static void noteRenderTarget(ID3D11RenderTargetView* rtv) {
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    if (FAILED(rtv->GetResource(&res)) || !res) return;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        LONG w = (LONG)d.Width, h = (LONG)d.Height, f = (LONG)d.Format;
        LONG pw = g_rtW, ph = g_rtH, pf = g_rtFmt;
        g_rtW = w; g_rtH = h; g_rtFmt = f;
        if (w != pw || h != ph || f != pf) {
            char b[200];
            wsprintfA(b, "dwm: RT now %ldx%ld fmt=%ld mip=%u arr=%u samp=%u",
                      w, h, f, (unsigned)d.MipLevels, (unsigned)d.ArraySize, (unsigned)d.SampleDesc.Count);
            iwnHookLog(b);
        }
        tex->Release();
    }
    res->Release();
}

// d3d11 的立即上下文 vtable 是进程内共享的 —— 直接替换 slot 33，
// 不复制 vtable，也不改对象指针，dwm 的其它 context 一并被观测。
#define VT_OMSETRT 33
static void patchContextVtable(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    void** vt = *(void***)ctx;
    if (!vt) return;
    void* cur = vt[VT_OMSETRT];
    if (cur == (void*)&HookOMSetRenderTargets) return;   // 已打过
    DWORD old = 0;
    if (!VirtualProtect(&vt[VT_OMSETRT], sizeof(void*), PAGE_READWRITE, &old)) {
        iwnHookLog("dwm: vtable protect failed");
        return;
    }
    g_origOMSetRT = (decltype(g_origOMSetRT))cur;
    vt[VT_OMSETRT] = (void*)&HookOMSetRenderTargets;
    DWORD dummy = 0;
    VirtualProtect(&vt[VT_OMSETRT], sizeof(void*), old, &dummy);
    iwnHookLog("dwm: OMSetRenderTargets hooked on immediate context");
}

// ---------------------------------------------------------------- detours
static HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
    HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT nLevels, UINT sdk,
    ID3D11Device** dev, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** ctx) {
    InterlockedIncrement((volatile LONG*)&g_cntCreate);
    ID3D11DeviceContext* localCtx = nullptr;
    ID3D11DeviceContext** pCtx = ctx ? ctx : &localCtx;
    HRESULT hr = g_origCreate ? g_origCreate(adapter, type, sw, flags, levels, nLevels, sdk,
                                             dev, outLevel, pCtx) : E_FAIL;
    char b[220];
    wsprintfA(b, "dwm: D3D11CreateDevice hr=0x%08X drv=%d flags=0x%X ctx=%08X dev=%08X",
              (unsigned)hr, (int)type, flags, (unsigned)(ULONG_PTR)*pCtx,
              (unsigned)(ULONG_PTR)(dev ? *dev : nullptr));
    iwnHookLog(b);
    if (SUCCEEDED(hr)) {
        if (outLevel) g_devFeature = (LONG)*outLevel;
        if (*pCtx) patchContextVtable(*pCtx);
        if (dev && *dev && !*pCtx) {
            ID3D11DeviceContext* c = nullptr;
            (*dev)->GetImmediateContext(&c);
            if (c) { patchContextVtable(c); c->Release(); }
        }
    }
    if (localCtx) localCtx->Release();
    return hr;
}

static HRESULT WINAPI HookOMSetRenderTargets(ID3D11DeviceContext* self, UINT num,
                                            ID3D11RenderTargetView* const* rtvs) {
    InterlockedIncrement((volatile LONG*)&g_cntRtset);
    if (num && rtvs && rtvs[0]) {
        InterlockedIncrement((volatile LONG*)&g_cntRtv);
        noteRenderTarget(rtvs[0]);
    }
    return g_origOMSetRT ? g_origOMSetRT(self, num, rtvs) : S_OK;
}

static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement((volatile LONG*)&g_cntConfirm);
    typedef LONG(WINAPI* PF)(void*, void*, void*, void*);
    return g_origConfirm ? ((PF)g_origConfirm)(a1, a2, a3, a4) : 0;
}

static LONG WINAPI HookRetireFrame(void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement((volatile LONG*)&g_cntRetire);
    typedef LONG(WINAPI* PF)(void*, void*, void*, void*);
    return g_origRetire ? ((PF)g_origRetire)(a1, a2, a3, a4) : 0;
}

// ---------------------------------------------------------------- 安装
static HMODULE waitModule(const wchar_t* name, int ms) {
    for (int i = 0; i < ms / 100; i++) {
        HMODULE m = GetModuleHandleW(name);
        if (m) return m;
        Sleep(100);
    }
    return nullptr;
}

static DWORD WINAPI reporter(LPVOID) {
    char b[260];
    LONG lastC = -1;
    for (;;) {
        Sleep(1000);
        LONG c = g_cntConfirm;
        iwnHookCounters(g_cntConfirm, g_cntRtset, g_cntCreate);
        if (c != lastC) {
            wsprintfA(b, "dwm: confirm=%ld retire=%ld rtset=%ld rtv=%ld rt=%ldx%ld fmt=%ld fl=%ld",
                      g_cntConfirm, g_cntRetire, g_cntRtset, g_cntRtv,
                      g_rtW, g_rtH, g_rtFmt, g_devFeature);
            iwnHookLog(b);
            lastC = c;
        }
    }
    return 0;
}

extern "C" void DwmProbeInstall() {
    if (InterlockedCompareExchange((volatile LONG*)&g_installed, 1, 0) != 0) return;
    iwnHookLog("dwm: probe install begin");

    HMODULE core = waitModule(L"dwmcore.dll", 30000);
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!core) {
        iwnHookLog("dwm: dwmcore.dll not loaded after 30s -> injection too early?");
    }
    char b[220];

    // 1) D3D11CreateDevice（dwm.exe / dwmcore.dll 任一处）
    void* old = nullptr;
    if (iatSwap(findIatSlot(core, "d3d11.dll", "D3D11CreateDevice"),
                (void*)&HookD3D11CreateDevice, &old)) {
        g_origCreate = (PFN_CreateDevice)old;
        iwnHookLog("dwm: IAT hooked d3d11!D3D11CreateDevice (dwmcore)");
    } else if (iatSwap(findIatSlot(exe, "d3d11.dll", "D3D11CreateDevice"),
                       (void*)&HookD3D11CreateDevice, &old)) {
        g_origCreate = (PFN_CreateDevice)old;
        iwnHookLog("dwm: IAT hooked d3d11!D3D11CreateDevice (dwm.exe)");
    } else {
        iwnHookLog("dwm: D3D11CreateDevice IAT slot not found");
    }

    // 2) 内核合成提交（win32u 的 NT 调用）
    void* o2 = nullptr;
    if (iatSwap(findIatSlot(core, "win32u.dll", "NtDCompositionConfirmFrame"),
                (void*)&HookConfirmFrame, &o2)) {
        g_origConfirm = o2;
        iwnHookLog("dwm: IAT hooked win32u!NtDCompositionConfirmFrame");
    } else {
        iwnHookLog("dwm: ConfirmFrame IAT slot not found");
    }
    void* o3 = nullptr;
    if (iatSwap(findIatSlot(core, "win32u.dll", "NtDCompositionRetireFrame"),
                (void*)&HookRetireFrame, &o3)) {
        g_origRetire = o3;
        iwnHookLog("dwm: IAT hooked win32u!NtDCompositionRetireFrame");
    } else {
        iwnHookLog("dwm: RetireFrame IAT slot not found");
    }

    // 3) 若设备早就建好了（晚注入），直接从现有设备上抓 context 打点
    if (!g_origOMSetRT) {
        // 没有设备引用，只能等下一次 D3D11CreateDevice；把现状记下来
        wsprintfA(b, "dwm: no device seen yet (late injection), create=%ld", g_cntCreate);
        iwnHookLog(b);
    }
    iwnHookLog("dwm: probe installed");
    CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
}
