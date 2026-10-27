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

// ---------------------------------------------------------------- CFG 注册
// dwm.exe 全程开启 Control Flow Guard。IAT 槽里的间接 call 会被 CFG 校验，
// 而 manual-mapped 的 DLL 不在进程 CFG 位图里 —— 不注册的话第一次调用
// detour 就是 fail-fast（dwm 直接重启）。SetProcessValidCallTargets 允许
// 进程内把自己代码页标为合法调用目标。
struct CfgTargetInfo { ULONG_PTR Offset; ULONG_PTR Flags; };
typedef BOOL(WINAPI* PFN_SPVCT)(HANDLE, PVOID, SIZE_T, ULONG, CfgTargetInfo*);

static HMODULE g_selfBase = nullptr;   // 由 HookEntry 传入（manual map 不在加载器链表里）

static void registerCfg(void* fn, const char* tag) {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto fnSet = kb ? (PFN_SPVCT)GetProcAddress(kb, "SetProcessValidCallTargets") : nullptr;
    char b[180];
    if (!fnSet || !g_selfBase) {
        wsprintfA(b, "dwm: CFG register %s skipped (api=%d base=%d)", tag, fnSet != nullptr,
                  g_selfBase != nullptr);
        iwnHookLog(b);
        return;
    }
    ULONG_PTR page = (ULONG_PTR)fn & ~(ULONG_PTR)0xFFF;
    CfgTargetInfo ti = {};
    ti.Offset = (ULONG_PTR)fn - page;
    ti.Flags = 1;                       // CFG_CALL_TARGET_VALID
    SetLastError(0);
    BOOL ok = fnSet(GetCurrentProcess(), (PVOID)page, 0x1000, 1, &ti);
    wsprintfA(b, "dwm: CFG register %s ok=%d gle=%u", tag, ok, (unsigned)GetLastError());
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
static LONG WINAPI HookConfirmFrame(void* a1, void* a2, void* a3, void* a4);
static LONG WINAPI HookRetireFrame(void* a1, void* a2, void* a3, void* a4);

typedef HRESULT(WINAPI* PFN_CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_CreateDevice g_origCreate = nullptr;
static HRESULT(WINAPI* g_origOMSetRT)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*) = nullptr;
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
        if (InterlockedCompareExchange(&g_deepDone, 1, 0) == 0) {
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

// ------------------------------------------------- 晚注入：从内存里找回 dwm 的 D3D11 设备
// 本 DLL 通常是在 dwm 已经建好 D3D11 设备之后才注入的，D3D11CreateDevice 不会再来
// 第二次 —— 于是拿不到设备、也就拿不到合成 RT（实测 rtset=0 / rt=0x0 / fl=0）。
// 但 dwm 进程里必然存在活的 ID3D11Device / ID3D11DeviceContext 对象：
// 在 dwmcore 的可写段里扫描 "指针 -> 对象 -> vtable 三者都落在 d3d11.dll 映像内" 的
// 候选，再用 QueryInterface 验证（失败的候选被 __except 吞掉，不会崩 dwm）。
static HMODULE g_coreMod = nullptr;
static ID3D11Device* g_dev = nullptr;
static volatile LONG g_scanRounds = 0;

static bool ptrInImage(const BYTE* p, const BYTE* base, ULONG_PTR size) {
    return p >= base && p < base + size;
}

static ULONG_PTR imageSizeOf(HMODULE m) {
    if (!m) return 0;
    auto dos = (IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

// 返回 true 表示已经拿到设备（并已给 context vtable 打点）
static bool recoverDeviceByScan() {
    if (g_dev) return true;
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d || !g_coreMod) return false;
    BYTE* dbase = (BYTE*)d3d;
    ULONG_PTR dsize = imageSizeOf(d3d);
    if (!dsize) return false;

    auto dos = (IMAGE_DOS_HEADER*)g_coreMod;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)g_coreMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);

    InterlockedIncrement(&g_scanRounds);
    int scanned = 0, cand = 0;
    ID3D11Device* found = nullptr;

    for (int s = 0; s < nt->FileHeader.NumberOfSections && !found; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        BYTE* start = (BYTE*)g_coreMod + sec[s].VirtualAddress;
        SIZE_T len = sec[s].Misc.VirtualSize;
        if (!len) continue;
        for (SIZE_T off = 0; off + sizeof(void*) <= len; off += sizeof(void*)) {
            BYTE* p = start + off;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
            if (mbi.State != MEM_COMMIT) continue;
            DWORD prot = mbi.Protect & 0xFF;
            if (prot & PAGE_NOACCESS) continue;
            scanned++;
            void* q = *(void**)p;
            if (!q || !ptrInImage((BYTE*)q, dbase, dsize)) continue;
            void* vt = *(void**)q;
            if (!ptrInImage((BYTE*)vt, dbase, dsize)) continue;
            cand++;

            ID3D11Device* dev = nullptr;
            ID3D11DeviceContext* ctx = nullptr;
            bool ok = false;
            __try {
                IUnknown* u = (IUnknown*)q;
                if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
                    ok = true;
                } else if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11DeviceContext), (void**)&ctx)) && ctx) {
                    ctx->GetDevice(&dev);
                    ctx->Release();
                    ctx = nullptr;
                    ok = (dev != nullptr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
                dev = nullptr;
            }
            if (ok && dev) { found = dev; break; }
        }
    }

    char b[240];
    wsprintfA(b, "dwm: scan round %ld: qwords=%d cand=%d -> device=%s",
              g_scanRounds, scanned, cand, found ? "FOUND" : "no");
    iwnHookLog(b);
    if (!found) return false;

    g_dev = found;                       // 自己持有，不释放
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    __try { fl = g_dev->GetFeatureLevel(); } __except (EXCEPTION_EXECUTE_HANDLER) { fl = (D3D_FEATURE_LEVEL)0; }
    g_devFeature = (LONG)fl;
    wsprintfA(b, "dwm: device recovered by scan feature=0x%X", (unsigned)fl);
    iwnHookLog(b);

    ID3D11DeviceContext* ic = nullptr;
    if (SUCCEEDED(g_dev->GetImmediateContext(&ic)) && ic) {
        patchContextVtable(ic);
        ic->Release();
    }
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
        // 晚注入路径：设备早就建好了，D3D11CreateDevice 不会再来 —— 主动去内存里找
        if (!g_origOMSetRT && g_coreMod && g_scanRounds < 30) {
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
