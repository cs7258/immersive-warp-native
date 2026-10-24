// Immersive Warp Native - IWNInject.exe (scanout pipeline Phase 1)
//
// Loads WarpHook.dll into a target process.
//
//   IWNInject.exe --launch <exe> [args...]   start suspended, inject, resume
//   IWNInject.exe --pid <pid> <dll>          inject into a running process
//                                            (only catches swapchains the
//                                            target creates AFTER injection)
//
// CreateRemoteThread + LoadLibraryW, no driver, no persistence.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

static bool injectInto(HANDLE proc, const wchar_t* dllPath) {
    const SIZE_T bytes = (lstrlenW(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { wprintf(L"VirtualAllocEx failed gle=%u\n", GetLastError()); return false; }
    if (!WriteProcessMemory(proc, remote, dllPath, bytes, nullptr)) {
        wprintf(L"WriteProcessMemory failed gle=%u\n", GetLastError()); return false;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadlib) { wprintf(L"LoadLibraryW not found\n"); return false; }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)loadlib, remote, 0, nullptr);
    if (!th) { wprintf(L"CreateRemoteThread failed gle=%u\n", GetLastError()); return false; }
    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    // x64 module handles have the low bits zeroed in the exit code; a zero
    // result means LoadLibraryW failed (missing file / bad arch / blocked).
    if (!code) { wprintf(L"LoadLibraryW returned NULL (dll not loaded)\n"); return false; }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage:\n  IWNInject.exe --launch <exe> [args...]\n  IWNInject.exe --pid <pid> <dll>\n");
        return 2;
    }
    if (!lstrcmpW(argv[1], L"--pid")) {
        if (argc < 4) { wprintf(L"--pid needs <pid> <dll>\n"); return 2; }
        DWORD pid = (DWORD)wcstoul(argv[2], nullptr, 10);
        HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
        if (!proc) { wprintf(L"OpenProcess(%u) failed gle=%u\n", pid, GetLastError()); return 1; }
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[3], MAX_PATH, full, nullptr);
        bool ok = injectInto(proc, full);
        CloseHandle(proc);
        wprintf(ok ? L"INJECTED pid=%u\n" : L"FAILED pid=%u\n", pid);
        return ok ? 0 : 1;
    }
    if (!lstrcmpW(argv[1], L"--launch")) {
        if (argc < 4) { wprintf(L"--launch needs <exe> <dll> [args...]\n"); return 2; }
        // argv[2]=exe, argv[3]=dll, argv[4..]=app args
        wchar_t cmdline[2048] = L"\"";
        lstrcatW(cmdline, argv[2]);
        lstrcatW(cmdline, L"\"");
        for (int i = 4; i < argc; i++) {
            lstrcatW(cmdline, L" ");
            lstrcatW(cmdline, argv[i]);
        }
        wchar_t fullDll[MAX_PATH];
        GetFullPathNameW(argv[3], MAX_PATH, fullDll, nullptr);
        STARTUPINFOW si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(argv[2], cmdline, nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            wprintf(L"CreateProcessW failed gle=%u\n", GetLastError());
            return 1;
        }
        bool ok = injectInto(pi.hProcess, fullDll);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        wprintf(ok ? L"INJECTED+RESUMED pid=%u\n" : L"INJECT FAILED pid=%u (resumed anyway)\n",
                pi.dwProcessId);
        return ok ? 0 : 1;
    }
    wprintf(L"unknown mode\n");
    return 2;
}
