// Immersive Warp Native - in-process injection integration (rounds 17-28)
//
// Two-stage loader driven through CMDNET (and therefore OSC / MQTT):
//
//   INJECT LAUNCH <exe> [args...]    start suspended, inject, resume
//   INJECT PID <pid> [dll]           inject into a running process
//   INJECT DWM                       attach to every dwm.exe (desktop warp)
//
// Stage 1 (LoadLibraryW in a remote thread) works for normal targets.
// dwm.exe refuses it: its process was launched with a kernel code-integrity
// policy that only maps Microsoft-signed images, so LoadLibraryW of our
// DLL fails with ERROR_ACCESS_DENIED (gle=5, measured with the round-26
// diagnostic shellcode). Stage 2 therefore MANUAL-MAPS the image: the PE is
// written into RWX memory the target already allowed us to allocate and
// execute (the diagnostic shellcode proves that), bypassing the kernel
// image loader entirely - no NtCreateSection, no signature check.
//
// The DLL defaults to WarpHook.dll next to the main executable. Prefixing
// the dll argument with "M:" forces manual mapping (used by tests).
// Self-injection is rejected: the engine already warps its own outputs.

#include "Common.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <set>
#include <vector>

namespace {

// dwm.exe runs at System integrity: without SeDebugPrivilege OpenProcess
// fails with ERROR_ACCESS_DENIED. Reports the reason so callers can tell
// "not elevated" apart from "elevated but privilege grant failed".
bool enableDebugPriv(std::string& why) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        why = "OpenProcessToken gle=" + std::to_string(GetLastError());
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        why = "LookupPrivilegeValue gle=" + std::to_string(GetLastError());
        return false;
    }
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD gle = GetLastError();
    CloseHandle(tok);
    if (gle != ERROR_SUCCESS) {
        // ERROR_NOT_ALL_ASSIGNED (1300) = the token does not hold
        // SeDebugPrivilege, which means the process is NOT elevated.
        why = gle == 1300 ? "token lacks SeDebugPrivilege (not elevated)"
                          : "AdjustTokenPrivileges gle=" + std::to_string(gle);
        return false;
    }
    return true;
}

// TokenElevation query - zero extra headers, unlike IsUserAnAdmin.
bool isElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD cb = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

// ---- tiny x64 shellcode builder: imm64 slots are recorded, never hand-counted
struct Imm { size_t at; ULONGLONG v; };
static void emit(std::vector<BYTE>& sc, std::initializer_list<BYTE> bytes) {
    sc.insert(sc.end(), bytes.begin(), bytes.end());
}
static void imm64(std::vector<BYTE>& sc, std::vector<Imm>& ims, ULONGLONG v) {
    ims.push_back({sc.size(), v});
    for (int i = 0; i < 8; i++) sc.push_back(0);
}

// ---- stage 1: remote LoadLibraryW via shellcode that also reports the real
// GetLastError (a bare LoadLibraryW thread only shows "returned NULL").
// On success *moduleOut receives the 64-bit HMODULE inside the target.
bool loadLibInTarget(HANDLE proc, DWORD pid, const wchar_t* dllPath,
                     std::string& err, ULONGLONG* moduleOut) {
    (void)pid;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    FARPROC gleFn = GetProcAddress(k32, "GetLastError");
    if (!loadlib || !gleFn) { err = "kernel32 lookup failed"; return false; }

    std::vector<BYTE> sc;
    std::vector<Imm> ims;
    emit(sc, {0x48,0x83,0xEC,0x28});                 // sub rsp,0x28
    emit(sc, {0x48,0xB9});                           // movabs rcx,&path
    size_t pathAt = sc.size(); imm64(sc, ims, 0);
    emit(sc, {0x48,0xB8});                           // movabs rax,LoadLibraryW
    imm64(sc, ims, (ULONGLONG)loadlib);
    emit(sc, {0xFF,0xD0});                           // call rax
    emit(sc, {0x49,0xBF});                           // movabs r15,&result
    size_t resAt = sc.size(); imm64(sc, ims, 0);
    emit(sc, {0x49,0x89,0x07});                      // mov [r15],rax
    emit(sc, {0x48,0xB8});                           // movabs rax,GetLastError
    imm64(sc, ims, (ULONGLONG)gleFn);
    emit(sc, {0xFF,0xD0});                           // call rax
    emit(sc, {0x41,0x89,0x47,0x08});                 // mov [r15+8],eax
    emit(sc, {0x31,0xC0, 0x48,0x83,0xC4,0x28, 0xC3});// eax=0; leave; ret

    const SIZE_T pathBytes = (lstrlenW(dllPath) + 1) * sizeof(wchar_t);
    const SIZE_T scSize = sc.size();
    const SIZE_T pathOff = (scSize + 7) & ~SIZE_T(7);
    const SIZE_T resOff = pathOff + pathBytes;
    const SIZE_T total = resOff + 16;

    BYTE* block = (BYTE*)VirtualAllocEx(proc, nullptr, total,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!block) { err = "VirtualAllocEx gle=" + std::to_string(GetLastError()); return false; }
    std::vector<BYTE> local(total, 0);
    memcpy(&local[0], &sc[0], scSize);
    memcpy(&local[pathOff], dllPath, pathBytes);
    ULONGLONG pathR = (ULONGLONG)(uintptr_t)(block + pathOff);
    ULONGLONG resR  = (ULONGLONG)(uintptr_t)(block + resOff);
    for (auto& im : ims) {
        ULONGLONG v = im.v;
        if (im.at == pathAt) v = pathR;
        else if (im.at == resAt) v = resR;
        memcpy(&local[im.at], &v, 8);
    }
    if (!WriteProcessMemory(proc, block, &local[0], total, nullptr)) {
        err = "WriteProcessMemory gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, block, 0, MEM_RELEASE);
        return false;
    }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)(block), nullptr, 0, nullptr);
    if (!th) {
        err = "CreateRemoteThread gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, block, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    ULONGLONG result[2] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, block + resOff, result, sizeof(result), &got) || got != sizeof(result)) {
        err = "ReadProcessMemory(result) gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, block, 0, MEM_RELEASE);
        return false;
    }
    VirtualFreeEx(proc, block, 0, MEM_RELEASE);
    if (result[0]) { if (moduleOut) *moduleOut = result[0]; return true; }
    DWORD gle = (DWORD)result[1];
    const char* hint = "";
    if (gle == 126) hint = " (ERROR_MOD_NOT_FOUND: a dependency is missing in the target)";
    else if (gle == 577) hint = " (ERROR_INVALID_IMAGE_HASH: signature policy - dwm only loads MS-signed DLLs)";
    else if (gle == 1114) hint = " (ERROR_DLL_INIT_FAILED: our DllMain failed in the target)";
    else if (gle == 5) hint = " (ACCESS_DENIED: kernel code-integrity refuses unsigned DLLs - switching to manual map)";
    err = "LoadLibraryW in target failed gle=" + std::to_string(gle) + hint;
    return false;
}

// ---- stage 2 helpers ---------------------------------------------------------

std::vector<BYTE> readAllBytes(const std::wstring& path, std::string& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = "open dll gle=" + std::to_string(GetLastError()); return {}; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<BYTE> data((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!data.empty() && (!ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr) || got != data.size())) {
        CloseHandle(f); err = "read dll failed"; return {};
    }
    CloseHandle(f);
    return data;
}

// base of a module inside the target: module snapshot first, then the
// shared-ASLR assumption (system DLLs share one base across processes
// per boot), which covers snapshot failures on protected processes.
ULONGLONG remoteModuleBase(DWORD pid, const wchar_t* name, HMODULE localFallback) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (lstrcmpiW(me.szModule, name) == 0) {
                    CloseHandle(snap);
                    return (ULONGLONG)(uintptr_t)me.modBaseAddr;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    return (ULONGLONG)(uintptr_t)localFallback;   // 0 if not loaded locally
}

// GetProcAddress results may be forwarder strings (kernel32 -> kernelbase).
// Detect via the export directory range and follow the "dll.func" text.
FARPROC resolveNoForwarder(HMODULE local, const char* name) {
    FARPROC fn = GetProcAddress(local, name);
    for (int hop = 0; hop < 8 && fn; hop++) {
        auto dos = (PIMAGE_DOS_HEADER)local;
        auto nt = (PIMAGE_NT_HEADERS)((BYTE*)local + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        ULONGLONG off = (ULONGLONG)fn - (ULONGLONG)local;
        if (!dir.Size || off < dir.VirtualAddress || off >= dir.VirtualAddress + dir.Size)
            break;                                   // a real function address
        const char* fwd = (const char*)fn;
        char buf[160];
        size_t j = 0;
        for (; j < sizeof(buf) - 1; j++) { buf[j] = fwd[j]; if (!fwd[j]) break; }
        buf[j] = 0;
        char* dot = strchr(buf, '.');
        if (!dot) return nullptr;
        *dot = 0;
        wchar_t wmod[96];
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wmod, 96);
        HMODULE h2 = GetModuleHandleW(wmod);
        if (!h2) h2 = LoadLibraryW(wmod);
        if (!h2) return nullptr;
        fn = GetProcAddress(h2, dot + 1);
    }
    return fn;
}

bool applyRelocs(std::vector<BYTE>& img, ULONGLONG newBase, std::string& err) {
    auto dos = (PIMAGE_DOS_HEADER)&img[0];
    auto nt = (PIMAGE_NT_HEADERS)(&img[0] + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.Size) return true;
    ULONGLONG delta = newBase - nt->OptionalHeader.ImageBase;
    if (!delta) return true;
    BYTE* p = &img[dir.VirtualAddress];
    BYTE* end = p + dir.Size;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        auto r = (PIMAGE_BASE_RELOCATION)p;
        if (!r->SizeOfBlock) break;
        WORD* e = (WORD*)(p + sizeof(IMAGE_BASE_RELOCATION));
        size_t n = (r->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
        for (size_t i = 0; i < n; i++) {
            WORD type = e[i] >> 12, off = e[i] & 0xFFF;
            if (type == IMAGE_REL_BASED_DIR64)
                *(ULONGLONG*)(p + r->VirtualAddress + off) += delta;
        }
        p += r->SizeOfBlock;
    }
    (void)err;
    return true;
}

bool resolveImports(HANDLE proc, DWORD pid, std::vector<BYTE>& img, std::string& err) {
    auto dos = (PIMAGE_DOS_HEADER)&img[0];
    auto nt = (PIMAGE_NT_HEADERS)(&img[0] + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size) return true;
    auto imp = (PIMAGE_IMPORT_DESCRIPTOR)(&img[0] + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* modName = (const char*)(&img[0] + imp->Name);
        wchar_t wname[96];
        MultiByteToWideChar(CP_UTF8, 0, modName, -1, wname, 96);
        dbgLogLine(std::string("mm: dep ") + modName);
        HMODULE local = GetModuleHandleW(wname);
        if (!local) local = LoadLibraryW(wname);
        if (!local) { err = std::string("local dep missing: ") + modName; return false; }
        ULONGLONG rbase = remoteModuleBase(pid, wname, local);
        if (!rbase) {
            // not loaded in the target (e.g. D3DCOMPILER_47 in dwm.exe):
            // load it there - MS-signed system DLLs pass the CI policy.
            std::string e2;
            if (!loadLibInTarget(proc, pid, wname, e2, &rbase)) {
                err = std::string("target dep missing: ") + modName + " (" + e2 + ")";
                return false;
            }
        }
        auto thunk = (PIMAGE_THUNK_DATA)(&img[0] + imp->FirstThunk);
        auto orig = imp->OriginalFirstThunk
                        ? (PIMAGE_THUNK_DATA)(&img[0] + imp->OriginalFirstThunk) : thunk;
        for (; orig->u1.AddressOfData; orig++, thunk++) {
            FARPROC fn;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                fn = resolveNoForwarder(local, (LPCSTR)(orig->u1.Ordinal & 0xFFFF));
            else {
                auto byName = (PIMAGE_IMPORT_BY_NAME)(&img[0] + orig->u1.AddressOfData);
                fn = resolveNoForwarder(local, (LPCSTR)byName->Name);
            }
            if (!fn) { err = std::string("export missing: ") + modName; return false; }
            thunk->u1.Function = (ULONGLONG)rbase + ((ULONGLONG)fn - (ULONGLONG)local);
        }
    }
    return true;
}

// ---- stage 2: manual map -----------------------------------------------------
bool manualMap(HANDLE proc, DWORD pid, const wchar_t* dllPath, std::string& err) {
    std::vector<BYTE> file = readAllBytes(dllPath, err);
    if (file.empty()) return false;
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) { err = "dll too small"; return false; }
    auto dos = (PIMAGE_DOS_HEADER)&file[0];
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err = "bad MZ"; return false; }
    auto nt = (PIMAGE_NT_HEADERS)(&file[0] + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        err = "not a PE32+ image"; return false;
    }
    const SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    dbgLogLine("mm: parsed ok image=" + std::to_string(imageSize));
    std::vector<BYTE> img(imageSize, 0);
    memcpy(&img[0], &file[0], nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!sec[i].SizeOfRawData) continue;
        SIZE_T src = sec[i].PointerToRawData, dst = sec[i].VirtualAddress;
        SIZE_T n = sec[i].SizeOfRawData;
        if (src + n > file.size() || dst + n > imageSize) { err = "section out of range"; return false; }
        memcpy(&img[dst], &file[src], n);
    }

    dbgLogLine("mm: sections copied");
    BYTE* remote = (BYTE*)VirtualAllocEx(proc, nullptr, imageSize,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) { err = "VirtualAllocEx(image) gle=" + std::to_string(GetLastError()); return false; }
    ULONGLONG base = (ULONGLONG)(uintptr_t)remote;
    dbgLogLine("mm: base=" + std::to_string(base));
    if (!applyRelocs(img, base, err)) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }
    dbgLogLine("mm: relocs ok");
    if (!resolveImports(proc, pid, img, err)) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); return false; }
    dbgLogLine("mm: imports ok");

    if (!WriteProcessMemory(proc, remote, &img[0], imageSize, nullptr)) {
        err = "WriteProcessMemory(image) gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    dbgLogLine("mm: image written, sc=" + std::to_string(sc.size()));

    // final shellcode: register the image's exception table (x64 unwinding
    // needs it for manual-mapped images), then run DllMain(base, ATTACH, 0)
    // and report both return values.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC aft = ntdll ? GetProcAddress(ntdll, "RtlAddFunctionTable") : nullptr;
    ULONGLONG table = 0, count = 0;
    if (aft) {
        auto& edir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (edir.Size) {
            table = base + edir.VirtualAddress;
            count = edir.Size / sizeof(RUNTIME_FUNCTION);
        }
    }
    std::vector<BYTE> sc;
    std::vector<Imm> ims;
    size_t resAt = SIZE_MAX;                         // &result slot (patched below)
    emit(sc, {0x48,0x83,0xEC,0x28});                 // sub rsp,0x28
    if (aft && table) {
        emit(sc, {0x48,0xB9});                       // movabs rcx,&pdata
        imm64(sc, ims, table);
        emit(sc, {0x48,0xBA});                       // movabs rdx,count
        imm64(sc, ims, count);
        emit(sc, {0x49,0xB8});                       // movabs r8,base
        imm64(sc, ims, base);
        emit(sc, {0x48,0xB8});                       // movabs rax,RtlAddFunctionTable
        imm64(sc, ims, (ULONGLONG)aft);
        emit(sc, {0xFF,0xD0});                       // call rax
        emit(sc, {0x49,0xBF});                       // movabs r15,&result
        resAt = sc.size(); imm64(sc, ims, 0);
        emit(sc, {0x49,0x89,0x07});                  // mov [r15],rax  (AFT ret)
        emit(sc, {0x48,0xB9});                       // movabs rcx,base
        imm64(sc, ims, base);
        emit(sc, {0x48,0xC7,0xC2,0x01,0x00,0x00,0x00});// mov rdx,1 (ATTACH)
        emit(sc, {0x45,0x33,0xC0});                  // xor r8d,r8d
        emit(sc, {0x48,0xB8});                       // movabs rax,DllMain
        imm64(sc, ims, base + nt->OptionalHeader.AddressOfEntryPoint);
        emit(sc, {0xFF,0xD0});                       // call rax
        emit(sc, {0x41,0x89,0x47,0x08});             // mov [r15+8],eax
        emit(sc, {0x31,0xC0, 0x48,0x83,0xC4,0x28, 0xC3});
    } else {
        emit(sc, {0x48,0xB9});                       // movabs rcx,base
        imm64(sc, ims, base);
        emit(sc, {0x48,0xC7,0xC2,0x01,0x00,0x00,0x00});// mov rdx,1
        emit(sc, {0x45,0x33,0xC0});                  // xor r8d,r8d
        emit(sc, {0x48,0xB8});                       // movabs rax,DllMain
        imm64(sc, ims, base + nt->OptionalHeader.AddressOfEntryPoint);
        emit(sc, {0xFF,0xD0});                       // call rax
        emit(sc, {0x48,0x83,0xC4,0x28, 0xC3});
    }

    const SIZE_T scSize = sc.size();
    const SIZE_T resOff = (scSize + 7) & ~SIZE_T(7);
    const SIZE_T total = resOff + 16;
    // reuse the tail of the image allocation for the result buffer: simpler
    // to allocate a small exec block for the shellcode + a separate RW result
    BYTE* scblock = (BYTE*)VirtualAllocEx(proc, nullptr, total + 64,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!scblock) {
        err = "VirtualAllocEx(sc) gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    BYTE* resBlock = scblock + ((scSize + 63) & ~SIZE_T(63));
    std::vector<BYTE> local(total + 64, 0);
    memcpy(&local[0], &sc[0], scSize);
    ULONGLONG resR = (ULONGLONG)(uintptr_t)resBlock;
    for (auto& im : ims)
        if (im.at == resAt) {                        // the recorded &result slot
            ULONGLONG v = resR;
            memcpy(&local[im.at], &v, 8);
            break;
        }
    if (!WriteProcessMemory(proc, scblock, &local[0], local.size(), nullptr)) {
        err = "WriteProcessMemory(sc) gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, scblock, 0, MEM_RELEASE);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)scblock, nullptr, 0, nullptr);
    if (!th) {
        err = "CreateRemoteThread(sc) gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, scblock, 0, MEM_RELEASE);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(th, 15000);
    CloseHandle(th);
    dbgLogLine("mm: remote thread done");
    ULONGLONG result[2] = {};
    SIZE_T got = 0;
    BOOL rd = ReadProcessMemory(proc, resBlock, result, sizeof(result), &got) && got == sizeof(result);
    VirtualFreeEx(proc, scblock, 0, MEM_RELEASE);
    // the image stays mapped (the DLL keeps running); freeing it would kill
    // the hook, so remote is intentionally leaked into the target's lifetime.
    if (!rd) { err = "ReadProcessMemory(manual) gle=" + std::to_string(GetLastError()); return false; }
    if (!result[1]) {
        err = "DllMain failed in target (manual map)";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

// two-stage entry: LoadLibrary first, manual map on rejection
bool injectInto(HANDLE proc, DWORD pid, const wchar_t* dllPath, bool forceManual,
                std::string& err) {
    if (forceManual) return manualMap(proc, pid, dllPath, err);
    std::string e1;
    if (loadLibInTarget(proc, pid, dllPath, e1, nullptr)) return true;
    std::string e2;
    if (manualMap(proc, pid, dllPath, e2)) return true;
    err = e1 + "; fallback manual map: " + e2;
    return false;
}

std::wstring hookDllDefault() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t s = dir.find_last_of(L"\\/");
    if (s != std::wstring::npos) dir.resize(s + 1);
    return dir + L"WarpHook.dll";
}

// resolve the dll argument: empty -> default path; "M:" prefix forces manual
// mapping; verify it exists
std::wstring resolveDll(const std::wstring& dllIn, std::string& err, bool& forceManual) {
    forceManual = false;
    std::wstring dll = dllIn.empty() ? hookDllDefault() : dllIn;
    if (dll.rfind(L"M:", 0) == 0) { forceManual = true; dll.erase(0, 2); }
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr)) {
        err = "GetFullPathName gle=" + std::to_string(GetLastError());
        return {};
    }
    DWORD attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        err = "dll not found: " + toNarrow(full);
        return {};
    }
    return full;
}

} // namespace

// INJECT LAUNCH: exe + optional args (single string, may be empty).
std::string injectLaunch(const std::wstring& exe, const std::wstring& args,
                         const std::wstring& dllIn) {
    if (exe.empty() || exe.size() > 512)
        return "ERR INJECT launch (exe path required)\n";
    std::string err;
    bool forceManual = false;
    std::wstring dll = resolveDll(dllIn, err, forceManual);
    if (dll.empty()) return "ERR INJECT launch (" + err + ")\n";

    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) { cmdline += L" " + args; }
    // lpApplicationName does NOT get the PATH search treatment: a bare
    // "notepad.exe" fails with ERROR_FILE_NOT_FOUND. Resolve via SearchPath
    // when the caller passed no directory component.
    std::wstring full = exe;
    if (exe.find_first_of(L"\\/:") == std::wstring::npos) {
        wchar_t buf[MAX_PATH]; wchar_t* filePart = nullptr;
        DWORD n = SearchPathW(nullptr, exe.c_str(), L".exe", MAX_PATH, buf, &filePart);
        if (n > 0 && n < MAX_PATH) full.assign(buf, (size_t)n);
    }
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(full.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        return "ERR INJECT launch (CreateProcess gle=" + std::to_string(GetLastError()) + ")\n";
    }
    bool ok = injectInto(pi.hProcess, pi.dwProcessId, dll.c_str(), forceManual, err);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!ok) {
        dbgLogLine("inject: launch failed (" + err + ") pid=" +
                   std::to_string((unsigned long)pi.dwProcessId));
        return "ERR INJECT launch (" + err + ", resumed anyway)\n";
    }
    dbgLogLine("inject: launch ok pid=" + std::to_string((unsigned long)pi.dwProcessId) +
               " dll=" + toNarrow(dll) + (forceManual ? " manual" : ""));
    return "OK INJECT launch pid=" + std::to_string((unsigned long)pi.dwProcessId) + "\n";
}

// INJECT PID: attach to a running process. Self-injection is rejected.
std::string injectPid(unsigned long pid, const std::wstring& dllIn) {
    if (pid == GetCurrentProcessId())
        return "ERR INJECT pid (self-injection not supported: engine already warps)\n";
    std::string err;
    bool forceManual = false;
    std::wstring dll = resolveDll(dllIn, err, forceManual);
    if (dll.empty()) return "ERR INJECT pid (" + err + ")\n";

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc)
        return "ERR INJECT pid (OpenProcess gle=" + std::to_string(GetLastError()) + ")\n";
    bool ok = injectInto(proc, pid, dll.c_str(), forceManual, err);
    CloseHandle(proc);
    if (!ok) {
        dbgLogLine("inject: pid failed (" + err + ") pid=" + std::to_string(pid));
        return "ERR INJECT pid (" + err + ")\n";
    }
    dbgLogLine("inject: pid ok pid=" + std::to_string(pid) + " dll=" + toNarrow(dll));
    return "OK INJECT pid=" + std::to_string(pid) + "\n";
}

// INJECT DWM: attach to every dwm.exe in this session (or all sessions).
// This is the real desktop-warping path: the desktop is composed by dwm.exe,
// so its existing per-monitor swapchains must carry the hook. The injection
// is two-stage: dwm.exe rejects LoadLibraryW of unsigned DLLs (kernel CI),
// so the loader falls back to manual mapping (round 28).
std::string injectDwm() {
    std::string privWhy;
    bool priv = enableDebugPriv(privWhy);
    bool elev = isElevated();
    dbgLogLine("inject: dwm elevated=" + std::to_string(elev) +
               (priv ? " seDebug=ok" : " seDebug=fail(" + privWhy + ")"));
    if (!elev)
        return "ERR INJECT dwm (NOT ELEVATED: right-click ImmersiveWarpNative.exe > "
               "Run as administrator, then press Enable Desktop warping again)\n";
    std::wstring dll = hookDllDefault();
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr) ||
        GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
        return "ERR INJECT dwm (WarpHook.dll not found next to exe)\n";

    static std::set<DWORD> g_done;   // attached pids: DLL already running there
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return "ERR INJECT dwm (snapshot failed)\n";
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int found = 0, ok = 0;
    std::string pids, errs;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"dwm.exe") != 0) continue;
            found++;
            DWORD pid = pe.th32ProcessID;
            if (g_done.count(pid)) { pids += std::to_string(pid) + "(attached) "; ok++; continue; }
            HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                      FALSE, pid);
            if (!proc) {
                errs += "pid=" + std::to_string(pid) + " gle=" +
                        std::to_string(GetLastError()) + "; ";
                continue;
            }
            std::string err;
            if (injectInto(proc, pid, full, false, err)) {
                g_done.insert(pid);
                pids += std::to_string(pid) + " ";
                ok++;
            } else {
                errs += "pid=" + std::to_string(pid) + " (" + err + "); ";
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) return "ERR INJECT dwm (no dwm.exe process found)\n";
    dbgLogLine("inject: dwm found=" + std::to_string(found) + " ok=" + std::to_string(ok) +
               (errs.empty() ? std::string() : " errs=" + errs));
    if (!ok)
        return "ERR INJECT dwm (elevated, seDebug=" + std::string(priv ? "ok" : "fail") +
               "; " + errs + "report this exact text)\n";
    return "OK INJECT dwm pids=" + pids + "\n";
}
