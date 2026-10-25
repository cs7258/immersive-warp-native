// Immersive Warp Native - in-process injection integration (round 17)
//
// Reuses the IWNInject.exe logic so the injection workflow can be driven
// remotely through CMDNET (and therefore OSC / MQTT):
//
//   INJECT LAUNCH <exe> [args...]    start suspended, inject, resume
//   INJECT PID <pid> [dll]           inject into a running process
//                                    (only catches swapchains the target
//                                    creates AFTER injection)
//
// The hook DLL defaults to WarpHook.dll next to the main executable, so a
// release-folder drop is self-contained. Self-injection (pid == our own
// process) is rejected on purpose: the engine already warps its own
// outputs, and hooking our own swapchains from inside would double-warp.
//
// CreateRemoteThread + LoadLibraryW, no driver, no persistence.

#include "Common.h"
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <set>

namespace {

// dwm.exe runs at System integrity: without SeDebugPrivilege OpenProcess
// fails with ERROR_ACCESS_DENIED. Best effort - report clearly on failure.
// Returns false + the reason so the caller can tell "not elevated" apart
// from "elevated but privilege grant failed" (nearly never happens).
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

// Remote thread body: LoadLibraryW(path) then store BOTH the returned module
// and GetLastError() back to the result buffer. A plain CreateRemoteThread on
// LoadLibraryW only reports "returned NULL" - the real rejection reason
// (missing dep 126, signature policy 577, DllMain failure 1114 ...) dies with
// the loader inside the target. Shellcode (x64, all absolutes encoded at
// build time, nothing position-relative):
//   sub rsp,0x28
//   movabs rcx,&path ; movabs rax,LoadLibraryW ; call rax
//   movabs r15,&result ; mov [r15],rax          ; module
//   movabs rax,GetLastError ; call rax
//   mov [r15+8],eax ; xor eax,eax ; add rsp,0x28 ; ret
bool injectInto(HANDLE proc, const wchar_t* dllPath, std::string& err) {
    const SIZE_T pathBytes = (lstrlenW(dllPath) + 1) * sizeof(wchar_t);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    FARPROC gleFn = GetProcAddress(k32, "GetLastError");
    if (!loadlib || !gleFn) { err = "kernel32 lookup failed"; return false; }

    // one remote block: [shellcode 64B pad][path][result 16B]
    static const BYTE sc[] = {
        0x48,0x83,0xEC,0x28,                                    // sub rsp,0x28
        0x48,0xB9, 0,0,0,0,0,0,0,0,                             // movabs rcx,path
        0x48,0xB8, 0,0,0,0,0,0,0,0,                             // movabs rax,loadlib
        0xFF,0xD0,                                              // call rax
        0x49,0xBF, 0,0,0,0,0,0,0,0,                             // movabs r15,result
        0x49,0x89,0x07,                                         // mov [r15],rax
        0x48,0xB8, 0,0,0,0,0,0,0,0,                             // movabs rax,gle
        0xFF,0xD0,                                              // call rax
        0x41,0x89,0x47,0x08,                                    // mov [r15+8],eax
        0x31,0xC0,                                              // xor eax,eax
        0x48,0x83,0xC4,0x28,                                    // add rsp,0x28
        0xC3                                                    // ret
    };
    const SIZE_T scOff = 0;
    const SIZE_T pathOff = 64;                      // past shellcode
    const SIZE_T resOff = pathOff + pathBytes;      // 16-byte result
    const SIZE_T total = resOff + 16;

    BYTE* block = (BYTE*)VirtualAllocEx(proc, nullptr, total,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!block) { err = "VirtualAllocEx gle=" + std::to_string(GetLastError()); return false; }
    BYTE local[64 + 520 + 16] = {};
    memcpy(local + scOff, sc, sizeof(sc));
    memcpy(local + pathOff, dllPath, pathBytes);

    // patch the three absolutes inside the shellcode (offsets must match the
    // byte table above: imm64 follows its 2-byte movabs prefix)
    ULONGLONG pathR = (ULONGLONG)((uintptr_t)block + pathOff);
    ULONGLONG resR  = (ULONGLONG)((uintptr_t)block + resOff);
    memcpy(local + 6,  &pathR, 8);
    memcpy(local + 16, &loadlib, 8);
    memcpy(local + 28, &resR, 8);
    memcpy(local + 41, &gleFn, 8);

    if (!WriteProcessMemory(proc, block, local, total, nullptr)) {
        err = "WriteProcessMemory gle=" + std::to_string(GetLastError());
        VirtualFreeEx(proc, block, 0, MEM_RELEASE);
        return false;
    }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)(block + scOff), nullptr, 0, nullptr);
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
    if (result[0]) return true;   // HMODULE - loaded
    DWORD gle = (DWORD)result[1];
    // translate the loader's real verdict for fast diagnosis
    const char* hint = "";
    if (gle == 126) hint = " (ERROR_MOD_NOT_FOUND: a dependency is missing in the target)";
    else if (gle == 577) hint = " (ERROR_INVALID_IMAGE_HASH: signature policy - dwm only loads MS-signed DLLs)";
    else if (gle == 1114) hint = " (ERROR_DLL_INIT_FAILED: our DllMain failed in the target)";
    else if (gle == 5) hint = " (ACCESS_DENIED: kernel code-integrity refuses unsigned DLLs in dwm.exe - injection path closed on this Windows build; use the Desktop Duplication engine path instead)";
    err = "LoadLibraryW in target failed gle=" + std::to_string(gle) + hint;
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

// resolve the dll argument: empty -> default path; verify it exists
std::wstring resolveDll(const std::wstring& dllIn, std::string& err) {
    std::wstring dll = dllIn.empty() ? hookDllDefault() : dllIn;
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
    std::wstring dll = resolveDll(dllIn, err);
    if (dll.empty()) return "ERR INJECT launch (" + err + ")\n";

    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) { cmdline += L" " + args; }
    // lpApplicationName does NOT get the PATH search treatment: a bare
    // "notepad.exe" fails with ERROR_FILE_NOT_FOUND. Resolve via SearchPath
    // when the caller passed no directory component (IWNInject demanded a
    // full path; the CMDNET form is friendlier).
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
    bool ok = injectInto(pi.hProcess, dll.c_str(), err);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!ok) {
        dbgLogLine("inject: launch failed (" + err + ") pid=" +
                   std::to_string((unsigned long)pi.dwProcessId));
        return "ERR INJECT launch (" + err + ", resumed anyway)\n";
    }
    dbgLogLine("inject: launch ok pid=" + std::to_string((unsigned long)pi.dwProcessId) +
               " dll=" + toNarrow(dll));
    return "OK INJECT launch pid=" + std::to_string((unsigned long)pi.dwProcessId) + "\n";
}

// INJECT PID: attach to a running process. Self-injection is rejected.
std::string injectPid(unsigned long pid, const std::wstring& dllIn) {
    if (pid == GetCurrentProcessId())
        return "ERR INJECT pid (self-injection not supported: engine already warps)\n";
    std::string err;
    std::wstring dll = resolveDll(dllIn, err);
    if (dll.empty()) return "ERR INJECT pid (" + err + ")\n";

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc)
        return "ERR INJECT pid (OpenProcess gle=" + std::to_string(GetLastError()) + ")\n";
    bool ok = injectInto(proc, dll.c_str(), err);
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
// so its existing per-monitor swapchains must carry the hook. dxgi.dll
// swapchain vtables are class-level shared, so WarpHook.dll's self-probe
// patch covers swapchains that already existed before the injection.
std::string injectDwm() {
    std::string privWhy;
    bool priv = enableDebugPriv(privWhy);
    bool elev = isElevated();
    dbgLogLine("inject: dwm elevated=" + std::to_string(elev) +
               (priv ? " seDebug=ok" : " seDebug=fail(" + privWhy + ")"));
    // Not elevated: OpenProcess on dwm.exe can only fail - tell the user
    // exactly what to do instead of a generic "run as administrator?".
    if (!elev)
        return "ERR INJECT dwm (NOT ELEVATED: right-click ImmersiveWarpNative.exe > "
               "Run as administrator, then press Enable Desktop warping again)\n";
    std::wstring dll = hookDllDefault();
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr) ||
        GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
        return "ERR INJECT dwm (WarpHook.dll not found next to exe)\n";

    static std::set<DWORD> g_done;   // LoadLibraryW twice = refcount only
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
            if (g_done.count(pid)) { pids += std::to_string(pid) + "(cached) "; ok++; continue; }
            HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                      FALSE, pid);
            if (!proc) {
                errs += "pid=" + std::to_string(pid) + " gle=" +
                        std::to_string(GetLastError()) + "; ";
                continue;
            }
            std::string err;
            if (injectInto(proc, full, err)) {
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
               "; " + errs + "dwm may be code-integrity protected - report this exact text)\n";
    return "OK INJECT dwm pids=" + pids + "\n";
}
