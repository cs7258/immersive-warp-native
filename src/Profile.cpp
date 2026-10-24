// Profile watcher (original Profiles-page parity): a background thread
// enumerates processes every 2s, matches executable names against the
// configured wildcard rules, and injects WarpHook.dll into every new match.
// Original reference: "Executable name" wildcard + per-app auto attach.
//
// Threading: the watcher runs on its own thread and only touches g_cfg under
// g_cfgMutex (READ only - it never writes config). Injection reuses
// injectPid() which is lock-free and UI-free. Rule mutation happens on the
// CMDNET/MQTT thread; profileRulesChanged() bumps a generation counter the
// watcher re-reads each pass so rule edits apply within one poll cycle.
#include "Common.h"
#include <tlhelp32.h>
#include <algorithm>

static std::atomic<bool> g_profRun{ false };
static std::atomic<unsigned> g_profGen{ 0 };   // bumped on rule edits
static std::mutex g_profSeenMtx;
static std::vector<unsigned long> g_profSeen;  // pids we already injected

// ---- wildcard match: * and ?, case-insensitive, no external deps ----
static bool wcMatchWild(const wchar_t* pat, const wchar_t* s) {
    const wchar_t* star = nullptr, * ss = nullptr;
    while (*s) {
        if (*pat == L'?') { ++pat; ++s; }
        else if (*pat == L'*') { star = pat++; ss = s; }
        else {
            wchar_t a = towlower(*pat), b = towlower(*s);
            if (a != b) {
                if (star) { pat = star + 1; s = ++ss; continue; }
                return false;
            }
            ++pat; ++s;
        }
    }
    while (*pat == L'*') pat++;
    return *pat == L'\0';
}

static bool profSeenHas(unsigned long pid) {
    std::lock_guard<std::mutex> lk(g_profSeenMtx);
    return std::find(g_profSeen.begin(), g_profSeen.end(), pid) != g_profSeen.end();
}
static void profSeenAdd(unsigned long pid) {
    std::lock_guard<std::mutex> lk(g_profSeenMtx);
    g_profSeen.push_back(pid);
}
// drop pids that exited (pid reuse after death would otherwise be skipped)
static void profSeenSweep() {
    std::lock_guard<std::mutex> lk(g_profSeenMtx);
    g_profSeen.erase(std::remove_if(g_profSeen.begin(), g_profSeen.end(),
        [](unsigned long pid) {
            if (pid == GetCurrentProcessId()) return false;   // keep self parked
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!p) return true;                              // gone
            DWORD code = 0;
            GetExitCodeProcess(p, &code);
            CloseHandle(p);
            return code != STILL_ACTIVE;
        }), g_profSeen.end());
}

// snapshot the active rules + generation under the config lock
static unsigned profRulesSnapshot(std::vector<std::wstring>& out) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    out = g_cfg.profExe;
    return g_profGen.load();
}

static void profilerThreadFn() {
    for (;;) {
        if (!g_profRun.load()) break;
        std::vector<std::wstring> rules;
        profRulesSnapshot(rules);
        if (!rules.empty()) {
            // enumerate pids via Toolhelp (tlhelp32.h - zero extra libs; the
            // K32EnumProcesses route needs psapi versioning gymnastics)
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe)) {
                    do {
                        if (!g_profRun.load()) break;
                        DWORD pid = pe.th32ProcessID;
                        if (!pid || pid == GetCurrentProcessId()) continue;
                        if (profSeenHas(pid)) continue;
                        // Toolhelp already hands us the exe basename - no
                        // OpenProcess/QueryFullProcessImageName needed for
                        // matching (avoids "Access denied" noise on system
                        // processes entirely)
                        bool match = false;
                        for (auto& r : rules)
                            if (wcMatchWild(r.c_str(), pe.szExeFile)) { match = true; break; }
                        if (!match) continue;
                        // inject; mark seen regardless of outcome so a target
                        // that refuses (e.g. protected process) is not hammered
                        profSeenAdd(pid);
                        std::string res = injectPid(pid, L"");
                        dbgLogLine("profile: auto-inject pid=" + std::to_string(pid) +
                                   " exe=" + toNarrow(pe.szExeFile) + " -> " + res);
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
            }
        }
        profSeenSweep();
        // sleep in short slices so Stop is responsive
        for (int i = 0; i < 20 && g_profRun.load(); i++) Sleep(100);
    }
}

static std::thread g_profThread;

void profilerStart() {
    if (g_profRun.load()) return;
    g_profRun.store(true);
    g_profThread = std::thread(profilerThreadFn);
}

void profilerStop() {
    if (!g_profRun.load()) return;
    g_profRun.store(false);
    if (g_profThread.joinable()) g_profThread.join();
}

bool profilerRunning() { return g_profRun.load(); }

void profileRulesChanged() { g_profGen++; }

int profileRulesCount() {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return (int)g_cfg.profExe.size();
}
