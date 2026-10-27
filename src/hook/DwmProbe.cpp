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

static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4) {
    InterlockedIncrement((volatile LONG*)&g_cntConfirm);
    probeActiveRT();
    typedef LONG(WINAPI* PF)(void*, void*, void*, void*);
    return g_origConfirm ? ((PF)g_origConfirm)(a1, a2, a3, a4) : 0;
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
// dwm 用硬件驱动，所以优先 HARDWARE；失败退 WARP。
static void buildVtableFingerprint() {
    if (g_vtDevice) return;
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d) { iwnHookLog("dwm: fingerprint no d3d11.dll"); return; }
    auto create = (PFN_CreateDevice)GetProcAddress(d3d, "D3D11CreateDevice");
    if (!create) { iwnHookLog("dwm: fingerprint no D3D11CreateDevice export"); return; }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    const D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
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
        char b[160];
        wsprintfA(b, "dwm: fingerprint type=%d hr=0x%08X dev=%08X",
                  (int)types[i], (unsigned)hr, (unsigned)(ULONG_PTR)dev);
        iwnHookLog(b);
        if (SUCCEEDED(hr) && dev) {
            g_vtDevice = *(void**)dev;
            if (ctx) g_vtCtx = *(void**)ctx;
            dev->Release();
            if (ctx) ctx->Release();
        }
    }
    char b[160];
    wsprintfA(b, "dwm: fingerprint vtDevice=%08X vtCtx=%08X",
              (unsigned)(ULONG_PTR)g_vtDevice, (unsigned)(ULONG_PTR)g_vtCtx);
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

extern "C" void DwmProbeInstall(HMODULE self) {
    if (InterlockedCompareExchange((volatile LONG*)&g_installed, 1, 0) != 0) return;
    g_selfBase = self;
    iwnHookLog("dwm: probe install begin");

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
}
