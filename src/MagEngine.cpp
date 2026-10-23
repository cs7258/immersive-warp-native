// DWM Magnification engine (engine=1).
//
// The output windows become magnifier host windows: the DWM compositor
// feeds them the source desktop content directly - no Desktop Duplication
// session, no render thread, no mesh. This is the same mechanism the
// original uses as its "DWM engine" and the right path for 1x1 full-screen
// desktop mirroring where geometric correction is not needed.
//
// Recursion safety: a magnifier host sitting on the mirrored desktop would
// show itself (infinite mirror). MagSetWindowFilterList with
// MW_FILTERMODE_EXCLUDE removes our own windows (every host + the console)
// from the magnified content - the official, non-hacky equivalent of
// WDA_EXCLUDEFROMCAPTURE for this API.

#include "Common.h"
#include <magnification.h>
#include <mutex>
#include <vector>

#pragma comment(lib, "magnification.lib")

static std::mutex g_magMutex;
static bool g_magReady = false;
static std::vector<HWND> g_magHosts;
static HWND g_magConsole = nullptr;   // excluded from the feed as well
static std::vector<HWND> g_magExtras; // test-pattern overlays etc.

// g_magMutex held: hosts + console + extras for every live host
static void applyFiltersLocked() {
    std::vector<HWND> filter = g_magHosts;
    if (g_magConsole) filter.push_back(g_magConsole);
    for (HWND e : g_magExtras) if (e && IsWindow(e)) filter.push_back(e);
    for (HWND h : g_magHosts)
        if (h && IsWindow(h))
            MagSetWindowFilterList(h, MW_FILTERMODE_EXCLUDE,
                                   (int)filter.size(), filter.data());
}

bool magEngineAvailable() {
    std::lock_guard<std::mutex> lk(g_magMutex);
    return g_magReady;
}

bool magEngineInit() {
    std::lock_guard<std::mutex> lk(g_magMutex);
    if (g_magReady) return true;
    g_magReady = MagInitialize() != FALSE;
    dbgLogLine(std::string("magEngineInit: ") + (g_magReady ? "ok" : "FAILED"));
    return g_magReady;
}

void magEngineShutdown() {
    std::lock_guard<std::mutex> lk(g_magMutex);
    if (!g_magReady) return;
    g_magReady = false;
    g_magHosts.clear();
    MagUninitialize();
    dbgLogLine("magEngineShutdown: done");
}

// host = the magnifier window (must be visible and created with
// WS_EX_LAYERED). srcDesktop = the desktop rectangle to mirror; the source
// content is scaled to fit the host client area.
bool magAttachOutput(HWND host, const RECT& srcDesktop) {
    std::lock_guard<std::mutex> lk(g_magMutex);
    if (!g_magReady || !host) return false;
    if (!MagSetWindowSource(host, srcDesktop)) {
        dbgLogLine("magAttachOutput: MagSetWindowSource failed gle=" +
                   std::to_string((long)GetLastError()));
        return false;
    }
    // exclude every host (incl. this one) plus the console from the feed
    g_magHosts.push_back(host);
    applyFiltersLocked();
    dbgLogLine("magAttachOutput: host attached, filter=" +
               std::to_string((long)g_magHosts.size() + (g_magConsole ? 1 : 0) + g_magExtras.size()));
    return true;
}

void magSetConsoleWindow(HWND ctl) {
    std::lock_guard<std::mutex> lk(g_magMutex);
    g_magConsole = ctl;
    applyFiltersLocked();
}

// extra windows that must never appear in the magnified feed (GDI test
// pattern overlays). Applies to every live host immediately.
void magAddFilter(HWND w) {
    std::lock_guard<std::mutex> lk(g_magMutex);
    if (w) g_magExtras.push_back(w);
    applyFiltersLocked();
}

void magRemoveFilter(HWND w) {
    std::lock_guard<std::mutex> lk(g_magMutex);
    for (size_t i = 0; i < g_magExtras.size(); i++)
        if (g_magExtras[i] == w) { g_magExtras.erase(g_magExtras.begin() + i); break; }
    applyFiltersLocked();
}

void magDetachAll() {
    std::lock_guard<std::mutex> lk(g_magMutex);
    for (HWND h : g_magHosts) {
        RECT empty{};
        MagSetWindowSource(h, empty);   // stops the magnifier feed
    }
    g_magHosts.clear();
}
