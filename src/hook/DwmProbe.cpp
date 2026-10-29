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
#include <dxgi.h>
#include <tlhelp32.h>
#include "hook_ipc.h"
#include "DwmWarp.h"

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
// dwm 的立即上下文（early injection 时由 D3D11CreateDevice / 扫描取得）。
// dwm 合成很可能是"初始化时绑一次 RT，之后每帧只 Draw"，所以 OMSetRenderTargets
// 一次都不会被调用（实测 rtset=0）—— 拿 RT 的正确方式是提交边界反查。
static ID3D11DeviceContext* g_ctx = nullptr;
static volatile LONG g_rtProbes = 0;

// ---------------------------------------------------------------- CFG 注册
// dwm.exe 全程开启 Control Flow Guard。IAT 槽里的间接 call 会被 CFG 校验，
// 而 manual-mapped 的 DLL 不在进程 CFG 位图里 —— 不注册的话第一次调用
// detour 就是 fail-fast（dwm 直接重启）。SetProcessValidCallTargets 允许
// 进程内把自己代码页标为合法调用目标。
struct CfgTargetInfo { ULONG_PTR Offset; ULONG_PTR Flags; };
typedef BOOL(WINAPI* PFN_SPVCT)(HANDLE, PVOID, SIZE_T, ULONG, CfgTargetInfo*);

static HMODULE g_selfBase = nullptr;   // 由 HookEntry 传入（manual map 不在加载器链表里）

static void registerCfg(void* fn, const char* tag) {
    // SetProcessValidCallTargets only accepts pages backed by an image section;
    // our manual-mapped .text is MEM_PRIVATE, so it always fails with
    // ERROR_INVALID_PARAMETER (87) - that is what the old log showed. It is also
    // unnecessary: CFG's bitmap covers image pages, and executable pages that
    // belong to no image are treated as VALID targets by default (which is why
    // the IAT detours were called every frame for 28s without dwm dying).
    // So: classify the page instead of calling an API that cannot succeed.
    ULONG_PTR page = (ULONG_PTR)fn & ~(ULONG_PTR)0xFFF;
    MEMORY_BASIC_INFORMATION mbi = {};
    char b[200];
    if (!VirtualQuery((void*)page, &mbi, sizeof(mbi))) {
        wsprintfA(b, "dwm: CFG %s VirtualQuery failed gle=%u", tag, (unsigned)GetLastError());
        iwnHookLog(b);
        return;
    }
    if (mbi.Type != MEM_IMAGE) {
        wsprintfA(b, "dwm: CFG %s -> non-image page (MEM_PRIVATE), valid target by default", tag);
        iwnHookLog(b);
        return;
    }
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto fnSet = kb ? (PFN_SPVCT)GetProcAddress(kb, "SetProcessValidCallTargets") : nullptr;
    if (!fnSet) {
        wsprintfA(b, "dwm: CFG %s image page but API missing", tag);
        iwnHookLog(b);
        return;
    }
    CfgTargetInfo ti = {};
    ti.Offset = (ULONG_PTR)fn - page;
    ti.Flags = 1;                       // CFG_CALL_TARGET_VALID
    SetLastError(0);
    BOOL ok = fnSet(GetCurrentProcess(), (PVOID)page, 0x1000, 1, &ti);
    wsprintfA(b, "dwm: CFG %s image page register ok=%d gle=%u", tag, ok, (unsigned)GetLastError());
    iwnHookLog(b);
}

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

// 判断地址落在哪个已加载模块里 —— 用于识别 IAT 槽是否已被"别人"（原版载荷）替换
static void ownerModuleOf(void* addr, char* out, int cap) {
    if (cap < 4) return;
    out[0] = 0;
    if (!addr) { lstrcpynA(out, "null", cap); return; }
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m) {
        lstrcpynA(out, "unmapped", cap);
        return;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
    int i = 0;
    for (; base[i] && i < cap - 1; ++i) {
        wchar_t c = base[i];
        out[i] = (c < 128) ? (char)c : '?';
    }
    out[i] = 0;
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
static HRESULT WINAPI HookOMSetRTUAV(ID3D11DeviceContext* self, UINT num,
    ID3D11RenderTargetView* const* rtvs, ID3D11UnorderedAccessView* uav,
    UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs,
    const UINT* counts);
static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4);
static LONG WINAPI HookRetireFrame(void* a1, void* a2, void* a3, void* a4);
static void proveTextureIsDesktop(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d);

typedef HRESULT(WINAPI* PFN_CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_CreateDevice g_origCreate = nullptr;
static HRESULT(WINAPI* g_origOMSetRT)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*) = nullptr;
typedef HRESULT(WINAPI* PFN_OMSetRTUAV)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11UnorderedAccessView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*, const UINT*);
static PFN_OMSetRTUAV g_origOMSetRTUAV = nullptr;
static PVOID g_origConfirm = nullptr;
static PVOID g_origRetire = nullptr;

// 只做一次的深度采集开关（零初始化，不触发 thread-safe static guard）
static volatile LONG g_deepDone = 0;

// 纹理扫描选中的"疑似合成表面"（由 scanTextures 登记，ConfirmFrame 里读回验证）
static ID3D11Texture2D* g_targetTex = nullptr;   // = g_targets[0], proof subject
static D3D11_TEXTURE2D_DESC g_targetDesc;
// dwm keeps several same-size RT|SRV surfaces (buffer rotation / redirection).
// The 2026-10-28 20:22 run proved painting exactly ONE of them (the
// TEXPROOF-carrying one) changes nothing on screen, so the scan now collects
// every exact-size candidate and the engine paints all of them.
#define IWN_PROBE_MAX_TGT 5
static ID3D11Texture2D*      g_targets[IWN_PROBE_MAX_TGT];
static D3D11_TEXTURE2D_DESC  g_tdescs[IWN_PROBE_MAX_TGT];
// capture list: EVERY screen-size texture, any bind/misc/usage. The 09:53
// run dumped the only RT|SRV candidate and it held wallpaper + desktop icons
// but NO windows and NO taskbar - the final composed buffer must live in a
// differently-flagged texture. Dump them all, let PrintScreen decide.
#define IWN_PROBE_MAX_CAP 8
static ID3D11Texture2D*      g_caps[IWN_PROBE_MAX_CAP];
static D3D11_TEXTURE2D_DESC  g_capsD[IWN_PROBE_MAX_CAP];
static int                   g_capN = 0;
static int                   g_targetN = 0;
static volatile LONG g_proofDone = 0;

// 合成帧的 RT 描述（只保留数值，不持有纹理引用，避免拖住 dwm 的资源）
static void noteRenderTarget(ID3D11RenderTargetView* rtv) {
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);          // ID3D11View::GetResource returns void
    if (!res) return;
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
        // ---- 一次性深度采集：Stage 1 能不能在这个 RT 上做 copy + 网格变形 ----
        // 只在"桌面尺寸"的表面上采一次：dwm 内部还有大量小临时表面，
        // 采到它们会把唯一一次机会浪费掉。
        bool desktopSized = (w >= 640 && h >= 360);
        if (desktopSized && InterlockedCompareExchange(&g_deepDone, 1, 0) == 0) {
            char b[300];
            wsprintfA(b, "dwm: RTDESC %lux%lu fmt=%lu mip=%u arr=%u samp=%u/%u usage=%u bind=0x%X cpu=0x%X misc=0x%X",
                      (unsigned long)d.Width, (unsigned long)d.Height, (unsigned long)d.Format,
                      (unsigned)d.MipLevels, (unsigned)d.ArraySize,
                      (unsigned)d.SampleDesc.Count, (unsigned)d.SampleDesc.Quality,
                      (unsigned)d.Usage, (unsigned)d.BindFlags,
                      (unsigned)d.CPUAccessFlags, (unsigned)d.MiscFlags);
            iwnHookLog(b);

            ID3D11Device* dev = nullptr;
            tex->GetDevice(&dev);
            if (dev) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = d.Format;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                ID3D11ShaderResourceView* srv = nullptr;
                HRESULT shr = dev->CreateShaderResourceView(tex, &sd, &srv);
                wsprintfA(b, "dwm: SRV on composed RT hr=0x%08X -> %s",
                          (unsigned)shr, SUCCEEDED(shr) ? "warp input OK" : "cannot sample (needs staging copy)");
                iwnHookLog(b);
                if (srv) srv->Release();

                D3D11_TEXTURE2D_DESC cd = d;
                cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                cd.MiscFlags = 0;
                cd.CPUAccessFlags = 0;
                cd.Usage = D3D11_USAGE_DEFAULT;
                ID3D11Texture2D* twin = nullptr;
                HRESULT chr = dev->CreateTexture2D(&cd, nullptr, &twin);
                wsprintfA(b, "dwm: twin RT %lux%lu create hr=0x%08X",
                          (unsigned long)cd.Width, (unsigned long)cd.Height, (unsigned)chr);
                iwnHookLog(b);
                if (twin) twin->Release();

                dev->Release();
            }

            IDXGIResource* dxres = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(IDXGIResource), (void**)&dxres)) && dxres) {
                HANDLE sh = nullptr;
                HRESULT ghr = dxres->GetSharedHandle(&sh);
                wsprintfA(b, "dwm: RT IDXGIResource hr=0x%08X shared=%08X",
                          (unsigned)ghr, (unsigned)(ULONG_PTR)sh);
                iwnHookLog(b);
                dxres->Release();
            }
        }
        tex->Release();
    }
    res->Release();
}

// d3d11 的立即上下文 vtable 是进程内共享的 —— 直接替换 slot 33，
// 不复制 vtable，也不改对象指针，dwm 的其它 context 一并被观测。
#define VT_OMSETRT 33
#define VT_OMSETRT_UAV 34
static void patchContextVtable(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (g_ctx != ctx) {                // 交给我们"提交边界反查 RT"使用，自己持引用
        if (g_ctx) g_ctx->Release();
        g_ctx = ctx;
        ctx->AddRef();
    }
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

    // slot 34：dwm 若用 OMSetRenderTargetsAndUnorderedAccessViews 绑定，也能捕获
    void* cur2 = vt[VT_OMSETRT_UAV];
    if (cur2 && cur2 != (void*)&HookOMSetRTUAV &&
        VirtualProtect(&vt[VT_OMSETRT_UAV], sizeof(void*), PAGE_READWRITE, &old)) {
        g_origOMSetRTUAV = (PFN_OMSetRTUAV)cur2;
        vt[VT_OMSETRT_UAV] = (void*)&HookOMSetRTUAV;
        VirtualProtect(&vt[VT_OMSETRT_UAV], sizeof(void*), old, &dummy);
        iwnHookLog("dwm: OMSetRenderTargetsAndUnorderedAccessViews hooked");
    }
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

static HRESULT WINAPI HookOMSetRTUAV(ID3D11DeviceContext* self, UINT num,
    ID3D11RenderTargetView* const* rtvs, ID3D11UnorderedAccessView* uav,
    UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs,
    const UINT* counts) {
    InterlockedIncrement((volatile LONG*)&g_cntRtset);
    if (num && rtvs && rtvs[0]) {
        InterlockedIncrement((volatile LONG*)&g_cntRtv);
        noteRenderTarget(rtvs[0]);
    }
    if (!g_ctx) g_ctx = self;
    return g_origOMSetRTUAV
        ? g_origOMSetRTUAV(self, num, rtvs, uav, uavStart, uavCount, uavs, counts)
        : S_OK;
}

// 提交边界反查：dwm 很可能只在初始化时绑定一次 RT，之后每帧只改 viewport/scissor
// 再 Draw，所以 OMSetRenderTargets 钩子永远不响（实测 rtset=0）。这里在 ConfirmFrame
// 里直接用 dwm 自己的立即上下文问"你现在绑的是哪个 RT"——不依赖它怎么绑的。
static void probeActiveRT() {
    if (!g_ctx) return;
    LONG n = InterlockedIncrement(&g_rtProbes);
    if (n > 10 && (n % 180) != 0) return;      // 前 10 帧连探，之后每 180 帧一次
    ID3D11RenderTargetView* rtv = nullptr;
    __try {
        g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rtv = nullptr;
    }
    if (!rtv) {
        if (n <= 10) iwnHookLog("dwm: OMGetRenderTargets -> nothing bound");
        return;
    }
    noteRenderTarget(rtv);
    rtv->Release();
}

// ---- capture-all dump (2026-10-29 user-directed experiment) ------------------
// Copy every screen-size texture we enumerated out to C:\Windows\Temp\iwncap<N>.raw
// (raw BGRA, pitch removed). The host compares each against PrintScreen: the
// dump that CONTAINS WINDOWS is the final pre-present composition buffer.
static volatile LONG s_capDumped = 0;

static void writeRawDump(const wchar_t* path, const void* data, SIZE_T bytes) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, data, (DWORD)bytes, &wrote, nullptr);
    CloseHandle(h);
}

static void dumpAllCaptures() {
    if (!g_ctx || !g_dev || g_capN <= 0) return;
    char b[256];
    for (int i = 0; i < g_capN; i++) {
        if (!g_caps[i]) continue;
        D3D11_TEXTURE2D_DESC td;
        __try { g_caps[i]->GetDesc(&td); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        // only 32bpp formats get a meaningful flat dump
        unsigned bpp = 0;
        if (td.Format == 87 || td.Format == 28 || td.Format == 24 || td.Format == 2) bpp = 4;
        else if (td.Format == 11 || td.Format == 10) bpp = 8;
        if (bpp != 4) {
            wsprintfA(b, "dwm: CAPTURE[%d] fmt=%u bpp=%u - SKIPPED (not 32bpp)",
                      i, (unsigned)td.Format, bpp);
            iwnHookLog(b);
            continue;
        }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.BindFlags        = 0;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags        = 0;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        ID3D11Texture2D* stg = nullptr;
        HRESULT hr = g_dev->CreateTexture2D(&sd, nullptr, &stg);
        if (FAILED(hr) || !stg) {
            wsprintfA(b, "dwm: CAPTURE[%d] staging hr=0x%08X", i, (unsigned)hr);
            iwnHookLog(b);
            continue;
        }
        g_ctx->CopyResource(stg, g_caps[i]);
        D3D11_MAPPED_SUBRESOURCE mp;
        hr = g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp);
        if (FAILED(hr)) {
            wsprintfA(b, "dwm: CAPTURE[%d] map hr=0x%08X", i, (unsigned)hr);
            iwnHookLog(b);
            stg->Release();
            continue;
        }
        wchar_t path[64];
        {
            const wchar_t* p1 = L"C:\\Windows\\Temp\\iwncap";
            const wchar_t* p2 = L".raw";
            wchar_t* o = path;
            for (const wchar_t* q = p1; *q; ) *o++ = *q++;
            if (i >= 10) *o++ = (wchar_t)(L'0' + i / 10);
            *o++ = (wchar_t)(L'0' + i % 10);
            for (const wchar_t* q = p2; *q; ) *o++ = *q++;
            *o = 0;
        }
        SIZE_T rowBytes = (SIZE_T)td.Width * 4;
        BYTE* flat = (BYTE*)HeapAlloc(GetProcessHeap(), 0, rowBytes * td.Height);
        if (flat) {
            for (UINT y = 0; y < td.Height; y++)
                CopyMemory(flat + y * rowBytes,
                           (BYTE*)mp.pData + (SIZE_T)y * mp.RowPitch, rowBytes);
            writeRawDump(path, flat, rowBytes * td.Height);
            HeapFree(GetProcessHeap(), 0, flat);
        }
        g_ctx->Unmap(stg, 0);
        stg->Release();
        wsprintfA(b, "dwm: CAPTURE[%d] %ux%u fmt=%u bind=0x%X misc=0x%X usage=%u pitch=%lu -> %S",
                  i, (unsigned)td.Width, (unsigned)td.Height, (unsigned)td.Format,
                  (unsigned)td.BindFlags, (unsigned)td.MiscFlags, (unsigned)td.Usage,
                  (unsigned long)mp.RowPitch, path);
        iwnHookLog(b);
    }
    wsprintfA(b, "dwm: CAPTURE dumped=%d", g_capN);
    iwnHookLog(b);
}

static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement((volatile LONG*)&g_cntConfirm);
    // capture-all: fire once well after texscan has run and steady state begun
    if (g_capN > 0 && g_cntConfirm > 1500 &&
        InterlockedCompareExchange(&s_capDumped, 1, 0) == 0) {
        __try { dumpAllCaptures(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { iwnHookLog("dwm: CAPTURE EXCEPTION"); }
    }
    probeActiveRT();
    // 纹理扫描已经登记了候选合成表面时，在提交边界做一次性像素读回。
    // 放在这里而不是扫描线程：immediate context 不是线程安全的，只有在
    // dwm 自己把这一帧推出去的这一刻用它才安全。
    //
    // 像素验证通过后，把这张表面交给扭曲引擎（Stage 1）。引擎自己 AddRef，
    // 我们随后释放自己这一份引用 —— 谁持有谁负责，不重复保留。
    if (g_targetN > 0 && InterlockedCompareExchange(&g_proofDone, 1, 0) == 0) {
        proveTextureIsDesktop(g_targets[0], g_tdescs[0]);
        IWNWarpTarget list[IWN_PROBE_MAX_TGT];
        for (int i = 0; i < g_targetN; i++) {
            list[i].tex = g_targets[i];
            list[i].desc = g_tdescs[i];
        }
        bool ok = DwmWarpAttachTargets(list, g_targetN);
        iwnHookLog(ok ? "dwm: warp attach OK" : "dwm: warp attach FAILED");
        // NOTE: the PresentInternal detour is NOT installed here anymore. Both
        // 00:40 and 00:59 runs show dwm dying ~1s after install (WER event:
        // 0xC0000005 with faulting IP in UNKNOWN/private memory = a live thread
        // fetched the 12-byte patch half-written and jumped to a torn garbage
        // address). It is now installed lazily at phase C entry, so phases A/B
        // (pure ConfirmFrame painting) get a clean 16s window first.
        for (int i = 0; i < g_targetN; i++) {
            __try { g_targets[i]->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            g_targets[i] = nullptr;
        }
        g_targetN = 0;
        g_targetTex = nullptr;
    }

    // 双侧结构：提交前存干净副本 + 画扭曲，提交后恢复。
    // 与原版 forcePreserveBB 同构，也是"屏幕扭曲、截图矩形"的唯一分离点。
    DwmWarpBeforeSubmit();
    typedef LONG(WINAPI* PF)(void*, void*, void*, void*);
    LONG r = g_origConfirm ? ((PF)g_origConfirm)(a1, a2, a3, a4) : 0;
    DwmWarpAfterSubmit();
    return r;
}

static LONG WINAPI HookRetireFrame(void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement((volatile LONG*)&g_cntRetire);
    typedef LONG(WINAPI* PF)(void*, void*, void*, void*);
    return g_origRetire ? ((PF)g_origRetire)(a1, a2, a3, a4) : 0;
}

// ------------------------------------------------- 晚注入：从内存里找回 dwm 的 D3D11 设备
// 本 DLL 通常是在 dwm 已经建好 D3D11 设备之后才注入的，D3D11CreateDevice 不会再来
// 第二次 —— 于是拿不到设备、也就拿不到合成 RT（实测 rtset=0 / rt=0x0 / fl=0）。
// 但 dwm 进程里必然存在活的 ID3D11Device / ID3D11DeviceContext 对象，用下面的
// vtable 指纹法把它们找出来（不调用任何未验证的指针）。
static HMODULE g_coreMod = nullptr;
static ID3D11Device* g_dev = nullptr;
static volatile LONG g_scanRounds = 0;

// ------------------------------------------------ 设备定位（不做任何盲虚调用）
//
// 关键教训（2026-10-27，两次黑屏换来）：
// 上一版扫描把"对象首 qword 落在 d3d11 映像内"当成"可能是 COM 对象"，然后对它调
// QueryInterface。堆里大量普通数据（缓存下来的 vtable 指针、函数指针、跳转表）也满足
// 那个条件，对它们做虚调用 = 跳到 d3d11 内部某个随机函数、this 指向垃圾 -> AV。
// 实测特征：注入后**恰好 1.0 秒**（reporter 线程第一轮扫描）dwm 死，
// 0xC0000005 落在 d3d11.dll+0x7DCB0，两次运行同一地址，且日志里从来没有扫描汇总行
// （= 死在扫描中途）。dwm 一死，原版载荷随之消失，桌面扭曲失效。
//
// 现在的做法：先自己建一个临时设备，把它的 vtable 指针当**指纹**，再按指纹在私有
// 内存里找"值完全等于指纹"的 qword。命中即真实 ID3D11Device 对象（同一个 C++ 类），
// 全程不需要任何虚调用就能确认身份，之后的操作才是安全的。
static void* g_vtDevice = nullptr;      // 临时设备的 vtable 指纹
static void* g_vtCtx = nullptr;         // 临时设备立即上下文的 vtable 指纹
static void* g_vtTex = nullptr;         // 临时纹理的 vtable 指纹（找 dwm 的全部纹理）
static LONG g_scrW = 0, g_scrH = 0;     // 主屏像素尺寸（合成表面必然等于它）

// 只对"已确认身份"的真设备调用（内部全被 __try 包住）
static bool ctxHasRT(ID3D11Device* dev) {
    ID3D11DeviceContext* c = nullptr;
    __try { dev->GetImmediateContext(&c); } __except (EXCEPTION_EXECUTE_HANDLER) { c = nullptr; }
    if (!c) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    __try { c->OMGetRenderTargets(1, &rtv, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { rtv = nullptr; }
    c->Release();
    if (!rtv) return false;
    rtv->Release();
    return true;
}

// 采指纹：建一个属于我们自己的临时设备，读完 vtable 立刻释放。
//
// 驱动类型优先 WARP（2026-10-27 第二轮实测调整）：我们只要 vtable 这个"类指纹"，
// 而 ID3D11Device / ID3D11DeviceContext / ID3D11Texture2D 的实现类都定义在
// d3d11.dll 内部，与 D3D_DRIVER_TYPE 无关，WARP 与 HARDWARE 取到的 vtable 完全
// 相同。但 HARDWARE 会在 dwm 里创建第二个硬件设备、触发 UMD 初始化 —— 上一轮
// 日志里那条 EXC 0xC0000005 @d3d11+0x4EBB7（紧跟在 fingerprint 之后）就是它。
// 用 WARP 只走软件光栅器，不碰 UMD，对 dwm 的扰动最小。
static void buildVtableFingerprint() {
    if (g_vtDevice) return;
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d) { iwnHookLog("dwm: fingerprint no d3d11.dll"); return; }
    auto create = (PFN_CreateDevice)GetProcAddress(d3d, "D3D11CreateDevice");
    if (!create) { iwnHookLog("dwm: fingerprint no D3D11CreateDevice export"); return; }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    const D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE };
    for (int i = 0; i < 2 && !g_vtDevice; i++) {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
        HRESULT hr = E_FAIL;
        __try {
            hr = create(nullptr, types[i], nullptr, 0, levels,
                        (UINT)(sizeof(levels) / sizeof(levels[0])),
                        D3D11_SDK_VERSION, &dev, &got, &ctx);
        } __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; }
        char b[200];
        wsprintfA(b, "dwm: fingerprint type=%d hr=0x%08X dev=%08X",
                  (int)types[i], (unsigned)hr, (unsigned)(ULONG_PTR)dev);
        iwnHookLog(b);
        if (SUCCEEDED(hr) && dev) {
            g_vtDevice = *(void**)dev;
            if (ctx) g_vtCtx = *(void**)ctx;
            // 顺手建一张 4x4 纹理，取 ID3D11Texture2D 的 vtable 当纹理类指纹
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = 4;
            td.Height = 4;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D* t = nullptr;
            __try {
                if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t)) && t) g_vtTex = *(void**)t;
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_vtTex = nullptr; }
            if (t) t->Release();
            dev->Release();
            if (ctx) ctx->Release();
        }
    }
    char b[220];
    wsprintfA(b, "dwm: fingerprint vtDevice=%08X vtCtx=%08X vtTex=%08X",
              (unsigned)(ULONG_PTR)g_vtDevice, (unsigned)(ULONG_PTR)g_vtCtx,
              (unsigned)(ULONG_PTR)g_vtTex);
    iwnHookLog(b);
}

// 按指纹在私有可读内存里找设备：纯读取 + 精确比对，不做任何盲虚调用。
static bool recoverDeviceByScan() {
    if (g_dev) return true;
    buildVtableFingerprint();
    if (!g_vtDevice) return false;

    InterlockedIncrement(&g_scanRounds);
    int qwords = 0, hits = 0, withRT = 0;
    ID3D11Device* best = nullptr;

    BYTE* addr = nullptr;
    while ((ULONG_PTR)addr < 0x7FFFFFFF0000ULL && !best) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T sz = mbi.RegionSize;
        if (!sz) break;
        DWORD prot = mbi.Protect & 0xFF;
        bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && readable &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && sz <= (512u << 20)) {
            for (SIZE_T off = 0; off + sizeof(void*) <= sz; off += sizeof(void*)) {
                BYTE* p = base + off;
                if (*(void**)p != g_vtDevice) continue;   // 只有精确指纹命中才算候选
                hits++;
                ID3D11Device* dev = (ID3D11Device*)p;
                bool hasRT = false;
                __try { hasRT = ctxHasRT(dev); } __except (EXCEPTION_EXECUTE_HANDLER) { hasRT = false; }
                if (hasRT) { withRT++; best = dev; break; }
                if (!best) {
                    // 记一个"没有绑定 RT"的作为后备，避免完全空手而归
                    __try {
                        ID3D11DeviceContext* c = nullptr;
                        dev->GetImmediateContext(&c);
                        if (c) { c->Release(); best = dev; }
                    } __except (EXCEPTION_EXECUTE_HANDLER) { }
                }
            }
            qwords += (int)(sz / sizeof(void*));
        }
        if (best) break;
        addr = base + sz;
    }

    char b[260];
    wsprintfA(b, "dwm: scan round %ld: qwords=%d vtHits=%d withRT=%d -> %s",
              g_scanRounds, qwords, hits, withRT, best ? "FOUND device" : "no");
    iwnHookLog(b);

    if (!best) return false;
    g_dev = best;
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    __try { fl = g_dev->GetFeatureLevel(); } __except (EXCEPTION_EXECUTE_HANDLER) { fl = (D3D_FEATURE_LEVEL)0; }
    g_devFeature = (LONG)fl;
    ID3D11DeviceContext* ic = nullptr;
    __try { g_dev->GetImmediateContext(&ic); } __except (EXCEPTION_EXECUTE_HANDLER) { ic = nullptr; }
    bool hooked = false;
    if (ic) { patchContextVtable(ic); ic->Release(); hooked = true; }
    wsprintfA(b, "dwm: device recovered by scan feature=0x%X ctx=%s rt=%s",
              (unsigned)fl, hooked ? "ok" : "none", withRT ? "bound" : "none");
    iwnHookLog(b);
    return true;
}

// ------------------------------------------------- 合成表面定位（纹理指纹法）
//
// 为什么不再问 context：这一轮实测已经把"上下文反查"这条路彻底否掉了 ——
//   * rtset=0      : dwm 从不在这台设备的立即上下文上调 OMSetRenderTargets
//   * rt=none      : ConfirmFrame 时刻 OMGetRenderTargets 恒为空（10/10 次）
// dwm 的渲染是"画完就解绑"，提交边界上根本没有 RT 可问。所以改成正向枚举：
// 按 ID3D11Texture2D 的 vtable 指纹把 dwm 进程里所有纹理对象抓出来，用
// GetDesc 读尺寸/格式/绑定标志 —— 桌面合成表面必然是 1920x1080 级且带
// D3D11_BIND_RENDER_TARGET，这一步不需要任何猜测。
static volatile LONG g_texScanDone = 0;

static bool isDesktopSize(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width >= 640 && d.Height >= 360;
}

// 对已确认是 ID3D11Texture2D 的对象做一次读回，证明它装的就是桌面像素。
// 时机放在 ConfirmFrame 里（dwm 正在提交 = 本帧渲染已结束、context 空闲），
// 只做一次；Map(READ) 会 stall 一帧，可以接受。
static void proveTextureIsDesktop(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) {
    char b[512];
    ID3D11Device* dev = nullptr;
    __try { tex->GetDevice(&dev); } __except (EXCEPTION_EXECUTE_HANDLER) { dev = nullptr; }
    if (!dev) { iwnHookLog("dwm: texproof no device"); return; }
    ID3D11DeviceContext* ctx = nullptr;
    // ID3D11Device::GetImmediateContext returns void - cannot wrap in SUCCEEDED()
    __try { dev->GetImmediateContext(&ctx); } __except (EXCEPTION_EXECUTE_HANDLER) { ctx = nullptr; }
    if (!ctx) { dev->Release(); iwnHookLog("dwm: texproof no context"); return; }

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    ID3D11Texture2D* stg = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = dev->CreateTexture2D(&sd, nullptr, &stg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; stg = nullptr; }
    if (FAILED(hr) || !stg) {
        wsprintfA(b, "dwm: texproof staging create hr=0x%08X", (unsigned)hr);
        iwnHookLog(b);
        ctx->Release(); dev->Release();
        return;
    }
    __try {
        if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(stg, 0, tex, 0, d.Format);
        else                        ctx->CopyResource(stg, tex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        iwnHookLog("dwm: texproof copy EXCEPTION");
    }

    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = E_FAIL;
    __try { hr = ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; }
    if (SUCCEEDED(hr)) {
        char px[192] = {};
        int off = 0;
        const UINT xs[3] = { d.Width / 2, d.Width / 4, d.Width * 3 / 4 };
        const UINT ys[3] = { d.Height / 2, d.Height / 4, d.Height * 3 / 4 };
        const char* tag[3] = { "mid", "q1", "q3" };
        for (int i = 0; i < 3; i++) {
            DWORD v = 0xDEADBEEF;
            __try {
                const BYTE* row = (const BYTE*)m.pData + (SIZE_T)ys[i] * m.RowPitch;
                v = *(const DWORD*)(row + (SIZE_T)xs[i] * 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xDEADBEEF; }
            off += wsprintfA(px + off, "%s=%08lX ", tag[i], (unsigned long)v);
        }
        wsprintfA(b, "dwm: TEXPROOF %lux%lu fmt=%lu bind=0x%X -> %s",
                  (unsigned long)d.Width, (unsigned long)d.Height,
                  (unsigned long)d.Format, (unsigned)d.BindFlags, px);
        iwnHookLog(b);
        __try { ctx->Unmap(stg, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    } else {
        wsprintfA(b, "dwm: texproof map hr=0x%08X", (unsigned)hr);
        iwnHookLog(b);
    }
    stg->Release();
    ctx->Release();
    dev->Release();
}

// 全地址空间按纹理指纹枚举。命中数可能成百上千，只报"桌面尺寸"的前若干个，
// 并在其中挑一个带 RENDER_TARGET 的做像素读回验证。
static void scanTextures() {
    if (!g_vtTex) return;                       // 指纹还没采到，留给下一轮
    if (InterlockedCompareExchange(&g_texScanDone, 1, 0) != 0) return;

    int hits = 0, big = 0, rtBig = 0, exactBig = 0;
    bool capped = false;

    BYTE* addr = nullptr;
    while ((ULONG_PTR)addr < 0x7FFFFFFF0000ULL && !capped) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        BYTE* base = (BYTE*)mbi.BaseAddress;
        SIZE_T sz = mbi.RegionSize;
        if (!sz) break;
        DWORD prot = mbi.Protect & 0xFF;
        bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && readable &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && sz <= (512u << 20)) {
            for (SIZE_T off = 0; off + sizeof(void*) <= sz; off += sizeof(void*)) {
                BYTE* p = base + off;
                if (*(void**)p != g_vtTex) continue;    // 精确指纹，不做盲虚调用
                hits++;
                if (hits > 8192) { capped = true; break; }
                ID3D11Texture2D* tex = (ID3D11Texture2D*)p;
                D3D11_TEXTURE2D_DESC d = {};
                bool ok = false;
                __try { tex->GetDesc(&d); ok = true; }
                __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                if (!ok) continue;
                // 自检：真实纹理的宽高必须落在合法区间，否则是误命中
                if (d.Width == 0 || d.Height == 0 || d.Width > 16384 || d.Height > 16384) continue;
                if (!isDesktopSize(d)) continue;
                big++;
                bool isRT = (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
                // 合成表面必然等于某个输出的像素尺寸。用主屏分辨率做精确判据：
                // 1366x768 这种尺寸在 dwm 里几乎不可能出现在临时表面上。
                bool exact = (g_scrW > 0 && (LONG)d.Width == g_scrW && (LONG)d.Height == g_scrH);
                if (exact) exactBig++;
                char b[300];
                wsprintfA(b, "dwm: TEX %lux%lu fmt=%lu mip=%u arr=%u samp=%u bind=0x%X misc=0x%X usage=%u%s",
                          (unsigned long)d.Width, (unsigned long)d.Height, (unsigned long)d.Format,
                          (unsigned)d.MipLevels, (unsigned)d.ArraySize, (unsigned)d.SampleDesc.Count,
                          (unsigned)d.BindFlags, (unsigned)d.MiscFlags, (unsigned)d.Usage,
                          exact ? "  <== SCREEN SIZE" : "");
                iwnHookLog(b);
                if (exact && g_capN < IWN_PROBE_MAX_CAP && d.SampleDesc.Count == 1) {
                    bool dupC = false;
                    for (int i = 0; i < g_capN; i++)
                        if (g_caps[i] == tex) { dupC = true; break; }
                    if (!dupC) {
                        __try { tex->AddRef(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                        g_capsD[g_capN] = d;
                        g_caps[g_capN]  = tex;
                        g_capN++;
                    }
                }
                if (isRT) {
                    rtBig++;
                    // 收集所有"屏幕尺寸 + RT|SRV + 单采样"的候选。dwm 的合成
                    // 缓冲是轮换的（多缓冲），只画其中一张 = 画在没上屏的那张。
                    bool collect = exact &&
                                   (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
                                   d.SampleDesc.Count == 1;
                    if (collect) {
                        bool dup = false;
                        for (int i = 0; i < g_targetN; i++) {
                            if (g_targets[i] == tex) { dup = true; break; }
                        }
                        if (!dup && g_targetN < IWN_PROBE_MAX_TGT) {
                            __try { tex->AddRef(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                            g_tdescs[g_targetN] = d;
                            g_targets[g_targetN] = tex;
                            g_targetN++;
                        }
                    }
                }
                if (big >= 32 || rtBig >= 14) { capped = true; break; }
            }
        }
        addr = base + sz;
    }

    char b[300];
    wsprintfA(b, "dwm: texscan hits=%d big=%d rtBig=%d exact=%d screen=%ldx%ld -> %s",
              hits, big, rtBig, exactBig, g_scrW, g_scrH,
              g_targetN > 0 ? "FOUND target surface(s)" : "no target surface");
    iwnHookLog(b);

    if (g_targetN > 0) {
        wsprintfA(b, "dwm: registered %d exact-size RT surfaces (engine paints ALL)",
                  g_targetN);
        iwnHookLog(b);
        // 只登记，不在这里读回：immediate context 不是线程安全的，reporter 线程
        // 直接用它和 dwm 的渲染线程并发会把两边都搞崩。像素验证交给
        // HookConfirmFrame（提交边界，dwm 自己把帧推出来的那一刻）。
        g_targetTex = g_targets[0];
        g_targetDesc = g_tdescs[0];
        iwnHookLog("dwm: target surface registered, pixel proof deferred to ConfirmFrame");
    }
}

// ---------------------------------------------------------------- 取证 Census（只读）
// 目的：把"原版桌面扭曲到底钩了什么"变成清单而不是猜测。
//   1) IAT 普查：遍历 dwm 每个模块的导入表，凡槽位指向"任何已加载映像之外"
//      就是 detour（manual-mapped 载荷 / VirtualAlloc 的 trampoline）。
//      自己（manual map 不在加载器链表）以 WarpHook(self) 名义加入模块表，避免误报。
//   2) vtable 普查：dwm 的 D3D11 设备/立即上下文/纹理类的 vtable 槽，
//      凡指向 d3d11.dll 映像之外的即 vtable 补丁。
// 跑两次（原版扭曲 off / on）取差集 = 原版桌面扭曲的确切 API 清单。
struct CensusMod { HMODULE base; DWORD size; char name[48]; };

static int censusModules(CensusMod* out, int cap) {
    int n = 0;
    if (g_selfBase) {
        auto dos = (IMAGE_DOS_HEADER*)g_selfBase;
        __try {
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto nt = (IMAGE_NT_HEADERS*)((BYTE*)g_selfBase + dos->e_lfanew);
                if (n < cap) {
                    out[n].base = g_selfBase;
                    out[n].size = nt->OptionalHeader.SizeOfImage;
                    lstrcpynA(out[n].name, "WarpHook(self)", 48);
                    n++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return n;
    MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (n >= cap) break;
            out[n].base = me.hModule;
            out[n].size = me.modBaseSize;
            int i = 0;
            for (; me.szModule[i] && i < 47; ++i) {
                wchar_t c = me.szModule[i];
                out[n].name[i] = (c < 128) ? (char)c : '?';
            }
            out[n].name[i] = 0;
            n++;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return n;
}

static int censusFindMod(const CensusMod* mods, int n, void* addr) {
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < n; i++) {
        if (a >= (uintptr_t)mods[i].base &&
            a <  (uintptr_t)mods[i].base + mods[i].size) return i;
    }
    return -1;
}

static void iatCensus() {
    CensusMod mods[96];
    int n = censusModules(mods, 96);
    char b[260];
    wsprintfA(b, "census: modules=%d (below: IAT slots pointing outside every image)", n);
    iwnHookLog(b);
    int reported = 0;
    for (int i = 0; i < n && reported < 48; i++) {
        BYTE* mod = (BYTE*)mods[i].base;
        auto dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto nt = (IMAGE_NT_HEADERS*)(mod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) continue;
        auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(mod + dir.VirtualAddress);
        for (; imp->Name && reported < 48; imp++) {
            const char* dllName = (const char*)(mod + imp->Name);
            auto oft = (IMAGE_THUNK_DATA*)(mod + (imp->OriginalFirstThunk ?
                      imp->OriginalFirstThunk : imp->FirstThunk));
            auto ft  = (IMAGE_THUNK_DATA*)(mod + imp->FirstThunk);
            for (; ft->u1.Function && reported < 48; oft++, ft++) {
                void* target = (void*)ft->u1.Function;
                if (censusFindMod(mods, n, target) >= 0) continue;
                char fn[64] = "?";
                if (!IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) {
                    auto ibn = (IMAGE_IMPORT_BY_NAME*)(mod + oft->u1.AddressOfData);
                    int k = 0;
                    for (; ibn->Name[k] && k < 60; k++) fn[k] = ibn->Name[k];
                    fn[k] = 0;
                } else {
                    wsprintfA(fn, "#%lu", (unsigned long)IMAGE_ORDINAL(oft->u1.Ordinal));
                }
                const char* kind = "unmapped";
                MEMORY_BASIC_INFORMATION mbi = {};
                if (VirtualQuery(target, &mbi, sizeof(mbi))) {
                    if (mbi.Type == MEM_IMAGE)  kind = "image?";
                    else if (mbi.Type == MEM_MAPPED) kind = "mapped";
                    else kind = "private";
                }
                wsprintfA(b, "census: IAT %s!%s!%s -> %s %08X",
                          mods[i].name, dllName, fn, kind,
                          (unsigned)(ULONG_PTR)target);
                iwnHookLog(b);
                reported++;
            }
        }
    }
    wsprintfA(b, "census: IAT detours=%d", reported);
    iwnHookLog(b);
}

static void vtableCensus(const char* what, void* const* vt, HMODULE host) {
    if (!vt || !host) return;
    CensusMod mods[96];
    int n = censusModules(mods, 96);
    auto dos = (IMAGE_DOS_HEADER*)host;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)host + dos->e_lfanew);
    uintptr_t hBase = (uintptr_t)host;
    uintptr_t hEnd = hBase + nt->OptionalHeader.SizeOfImage;
    int patched = 0;
    for (int i = 0; i < 48; i++) {
        void* fn = vt[i];                       // vtable 页在映像内，读取安全
        if (!fn) break;
        uintptr_t a = (uintptr_t)fn;
        if (a >= hBase && a < hEnd) continue;
        const char* kind = "unmapped";
        int o = censusFindMod(mods, n, fn);
        if (o >= 0) kind = mods[o].name;
        else {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(fn, &mbi, sizeof(mbi)))
                kind = (mbi.Type == MEM_IMAGE) ? "image?" : "private";
        }
        char b[192];
        wsprintfA(b, "census: VT %s[%d] -> %s %08X", what, i, kind, (unsigned)a);
        iwnHookLog(b);
        if (++patched >= 16) { iwnHookLog("census: VT ... (capped 16)"); break; }
    }
    if (!patched) {
        char b[128];
        wsprintfA(b, "census: VT %s clean (0/48 outside host)", what);
        iwnHookLog(b);
    }
}

static DWORD WINAPI censusThread(LPVOID) {
    // 等注入/扫描稳定（23:20 实测设备恢复发生在注入后约 1s）
    Sleep(5000);
    iatCensus();
    for (int i = 0; i < 20 && !g_dev; i++) Sleep(500);
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (g_dev) vtableCensus("device", *(void* const**)g_dev, d3d);
    if (g_ctx) vtableCensus("ctx",     *(void* const**)g_ctx, d3d);
    if (g_vtTex) vtableCensus("texclass", (void* const*)g_vtTex, d3d);
    iwnHookLog("census: done");
    return 0;
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
        // 晚注入路径：设备早就建好了，D3D11CreateDevice 不会再来 —— 主动去内存里找。
        // 全地址空间扫描很重（几十 MB），只在前 4 轮做，避免拖慢 dwm。
        if (!g_dev && g_scanRounds < 3) {
            recoverDeviceByScan();
        }
        // 设备到手后，立刻按纹理指纹把 dwm 的纹理清单抓出来（只做一次）
        if (g_dev && !g_texScanDone) {
            scanTextures();
        }
        LONG c = g_cntConfirm;
        iwnHookCounters(g_cntConfirm, g_cntRtset, g_cntCreate);
        if (c != lastC) {
            char ws[192];
            DwmWarpStatus(ws, (int)sizeof(ws));
            wsprintfA(b, "dwm: confirm=%ld retire=%ld rtset=%ld rtv=%ld rt=%ldx%ld fmt=%ld fl=%ld",
                      g_cntConfirm, g_cntRetire, g_cntRtset, g_cntRtv,
                      g_rtW, g_rtH, g_rtFmt, g_devFeature);
            iwnHookLog(b);
            iwnHookLog(ws);
            lastC = c;
        }
    }
    return 0;
}

extern "C" void DwmProbeInstall(HMODULE self) {
    if (InterlockedCompareExchange((volatile LONG*)&g_installed, 1, 0) != 0) return;
    g_selfBase = self;
    // 主屏像素尺寸（dwm 是 per-monitor DPI aware，这里拿到的是物理像素）。
    // 合成表面必然等于某个输出的像素尺寸，所以这个值同时是"精确判据"的来源。
    g_scrW = GetSystemMetrics(SM_CXSCREEN);
    g_scrH = GetSystemMetrics(SM_CYSCREEN);
    iwnHookLog("dwm: probe install begin");
    {
        char sb[96];
        wsprintfA(sb, "dwm: screen %ldx%ld (composite surface must match this)", g_scrW, g_scrH);
        iwnHookLog(sb);
    }

    HMODULE core = waitModule(L"dwmcore.dll", 30000);
    g_coreMod = core;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!core) {
        iwnHookLog("dwm: dwmcore.dll not loaded after 30s -> injection too early?");
    }
    char b[220];

    // 先把自己标成合法 CFG 调用目标，否则第一次间接调用就 fail-fast
    registerCfg((void*)&HookD3D11CreateDevice, "D3D11CreateDevice");
    registerCfg((void*)&HookOMSetRenderTargets, "OMSetRenderTargets");
    registerCfg((void*)&HookConfirmFrame, "ConfirmFrame");
    registerCfg((void*)&HookRetireFrame, "RetireFrame");

    // 1) D3D11CreateDevice（dwm.exe / dwmcore.dll 任一处）
    void* old = nullptr;
    if (iatSwap(findIatSlot(core, "d3d11.dll", "D3D11CreateDevice"),
                (void*)&HookD3D11CreateDevice, &old)) {
        g_origCreate = (PFN_CreateDevice)old;
        char om[64];
        ownerModuleOf(old, om, sizeof(om));
        wsprintfA(b, "dwm: IAT hooked d3d11!D3D11CreateDevice (dwmcore) prev=%s %08X",
                  om, (unsigned)(ULONG_PTR)old);
        iwnHookLog(b);
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
        char om[64];
        ownerModuleOf(o2, om, sizeof(om));
        wsprintfA(b, "dwm: IAT hooked win32u!NtDCompositionConfirmFrame prev=%s %08X",
                  om, (unsigned)(ULONG_PTR)o2);
        iwnHookLog(b);
    } else {
        iwnHookLog("dwm: ConfirmFrame IAT slot not found");
    }
    void* o3 = nullptr;
    if (iatSwap(findIatSlot(core, "win32u.dll", "NtDCompositionRetireFrame"),
                (void*)&HookRetireFrame, &o3)) {
        g_origRetire = o3;
        char om[64];
        ownerModuleOf(o3, om, sizeof(om));
        wsprintfA(b, "dwm: IAT hooked win32u!NtDCompositionRetireFrame prev=%s %08X",
                  om, (unsigned)(ULONG_PTR)o3);
        iwnHookLog(b);
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
    CreateThread(nullptr, 0, censusThread, nullptr, 0, nullptr);
}
