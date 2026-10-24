#include "Common.h"
#include "Engine.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <map>
#include <vector>
#include <algorithm>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ------------------------------------------------------------------
// Output windows (fullscreen per display) + calibration interaction
// ------------------------------------------------------------------
struct OutWinState {
    int displayIdx = -1;
    int subMode = 0;              // 0 = geometrical correction, 1 = edge blending
    int dragKind = -1;            // -1 none, 0 warp point, 1 simple blend side, 2 advanced point, 3 window move, 4 mask rubber band
    int dragIdx = -1;             // warp point index or blend side index
    int advIdx = -1;              // advanced point index
    float lastX = 0, lastY = 0;
    int moveOffX = 0, moveOffY = 0;   // preview window drag offset
    // pending mask rubber band (right-drag), normalized; the render thread
    // overlays it as an extra black rect so the user sees the area live
    std::atomic<bool> pend{ false };
    float pendX0 = 0, pendY0 = 0, pendX1 = 0, pendY1 = 0;
};
static const wchar_t* OUT_CLASS = L"IWN_OutputWnd";
static const wchar_t* MAG_CLASS = L"IWN_MagWnd";   // engine=1 magnifier host
static const wchar_t* MAGTEST_CLASS = L"IWN_MagTestWnd"; // engine=1 test pattern
static void syncMagTestOverlays(bool on);   // fwd: createOutputWindows calls it
static void syncMagMaskOverlays();          // fwd: createOutputWindows calls it
static void syncMagProbeOverlays(bool on);  // fwd: WM_MAGPROBE handler + createOutputWindows

// GDI replica of the DXGI test pattern (Engine.cpp testPattern shader):
// 24x16 checker, 12x8 grid lines, red border, green center cross. Drawn in
// an overlay on top of the magnifier host because the MAG engine has no
// render pipeline of its own.
static LRESULT CALLBACK MagTestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;
        // 24x16 checker (same greys as the shader)
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 24; col++) {
                if ((col + row) % 2 == 0) continue;
                RECT cell{ col * W / 24, row * H / 16,
                           (col + 1) * W / 24, (row + 1) * H / 16 };
                HBRUSH b = CreateSolidBrush(RGB(97, 97, 97));   // 0.38
                FillRect(dc, &cell, b);
                DeleteObject(b);
            }
        // base 0.06 grey where the checker is dark
        HBRUSH dark = CreateSolidBrush(RGB(15, 15, 15));
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 24; col++) {
                if ((col + row) % 2 != 0) continue;
                RECT cell{ col * W / 24, row * H / 16,
                           (col + 1) * W / 24, (row + 1) * H / 16 };
                FillRect(dc, &cell, dark);
            }
        DeleteObject(dark);
        // 12x8 grid lines
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(217, 217, 217));   // 0.85
        HGDIOBJ oldPen = SelectObject(dc, gridPen);
        for (int c = 1; c < 12; c++) {
            MoveToEx(dc, c * W / 12, 0, nullptr);
            LineTo(dc, c * W / 12, H);
        }
        for (int r = 1; r < 8; r++) {
            MoveToEx(dc, 0, r * H / 8, nullptr);
            LineTo(dc, W, r * H / 8);
        }
        SelectObject(dc, oldPen);
        DeleteObject(gridPen);
        // red border
        HPEN redPen = CreatePen(PS_SOLID, 3, RGB(255, 64, 64));
        oldPen = SelectObject(dc, redPen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 1, 1, W - 1, H - 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(redPen);
        // green center cross
        HPEN greenPen = CreatePen(PS_SOLID, 1, RGB(38, 255, 38));
        oldPen = SelectObject(dc, greenPen);
        MoveToEx(dc, W / 2, 0, nullptr); LineTo(dc, W / 2, H);
        MoveToEx(dc, 0, H / 2, nullptr); LineTo(dc, W, H / 2);
        SelectObject(dc, oldPen);
        DeleteObject(greenPen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Magnifier host proc: the Magnification API paints through the default
// window procedure, we only keep click-to-raise disabled like DXGI outputs.
static LRESULT CALLBACK MagWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static std::vector<std::pair<HWND, OutWinState*>> g_outWins;
static std::vector<HWND> g_magTests;    // MAG test-pattern overlays (UI thread)
static std::vector<HWND> g_magMasks;    // MAG black-mask overlays (UI thread)
static std::vector<HWND> g_magProbes;   // MAG probe-marker overlays (UI thread)
static std::vector<HWND> g_magBlends;   // MAG edge-blend overlays (UI thread)
constexpr UINT WM_MAGBLEND = WM_APP + 13;   // MAG blend overlays resync (defined
                                            // before requestMagBlendSync uses it)

static OutputCfg* cfgForWindowNoLock(OutWinState* st) {
    // caller must already hold g_cfgMutex
    auto displays = enumerateDisplays();
    if (st->displayIdx < 0 || st->displayIdx >= (int)displays.size()) return nullptr;
    for (auto& o : g_cfg.outputs)
        if (o.enabled && o.deviceName == displays[st->displayIdx].deviceName) return &o;
    return nullptr;
}

static OutputCfg* cfgForWindow(OutWinState* st) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return cfgForWindowNoLock(st);
}

// render-thread bridge: the in-progress right-drag rectangle, normalized and
// min/max ordered, so Engine can overlay it as a temporary black rect
bool getPendingMask(HWND hwnd, float m[4]) {
    for (auto& w : g_outWins) {
        if (w.first != hwnd) continue;
        if (!w.second->pend.load()) return false;
        float x0 = w.second->pendX0, x1 = w.second->pendX1;
        float y0 = w.second->pendY0, y1 = w.second->pendY1;
        m[0] = (std::min)(x0, x1); m[1] = (std::min)(y0, y1);
        m[2] = std::fabs(x1 - x0); m[3] = std::fabs(y1 - y0);
        return true;
    }
    return false;
}

static void normFromLparam(HWND hwnd, LPARAM lp, float& nx, float& ny) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    nx = clampf(GET_X_LPARAM(lp) / (float)(w > 0 ? w : 1), 0, 1);
    ny = clampf(GET_Y_LPARAM(lp) / (float)(h > 0 ? h : 1), 0, 1);
}

static float normPointRadius(HWND hwnd) { return 12.0f; }

static bool hitTestWarp(OutWinState* st, HWND hwnd, float nx, float ny, int& idxOut) {
    RECT rc; GetClientRect(hwnd, &rc);
    float px = 1.0f / (rc.right - rc.left);
    auto* cfg = cfgForWindow(st);
    if (!cfg) return false;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    float best = normPointRadius(hwnd) * px;
    bool found = false;
    for (size_t i = 0; i + 1 < cfg->points.size(); i += 2) {
        float d = std::hypot(cfg->points[i] - nx, cfg->points[i + 1] - ny);
        if (d < best) { best = d; idxOut = (int)(i / 2); found = true; }
    }
    return found;
}

// returns side index 0..3, sets advIdx (-1 = simple)
static bool hitTestBlend(OutWinState* st, HWND hwnd, float nx, float ny, int& sideOut, int& advOut) {
    RECT rc; GetClientRect(hwnd, &rc);
    float px = 1.0f / (rc.right - rc.left);
    auto* cfg = cfgForWindow(st);
    if (!cfg) return false;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    float best = normPointRadius(hwnd) * px;
    bool found = false;
    for (int s = 0; s < 4; s++) {
        if (g_cfg.advancedBlend) {
            for (size_t i = 0; i < cfg->adv[s].size(); i++) {                auto& p = cfg->adv[s][i];
                float x, y;
                if (s == 0) { x = p.fade; y = p.pos; }
                else if (s == 1) { x = 1 - p.fade; y = p.pos; }
                else if (s == 2) { x = p.pos; y = p.fade; }
                else { x = p.pos; y = 1 - p.fade; }
                float d = std::hypot(x - nx, y - ny);
                if (d < best) { best = d; sideOut = s; advOut = (int)i; found = true; }
            }
        } else {
            auto& b = cfg->blend[s];
            // handles are visible/draggable even at fade=0 (parked at the edge)
            float x, y;
            if (s == 0) { x = b.fade; y = 0.5f; }
            else if (s == 1) { x = 1 - b.fade; y = 0.5f; }
            else if (s == 2) { x = 0.5f; y = b.fade; }
            else { x = 0.5f; y = 1 - b.fade; }
            float d = std::hypot(x - nx, y - ny);
            if (d < best) { best = d; sideOut = s; advOut = -1; found = true; }
        }
    }
    return found;
}

static LRESULT CALLBACK OutputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (OutWinState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_KEYDOWN:
        if (!g_calib.load()) break;
        if (wp == VK_F10 && st) { st->subMode = 1 - st->subMode; return 0; }
        if (wp == VK_ESCAPE) { setCalibrationMode(false); return 0; }
        if (wp >= VK_LEFT && wp <= VK_DOWN && st) { // nudge selected point
            float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.01f : 0.002f;
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            if (auto* cfg = cfgForWindowNoLock(st)) {
                if (st->subMode == 0 && st->dragKind == 0) {
                    int i = st->dragIdx * 2;
                    if (i + 1 < (int)cfg->points.size()) {
                        if (wp == VK_LEFT) cfg->points[i] -= step;
                        if (wp == VK_RIGHT) cfg->points[i] += step;
                        if (wp == VK_UP) cfg->points[i + 1] -= step;
                        if (wp == VK_DOWN) cfg->points[i + 1] += step;
                        cfg->points[i] = clampf(cfg->points[i], 0, 1);
                        cfg->points[i + 1] = clampf(cfg->points[i + 1], 0, 1);
                        g_rev++;
                    }
                }
            }
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (!st) break;
        if (!g_calib.load()) {
            // windowed preview: the output window is movable by dragging
            if (g_previewMode.load()) {
                POINT pt{}; GetCursorPos(&pt);
                RECT wr{}; GetWindowRect(hwnd, &wr);
                st->moveOffX = pt.x - wr.left; st->moveOffY = pt.y - wr.top;
                st->dragKind = 3;
                SetCapture(hwnd);
            }
            return 0;
        }
        RECT rc; GetClientRect(hwnd, &rc);
        float nx = GET_X_LPARAM(lp) / (float)(rc.right - rc.left);
        float ny = GET_Y_LPARAM(lp) / (float)(rc.bottom - rc.top);
        SetCapture(hwnd);
        if (st->subMode == 0) {
            int idx = -1;
            if (hitTestWarp(st, hwnd, nx, ny, idx)) {
                st->dragKind = 0; st->dragIdx = idx;
                st->lastX = nx; st->lastY = ny;
            }
        } else {
            int side = -1, adv = -1;
            if (hitTestBlend(st, hwnd, nx, ny, side, adv)) {
                st->dragKind = adv >= 0 ? 2 : 1;
                st->dragIdx = side; st->advIdx = adv;
                st->lastX = nx; st->lastY = ny;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!st || st->dragKind < 0) break;
        if (st->dragKind == 3) {   // preview window move
            POINT pt{}; GetCursorPos(&pt);
            SetWindowPos(hwnd, HWND_TOPMOST, pt.x - st->moveOffX, pt.y - st->moveOffY, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        if (st->dragKind == 4) {   // mask rubber band (any mode)
            float nx, ny;
            normFromLparam(hwnd, lp, nx, ny);
            st->pendX1 = nx; st->pendY1 = ny;
            return 0;
        }
        if (!g_calib.load()) break;
        RECT rc; GetClientRect(hwnd, &rc);
        float nx = GET_X_LPARAM(lp) / (float)(rc.right - rc.left);
        float ny = GET_Y_LPARAM(lp) / (float)(rc.bottom - rc.top);
        float dx = nx - st->lastX, dy = ny - st->lastY;
        st->lastX = nx; st->lastY = ny;
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (auto* cfg = cfgForWindowNoLock(st)) {
            if (st->dragKind == 0) {
                int i = st->dragIdx * 2;
                if (i + 1 < (int)cfg->points.size()) {
                    cfg->points[i] = clampf(cfg->points[i] + dx, 0, 1);
                    cfg->points[i + 1] = clampf(cfg->points[i + 1] + dy, 0, 1);
                    g_rev++;
                }
            } else if (st->dragKind == 1) {
                auto& b = cfg->blend[st->dragIdx];
                if (st->dragIdx == 0) b.fade = clampf(b.fade + dx, 0.001f, 0.5f);
                if (st->dragIdx == 1) b.fade = clampf(b.fade - dx, 0.001f, 0.5f);
                if (st->dragIdx == 2) b.fade = clampf(b.fade + dy, 0.001f, 0.5f);
                if (st->dragIdx == 3) b.fade = clampf(b.fade - dy, 0.001f, 0.5f);
                g_rev++;
            } else if (st->dragKind == 2) {
                auto& p = cfg->adv[st->dragIdx][st->advIdx];
                bool horiz = (st->dragIdx == 0 || st->dragIdx == 1);
                if (horiz) {
                    p.pos = clampf(p.pos + dy, 0, 1);
                    p.fade = clampf(p.fade + (st->dragIdx == 0 ? dx : -dx), 0.001f, 0.5f);
                } else {
                    p.pos = clampf(p.pos + dx, 0, 1);
                    p.fade = clampf(p.fade + (st->dragIdx == 2 ? dy : -dy), 0.001f, 0.5f);
                }
                g_rev++;
            }
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        // right-drag = draw a black mask rectangle, works in every mode
        // (masks are content blocking, not a calibration-only feature)
        if (!st || st->dragKind >= 0) break;
        float nx, ny;
        normFromLparam(hwnd, lp, nx, ny);
        st->pendX0 = st->pendX1 = nx;
        st->pendY0 = st->pendY1 = ny;
        st->pend.store(true);
        st->dragKind = 4;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (st) { st->dragKind = -1; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONUP: {
        if (!st || st->dragKind != 4) break;
        float nx, ny;
        normFromLparam(hwnd, lp, nx, ny);
        st->pendX1 = nx; st->pendY1 = ny;
        st->pend.store(false);          // stop overlaying before mutating config
        float x0 = (std::min)(st->pendX0, st->pendX1), x1 = (std::max)(st->pendX0, st->pendX1);
        float y0 = (std::min)(st->pendY0, st->pendY1), y1 = (std::max)(st->pendY0, st->pendY1);
        float w = x1 - x0, h = y1 - y0;
        st->dragKind = -1;
        ReleaseCapture();
        if (w < 0.01f || h < 0.01f) return 0;   // click, not a drag
        w = (std::min)(w, 1.0f - x0); h = (std::min)(h, 1.0f - y0);
        bool added = false;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            if (auto* cfg = cfgForWindowNoLock(st)) {
                if (cfg->masks.size() < MAX_MASKS) {
                    cfg->masks.push_back(MaskRect{ x0, y0, w, h });
                    added = true;
                } else {
                    dbgLogLine("mask drag dropped: max 8 masks");
                }
            }
        }
        if (added) refreshStatusBar();   // locks g_cfgMutex itself
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (st && st->dragKind == 4) { st->dragKind = -1; st->pend.store(false); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!g_calib.load() || !st || st->dragKind < 1 || st->dragKind == 4) break;
        float dir = (short)HIWORD(wp) > 0 ? 0.05f : -0.05f;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (auto* cfg = cfgForWindowNoLock(st)) {
            // wheel = gamma (original behaviour), Ctrl+wheel = gain,
            // Shift+wheel = slope of the parametric blend curve
            if (st->dragKind == 1) {
                auto& b = cfg->blend[st->dragIdx];
                if (ctrl)      b.gain  = clampf(b.gain + dir, 0.0f, 2.0f);
                else if (shift) b.slope = clampf(b.slope + dir, 0.0f, 1.0f);
                else           b.gamma = clampf(b.gamma + dir, 0.1f, 6.0f);
            } else {
                auto& p = cfg->adv[st->dragIdx][st->advIdx];
                p.gamma = clampf(p.gamma + dir, 0.1f, 6.0f);
            }
            g_rev++;
        }
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        dbgLogLine("output window destroyed");
        for (size_t i = 0; i < g_outWins.size(); i++)
            if (g_outWins[i].first == hwnd) { delete g_outWins[i].second; g_outWins.erase(g_outWins.begin() + i); g_outCount.store((int)g_outWins.size()); break; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void createOutputWindows() {
    // Idempotent: main() starts the outputs and the UI can ask again (preview
    // toggle, Start Output). A second call used to stack a duplicate window on
    // the same display, and the second DuplicateOutput on that display then
    // failed with E_INVALIDARG -> black output.
    if (!g_outWins.empty()) destroyOutputWindows();
    WNDCLASSW wc{};
    wc.lpfnWndProc = OutputWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = OUT_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW mc{};
    mc.lpfnWndProc = MagWndProc;
    mc.hInstance = GetModuleHandleW(nullptr);
    mc.lpszClassName = MAG_CLASS;
    RegisterClassW(&mc);

    auto displays = enumerateDisplays();
    dbgLogLine("createOutputWindows: displays=" + std::to_string(displays.size()) +
               " preview=" + std::to_string((int)g_previewMode.load()) +
               " engine=" + std::to_string(g_cfg.engine) +
               " tid=" + std::to_string((long)GetCurrentThreadId()));
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    // source desktop rectangle for the Magnification hosts: pinned source,
    // else the first display that is not an enabled output; a single-display
    // machine mirrors the primary monitor (hosts are filter-excluded so no
    // infinite-mirror recursion)
    auto sourceRect = [&]() -> RECT {
        std::wstring src = g_cfg.sourceDev;
        if (src.empty()) {
            for (auto& d : displays) {
                bool isOut = false;
                for (auto& o : g_cfg.outputs)
                    if (o.enabled && o.deviceName == d.deviceName) { isOut = true; break; }
                if (!isOut) { src = d.deviceName; break; }
            }
        }
        if (!src.empty())
            for (auto& d : displays)
                if (d.deviceName == src) return d.desktop;
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &mi))
            return mi.rcMonitor;
        return displays.empty() ? RECT{0, 0, 1920, 1080} : displays[0].desktop;
    };
    int n = 0;
    // tile grid index per enabled output (same physical ordering as
    // applyLayoutLocked) so MAG hosts can mirror their tile's source window
    std::map<std::wstring, std::pair<int, int>> tileOf;
    {
        int R = g_cfg.layoutR, C = g_cfg.layoutC;
        struct Placed { RECT rc; const std::wstring* name; };
        std::vector<Placed> pl;
        for (auto& d : displays)
            for (auto& o : g_cfg.outputs)
                if (o.enabled && o.deviceName == d.deviceName)
                    pl.push_back({ d.desktop, &o.deviceName });
        std::sort(pl.begin(), pl.end(), [](const Placed& a, const Placed& b) {
            if (a.rc.top / 100 != b.rc.top / 100) return a.rc.top < b.rc.top;
            return a.rc.left < b.rc.left;
        });
        for (int k = 0; k < (int)pl.size(); k++)
            tileOf[*pl[k].name] = { (R > 1) ? k / C : 0, (R > 1) ? k % C : (C > 1 ? k : 0) };
    }
    for (size_t i = 0; i < displays.size(); i++) {
        OutputCfg* cfg = nullptr;
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == displays[i].deviceName) { cfg = &o; break; }
        if (!cfg) {
            dbgLogLine("createOutputWindows: no enabled cfg for " +
                       std::string(displays[i].deviceName.begin(), displays[i].deviceName.end()));
            continue;
        }
        n++;
        auto* st = new OutWinState();
        st->displayIdx = (int)i;
        st->subMode = g_calibMode.load();
        // Window title identifies the window for show-control / test tooling:
        // "<label>   [src DISPLAY2]". Auto resolves to the first display that
        // is not an enabled output; pinned source shows as-is. g_cfg.sourceDev
        // is read directly because we already hold g_cfgMutex here
        // (currentSourceDevice() would lock it again -> deadlock).
        std::wstring title = cfg->label.empty() ? cfg->deviceName : cfg->label;
        {
            std::wstring src = g_cfg.sourceDev;
            if (src.empty()) {
                for (auto& d : displays) {
                    bool isOut = false;
                    for (auto& o : g_cfg.outputs)
                        if (o.enabled && o.deviceName == d.deviceName) { isOut = true; break; }
                    if (!isOut) { src = d.deviceName; break; }
                }
            }
            if (!src.empty()) {
                std::wstring shortSrc = src;
                size_t p = shortSrc.find(L"\\\\.\\");
                if (p != std::wstring::npos) shortSrc = shortSrc.substr(p + 4);
                title += L"   [src " + shortSrc + L"]";
            }
        }
        RECT r = displays[i].desktop;
        bool preview = g_previewMode.load();
        bool inplace = g_inplace.load();
        if (g_cfg.engine == 1) {
            // DWM Magnification host window: WS_EX_LAYERED is required by
            // the API. In-place keeps its click-through semantics; the mag
            // feed never shows our own windows (filter list in MagEngine).
            HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                        WS_EX_LAYERED |
                                        (inplace && !g_calib.load() ? WS_EX_TRANSPARENT : 0),
                MAG_CLASS, title.c_str(), WS_POPUP | WS_VISIBLE,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (inplace) {
                // same capture-exclusion as DXGI in-place (belt and braces;
                // the mag filter list already covers the recursion case)
                typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
                auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                   "SetWindowDisplayAffinity");
                if (fn) fn(hwnd, 0x11);
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            g_outWins.push_back({ hwnd, st });
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            // NxN tile: the host mirrors THIS tile's source window - same
            // geometry as the DXGI pipeline (screen x shows source
            // x + c*(1-ov)), clamped to the source desktop.
            RECT src = sourceRect();
            auto it = tileOf.find(cfg->deviceName);
            int tr = it != tileOf.end() ? it->second.first : 0;
            int tc = it != tileOf.end() ? it->second.second : 0;
            float ovl = g_cfg.overlap / 100.0f;
            LONG sw = src.right - src.left, sh = src.bottom - src.top;
            RECT tile{
                src.left + (LONG)(clampf(tc * (1.0f - ovl), 0.0f, 1.0f) * sw),
                src.top + (LONG)(clampf(tr * (1.0f - ovl), 0.0f, 1.0f) * sh),
                src.left + (LONG)(clampf(tc * (1.0f - ovl) + 1.0f, 0.0f, 1.0f) * sw),
                src.top + (LONG)(clampf(tr * (1.0f - ovl) + 1.0f, 0.0f, 1.0f) * sh) };
            if (magEngineInit())
                magAttachOutput(hwnd, tile);
            else
                dbgLogLine("magEngineInit failed - mag output stays black");
            continue;   // the render thread owns nothing in MAG mode
        }
        if (preview) {
            // windowed preview: 60% of the monitor, centered. Combined with
            // WDA_EXCLUDEFROMCAPTURE this allows live desktop warping on a
            // single display without capturing our own output (no recursion).
            int mw = r.right - r.left, mh = r.bottom - r.top;
            int pw = (int)(mw * 0.6f), ph = (int)(mh * 0.6f);
            r.left += (mw - pw) / 2; r.top += (mh - ph) / 2;
            r.right = r.left + pw; r.bottom = r.top + ph;
        }
        // WS_EX_NOACTIVATE: clicking the output never raises it above the
        // console and never steals keyboard focus (hotkeys handle F3/F10/Esc).
        // In-place overlay adds WS_EX_LAYERED always; WS_EX_TRANSPARENT only
        // when NOT calibrating (the bypass in refreshInplaceStyles - points
        // must be draggable during calibration even with the overlay on).
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                    (inplace ? WS_EX_LAYERED : 0) |
                                    ((inplace && !g_calib.load()) ? WS_EX_TRANSPARENT : 0),
            OUT_CLASS,
            title.c_str(), WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (preview || inplace) {
            // 0x11 = WDA_EXCLUDEFROMCAPTURE (Win10 2004+), 0x01 = WDA_MONITOR
            typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
            auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
            BOOL ok = fn ? fn(hwnd, 0x11) : FALSE;
            if (!ok && fn) ok = fn(hwnd, 0x01);
            g_previewNoCapture.store(!ok);
            dbgLogLine(std::string(preview ? "preview" : "inplace") + std::string(": window ") +
                       (ok ? "excluded" : "NOT excluded") +
                       " from capture, rect=" + std::to_string(r.left) + "," + std::to_string(r.top) +
                       " " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top));
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        g_outWins.push_back({ hwnd, st });
        Engine::inst().attachOutput((int)i, hwnd);
    }
    // MAG test pattern overlays (g_cfgMutex already held; no lock inside)
    if (g_cfg.engine == 1 && g_cfg.testPattern != 0)
        syncMagTestOverlays(true);
    // MAG black-mask overlays: below the blend overlays (created first),
    // repaint on their own 250 ms timer after the lock is released
    if (g_cfg.engine == 1)
        syncMagMaskOverlays();
    // MAG probe-marker overlays (camera alignment): raise themselves above
    // the blend bars on first paint
    if (g_cfg.engine == 1)
        syncMagProbeOverlays(g_probe.load());
    // MAG edge-blend overlays: marshalled so they paint after the lock
    requestMagBlendSync();
    g_outCount.store((int)g_outWins.size());
    // console always stays on top of the output windows (like the original)
    notifyOutputRaised();
}

void destroyOutputWindows() {
    dbgLogLine("destroyOutputWindows: " + std::to_string(g_outWins.size()) + " window(s)");
    magDetachAll();   // no-op outside engine=1
    // test-pattern overlays are owned by the mag hosts: destroy them first
    for (HWND t : g_magTests) magRemoveFilter(t);
    for (HWND t : g_magTests) DestroyWindow(t);
    g_magTests.clear();
    for (HWND t : g_magBlends) magRemoveFilter(t);
    for (HWND t : g_magBlends) DestroyWindow(t);
    g_magBlends.clear();
    for (HWND t : g_magMasks) magRemoveFilter(t);
    for (HWND t : g_magMasks) DestroyWindow(t);   // WM_DESTROY kills the timer
    g_magMasks.clear();
    for (HWND t : g_magProbes) magRemoveFilter(t);
    for (HWND t : g_magProbes) DestroyWindow(t);
    g_magProbes.clear();
    for (auto& p : g_outWins) DestroyWindow(p.first);
    g_outWins.clear();
    g_outCount.store(0);
    Engine::inst().detachAll();
}

// UI thread: one GDI overlay per MAG host when `on`. Callers decide the
// state BEFORE taking g_cfgMutex (createOutputWindows calls this while the
// mutex is already held). The overlays sit in the mag filter list (and
// WDA_EXCLUDEFROMCAPTURE) so they neither recurse the feed nor pollute a
// capture of the output.
static void syncMagTestOverlays(bool on) {
    if (!on) {
        for (HWND t : g_magTests) magRemoveFilter(t);
        for (HWND t : g_magTests) DestroyWindow(t);
        g_magTests.clear();
        return;
    }
    WNDCLASSW tc{};
    tc.lpfnWndProc = MagTestWndProc;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.lpszClassName = MAGTEST_CLASS;
    RegisterClassW(&tc);
    for (auto& p : g_outWins) {
        // keep 1 overlay per host (idempotent)
        bool have = false;
        for (HWND t : g_magTests) if (GetWindow(t, GW_OWNER) == p.first) { have = true; break; }
        if (have) continue;
        RECT r; GetWindowRect(p.first, &r);
        HWND ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            MAGTEST_CLASS, L"TEST PATTERN", WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            p.first, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ov) continue;
        // slot the test pattern below the mask/blend overlays (shader order:
        // pattern, then mask zeroes it, then blend weights multiply) WITHOUT
        // HWND_BOTTOM - leaving the topmost band would clear WS_EX_TOPMOST.
        // If neither overlay exists yet, later creation lands above anyway.
        HWND above = g_magMasks.empty() ?
            (g_magBlends.empty() ? nullptr : g_magBlends.back()) :
            g_magMasks.back();
        if (above)
            SetWindowPos(ov, above, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
        auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                           "SetWindowDisplayAffinity");
        if (fn) fn(ov, 0x11);
        g_magTests.push_back(ov);
        magAddFilter(ov);
    }
    dbgLogLine("syncMagTestOverlays: " + std::to_string(g_magTests.size()) + " overlay(s)");
}

// ------------------------------------------------------------------
// MAG mask overlays: per-host layered windows painting the OutputCfg black
// rectangles (same semantics as the DXGI shader: region forced to black,
// premultiplied alpha). The overlay lives as long as its host and repaints
// from a 250 ms timer only when the mask list actually changed - the change
// entry points (SET mask.*, config load) are numerous, so a poll beats
// instrumenting each one. Created BEFORE the blend overlays so blending
// stays on top (shader order: mask zeroes the texel, blend weights multiply).
static const wchar_t* MAGMASK_CLASS = L"IWN_MagMaskWnd";

static bool paintMagMask(HWND ov, HWND host);   // fwd: WM_TIMER handler calls it

static LRESULT CALLBACK MagMaskWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_TIMER && wp == 1) {
        paintMagMask(hwnd, GetWindow(hwnd, GW_OWNER));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// paint the host's cfg.masks as opaque black over a fully transparent layer.
// Returns true when something was repainted; the FNV-1a hash of the rect
// list is cached in the overlay's GWLP_USERDATA to skip identical frames.
bool paintMagMask(HWND ov, HWND host) {
    if (!ov || !host) return false;
    OutWinState* st = (OutWinState*)GetWindowLongPtrW(host, GWLP_USERDATA);
    if (!st) return false;
    auto displays = enumerateDisplays();
    if (st->displayIdx < 0 || st->displayIdx >= (int)displays.size()) return false;
    std::vector<MaskRect> masks;
    unsigned hash = 2166136261u;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == displays[st->displayIdx].deviceName) {
                masks = o.masks;
                break;
            }
    }
    // blackout participates in the change hash so toggling always repaints
    bool bo = g_blackout.load();
    hash = (hash ^ (bo ? 0x9E3779B9u : 0u)) * 16777619u;
    for (auto& m : masks) {
        auto mix = [&hash](float f) {
            unsigned v;
            memcpy(&v, &f, 4);
            hash = (hash ^ v) * 16777619u;
        };
        mix(m.x); mix(m.y); mix(m.w); mix(m.h);
    }
    if ((unsigned)GetWindowLongPtrW(ov, GWLP_USERDATA) == hash) return false;
    RECT wr{};
    GetWindowRect(host, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w; bi.bV5Height = -h; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto* px = (unsigned*)bits;
        if (bo) {
            // show blackout: the whole overlay is opaque black (covers test
            // pattern; the blend bars above just multiply more black onto it)
            unsigned black = 0xFF000000;
            for (int i = 0; i < w * h; i++) px[i] = black;
        } else for (auto& m : masks) {
            // premultiplied opaque black = alpha 255, color 0
            unsigned black = 0xFF000000;
            int x0 = (int)(m.x * w), y0 = (int)(m.y * h);
            int x1 = (int)((m.x + m.w) * w), y1 = (int)((m.y + m.h) * h);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            for (int y = y0; y < y1; y++) {
                unsigned* row = px + (size_t)y * w;
                for (int x = x0; x < x1; x++) row[x] = black;
            }
        }
        SelectObject(mdc, bmp);
        POINT src{ 0, 0 };
        SIZE sz{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(ov, sdc, nullptr, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        SetWindowLongPtrW(ov, GWLP_USERDATA, (LONG_PTR)hash);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return true;
}

// UI thread: one always-on mask overlay per MAG host (idempotent). Callers
// decide the engine before calling; safe under g_cfgMutex (the paint itself
// happens later on the timer, after the lock is released).
static void syncMagMaskOverlays() {
    WNDCLASSW tc{};
    tc.lpfnWndProc = MagMaskWndProc;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.lpszClassName = MAGMASK_CLASS;
    RegisterClassW(&tc);
    for (auto& p : g_outWins) {
        bool have = false;
        for (HWND t : g_magMasks) if (GetWindow(t, GW_OWNER) == p.first) { have = true; break; }
        if (have) continue;
        RECT r; GetWindowRect(p.first, &r);
        HWND ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            MAGMASK_CLASS, L"MASK", WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            p.first, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ov) continue;
        SetWindowLongPtrW(ov, GWLP_USERDATA, 0);   // hash cache: forces first paint
        SetTimer(ov, 1, 250, nullptr);
        typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
        auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                           "SetWindowDisplayAffinity");
        if (fn) fn(ov, 0x11);
        g_magMasks.push_back(ov);
        magAddFilter(ov);
    }
    dbgLogLine("syncMagMaskOverlays: " + std::to_string(g_magMasks.size()) + " overlay(s)");
}

// ------------------------------------------------------------------
// MAG probe overlays: white crosses at a uniform rows x cols grid, one
// layered window per MAG host. Camera-alignment parity with the DXGI
// engine's probe pass (white crosses at the warp destination points); MAG
// has no warp mesh, so the markers sit on an even grid across the mirror.
// Create/destroy driven by explicit resync hooks (PROBE / SET probe= /
// LOAD / host rebuild) - few entry points, so no polling timer is needed.
static const wchar_t* MAGPROBE_CLASS = L"IWN_MagProbeWnd";

static bool paintMagProbe(HWND ov, HWND host);   // fwd: posted self-message

static LRESULT CALLBACK MagProbeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_USER + 1) {              // deferred first paint (lock-free)
        paintMagProbe(hwnd, GetWindow(hwnd, GW_OWNER));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// paint the probe grid: opaque white crosses (12 px arms, 2 px stroke, 4 px
// center dot) inset so border markers stay fully visible. Probe off paints
// a fully transparent layer (the window stays but shows nothing).
static bool paintMagProbe(HWND ov, HWND host) {
    if (!ov || !host) return false;
    OutWinState* st = (OutWinState*)GetWindowLongPtrW(host, GWLP_USERDATA);
    if (!st) return false;
    auto displays = enumerateDisplays();
    if (st->displayIdx < 0 || st->displayIdx >= (int)displays.size()) return false;
    bool on = g_probe.load() && !g_blackout.load();   // blackout hides markers
    int rows = 10, cols = 10;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == displays[st->displayIdx].deviceName) {
                rows = o.rows; cols = o.cols;
                break;
            }
    }
    RECT wr{};
    GetWindowRect(host, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w; bi.bV5Height = -h; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto* px = (unsigned*)bits;      // all-zero = fully transparent
        if (on) {
            const int arm = 12, inset = arm + 4;
            auto put = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= w || y >= h) return;
                px[(size_t)y * w + x] = 0xFFFFFFFF;   // opaque white
            };
            for (int r = 0; r < rows; r++) {
                int cy = rows > 1 ? inset + (int)((long long)(h - 2 * inset) * r / (rows - 1))
                                  : h / 2;
                for (int c = 0; c < cols; c++) {
                    int cx = cols > 1 ? inset + (int)((long long)(w - 2 * inset) * c / (cols - 1))
                                      : w / 2;
                    for (int d = -arm; d <= arm; d++) {
                        put(cx + d, cy); put(cx + d, cy + 1);      // horizontal
                        put(cx, cy + d); put(cx + 1, cy + d);      // vertical
                    }
                    for (int dy = -2; dy <= 1; dy++)
                        for (int dx = -2; dx <= 1; dx++) put(cx + dx, cy + dy);
                }
            }
        }
        SelectObject(mdc, bmp);
        POINT src{ 0, 0 };
        SIZE sz{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(ov, sdc, nullptr, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        if (on)
            // the blend overlays are created asynchronously after us and on
            // every edges/fade change - keep the markers readable above them
            SetWindowPos(ov, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return true;
}

// UI thread: one probe overlay per MAG host when `on` (create/destroy
// semantics, idempotent). Callers decide engine + state; safe under
// g_cfgMutex - the paint is deferred via a posted self-message.
static void syncMagProbeOverlays(bool on) {
    for (HWND t : g_magProbes) magRemoveFilter(t);
    for (HWND t : g_magProbes) DestroyWindow(t);
    g_magProbes.clear();
    if (!on) {
        dbgLogLine("syncMagProbeOverlays: off");
        return;
    }
    WNDCLASSW tc{};
    tc.lpfnWndProc = MagProbeWndProc;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.lpszClassName = MAGPROBE_CLASS;
    RegisterClassW(&tc);
    for (auto& p : g_outWins) {
        RECT r; GetWindowRect(p.first, &r);
        HWND ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            MAGPROBE_CLASS, L"PROBE", WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            p.first, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ov) continue;
        PostMessageW(ov, WM_USER + 1, 0, 0);   // paint after the lock
        typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
        auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                           "SetWindowDisplayAffinity");
        if (fn) fn(ov, 0x11);
        g_magProbes.push_back(ov);
        magAddFilter(ov);
    }
    dbgLogLine("syncMagProbeOverlays: " + std::to_string(g_magProbes.size()) + " overlay(s)");
}

// ------------------------------------------------------------------
// MAG blend overlays: per-host layered windows painting the edge-fade bars
// with per-pixel alpha. The MAG engine has no mesh renderer, so the blend
// rides on top of the feed - same edge mask / fade semantics as the DXGI
// shader (edges bits L/R/T/B = 1/2/4/8, fade = fraction of the dimension).
static const wchar_t* MAGBLEND_CLASS = L"IWN_MagBlendWnd";

static LRESULT CALLBACK MagBlendWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);   // painted at creation/sync
}

// per-output blend parameters snapshot (by OutWinState), under lock. Side
// order L/R/T/B = 0..3, matching BlendSide. Advanced points follow the
// shader's gating: only when advancedBlend is on AND the side's edge bit is
// set, capped at MAX_ADV entries.
struct MagBlendParams {
    int edges = 0;
    float fade[4] = {}, gam[4] = {}, gn[4] = {}, sl[4] = {};
    AdvPoint adv[4][MAX_ADV];
    int advN[4] = {};
};

static bool magBlendInfo(OutWinState* st, MagBlendParams& p) {
    if (!st) return false;
    auto displays = enumerateDisplays();
    if (st->displayIdx < 0 || st->displayIdx >= (int)displays.size()) return false;
    p = MagBlendParams{};
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    for (auto& o : g_cfg.outputs)
        if (o.enabled && o.deviceName == displays[st->displayIdx].deviceName) {
            p.edges = o.edges;
            bool advOn = g_cfg.advancedBlend;
            for (int s = 0; s < 4; s++) {
                p.fade[s] = o.blend[s].fade;
                p.gam[s] = o.blend[s].gamma;
                p.gn[s] = o.blend[s].gain;
                p.sl[s] = o.blend[s].slope;
                if (advOn && (o.edges & (1 << s))) {
                    int n = (int)(std::min)(o.adv[s].size(), (size_t)MAX_ADV);
                    for (int i = 0; i < n; i++) p.adv[s][i] = o.adv[s][i];
                    p.advN[s] = n;
                }
            }
            return true;
        }
    return false;
}

// CPU mirror of the shader's parametric blend curve (Engine.cpp):
// f(x) = (gain+1)x / (slope*(x-1) + gain*x + 1); defaults give f(x)=x.
static float magBlendCurve(float x, float gain, float slope) {
    float d = slope * (x - 1.0f) + gain * x + 1.0f;
    if (d < 1e-4f) d = 1e-4f;
    return (gain + 1.0f) * x / d;
}

// CPU mirror of the shader's edgeParams(): piecewise-linear fade/gamma along
// the edge coordinate c over the advanced points (pos, fade, gamma).
static void magEdgeParams(const AdvPoint* pts, int n, float c,
                          float baseF, float baseG, float& fw, float& g) {
    fw = baseF; g = baseG;
    if (n <= 0) return;
    if (c <= pts[0].pos) { fw = pts[0].fade; g = pts[0].gamma; return; }
    for (int i = 0; i < n - 1; i++) {
        if (c <= pts[i + 1].pos) {
            float d = pts[i + 1].pos - pts[i].pos;
            float t = (c - pts[i].pos) / (d > 1e-5f ? d : 1e-5f);
            fw = pts[i].fade + (pts[i + 1].fade - pts[i].fade) * t;
            g = pts[i].gamma + (pts[i + 1].gamma - pts[i].gamma) * t;
            return;
        }
    }
    fw = pts[n - 1].fade; g = pts[n - 1].gamma;
}

static void paintMagBlend(HWND ov, HWND host) {
    OutWinState* st = (OutWinState*)GetWindowLongPtrW(host, GWLP_USERDATA);
    MagBlendParams p;
    magBlendInfo(st, p);
    int edges = p.edges;
    RECT wr{};
    GetWindowRect(host, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return;
    if (!edges) { UpdateLayeredWindow(ov, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0); return; }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w; bi.bV5Height = -h; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto* px = (unsigned*)bits;
        for (int y = 0; y < h; y++) {
            unsigned* row = px + (size_t)y * w;
            float ny = (y + 0.5f) / h;
            for (int x = 0; x < w; x++) {
                // transmitted weight per pixel = product of the participating
                // sides' pow(blendCurve(t), gamma) - exactly the shader's
                // multiplication (normalized pixel-center coords); the
                // overlay paints black with alpha 1 - weight so
                // content * (1 - alpha) == content * weight
                float nx = (x + 0.5f) / w;
                float wt = 1.0f;
                if (edges & 1) {
                    float fw, g;
                    magEdgeParams(p.adv[0], p.advN[0], ny, p.fade[0], p.gam[0], fw, g);
                    if (fw > 0.001f && nx < fw)
                        wt *= powf(magBlendCurve(nx / fw, p.gn[0], p.sl[0]), g);
                }
                if (edges & 2) {
                    float fw, g;
                    magEdgeParams(p.adv[1], p.advN[1], ny, p.fade[1], p.gam[1], fw, g);
                    if (fw > 0.001f && nx > 1 - fw)
                        wt *= powf(magBlendCurve((1 - nx) / fw, p.gn[1], p.sl[1]), g);
                }
                if (edges & 4) {
                    float fw, g;
                    magEdgeParams(p.adv[2], p.advN[2], nx, p.fade[2], p.gam[2], fw, g);
                    if (fw > 0.001f && ny < fw)
                        wt *= powf(magBlendCurve(ny / fw, p.gn[2], p.sl[2]), g);
                }
                if (edges & 8) {
                    float fw, g;
                    magEdgeParams(p.adv[3], p.advN[3], nx, p.fade[3], p.gam[3], fw, g);
                    if (fw > 0.001f && ny > 1 - fw)
                        wt *= powf(magBlendCurve((1 - ny) / fw, p.gn[3], p.sl[3]), g);
                }
                float a = 1.0f - wt;
                if (a < 0) a = 0;
                if (a > 1) a = 1;
                row[x] = (unsigned)(a * 255.0f) << 24;   // black, premultiplied
            }
        }
        SelectObject(mdc, bmp);
        POINT src{ 0, 0 };
        SIZE sz{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(ov, sdc, nullptr, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}

// UI thread only (via WM_MAGBLEND): create/remove overlays per host and
// repaint them with the current edges/fade values.
static void syncMagBlendOverlays() {
    bool want;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        want = g_cfg.engine == 1;
    }
    if (!want) {
        for (HWND t : g_magBlends) magRemoveFilter(t);
        for (HWND t : g_magBlends) DestroyWindow(t);
        g_magBlends.clear();
        return;
    }
    WNDCLASSW bc{};
    bc.lpfnWndProc = MagBlendWndProc;
    bc.hInstance = GetModuleHandleW(nullptr);
    bc.lpszClassName = MAGBLEND_CLASS;
    RegisterClassW(&bc);
    for (auto& p : g_outWins) {
        int mine = -1;
        for (size_t i = 0; i < g_magBlends.size(); i++)
            if (GetWindow(g_magBlends[i], GW_OWNER) == p.first) { mine = (int)i; break; }
        // no fade content for this output (no participating side carries a
        // band - base fade or advanced points - ): no overlay
        MagBlendParams bp;
        bool info = magBlendInfo((OutWinState*)GetWindowLongPtrW(p.first, GWLP_USERDATA), bp);
        bool content = info;
        if (content) {
            content = false;
            for (int s = 0; s < 4 && !content; s++) {
                if (!(bp.edges & (1 << s))) continue;
                if (bp.fade[s] > 0) { content = true; break; }
                for (int i = 0; i < bp.advN[s]; i++)
                    if (bp.adv[s][i].fade > 0) { content = true; break; }
            }
        }
        if (!content) {
            if (mine >= 0) {
                magRemoveFilter(g_magBlends[mine]);
                DestroyWindow(g_magBlends[mine]);
                g_magBlends.erase(g_magBlends.begin() + mine);
            }
            continue;
        }
        if (mine < 0) {
            RECT r{};
            GetWindowRect(p.first, &r);
            HWND ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                MAGBLEND_CLASS, L"", WS_POPUP,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                p.first, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!ov) continue;
            g_magBlends.push_back(ov);
            magAddFilter(ov);
            typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
            auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetWindowDisplayAffinity");
            if (fn) fn(ov, 0x11);
            mine = (int)g_magBlends.size() - 1;
        }
        paintMagBlend(g_magBlends[mine], p.first);
    }
    // drop overlays whose host is gone
    for (size_t i = 0; i < g_magBlends.size();) {
        bool alive = false;
        for (auto& p : g_outWins)
            if (GetWindow(g_magBlends[i], GW_OWNER) == p.first) { alive = true; break; }
        if (alive) { i++; continue; }
        magRemoveFilter(g_magBlends[i]);
        DestroyWindow(g_magBlends[i]);
        g_magBlends.erase(g_magBlends.begin() + i);
    }
    // blend bars just (re)landed above everything - raise the probe markers
    // back so camera alignment stays readable over the fade bands
    for (HWND t : g_magProbes)
        SetWindowPos(t, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ------------------------------------------------------------------
// Control window
// ------------------------------------------------------------------
static const wchar_t* CTL_CLASS = L"IWN_ControlWnd";
static HWND g_ctl = nullptr;

void requestMagBlendSync() {
    // any thread: the sync itself touches windows, so always marshal to the
    // UI thread even when already on it (PostMessage defers past the caller's
    // g_cfgMutex section)
    if (g_ctl) PostMessageW(g_ctl, WM_MAGBLEND, 0, 0);
}

static HWND g_list = nullptr, g_status = nullptr, g_statusInfo = nullptr;
static HWND g_ckEnable = nullptr, g_ckTest = nullptr, g_ckAdv = nullptr;
static HWND g_ckPreview = nullptr, g_ckImage = nullptr, g_imgLabel = nullptr;
static HWND g_ckInplace = nullptr;  // in-place overlay (fullscreen click-through)
static HWND g_ckNet = nullptr;
static HWND g_ckAuto = nullptr, g_cbSamp = nullptr;
static HWND g_cbLayout = nullptr, g_cbOvl = nullptr, g_cbEng = nullptr;
static HWND g_cbSrc = nullptr;
static HWND g_cbPreset = nullptr;   // named presets (shared with CMDNET)
static HWND g_cbTarget = nullptr;   // per-output command target (mirrors SET out=)
// capture source combo: index 0 = auto, index i>0 = g_srcDevs[i]
static std::vector<std::wstring> g_srcDevs;
// target combo: index 0 = auto, index i>0 = g_tgtShort[i] ("DISPLAY2")
static std::vector<std::string> g_tgtShort;
static bool g_tgtBusy = false;      // suppress CBN_SELCHANGE during programmatic fill

// Main-console tab strip: controls are tagged to a page at creation time and
// shown/hidden together (1:1 with the original's multi-page console).
// menu bar ids (File/View/Warping/Help). Must stay outside 1001..1034 and the
// tray ranges (2001..2004, 2200..2231). Defined BEFORE showTab (which marks
// the View items) - the C2065 recurrence guard.
#define IDM_FILE_LOAD  2101
#define IDM_FILE_SAVE  2102
#define IDM_FILE_EXIT  2103
#define IDM_VIEW_DISP  2111   // +0..3 = the four pages
#define IDM_VIEW_INPLACE 2115 // in-place overlay toggle (checkable)
#define IDM_VIEW_HIDE  2116
#define IDM_VIEW_BLACKOUT 2117   // show blackout: all outputs full black (checkable)
#define IDM_WARP_ALIGN 2121
#define IDM_WARP_EDGES 2122
#define IDM_WARP_SNAP  2123
#define IDM_WARP_TEST  2124
#define IDM_HELP_ABOUT 2131
static std::vector<HWND> g_pages[4];
static HWND g_tab = nullptr;
static HWND g_schematic = nullptr;   // Displays page output schematic (owner-draw)
enum { PAGE_DISPLAYS = 0, PAGE_WARP, PAGE_IMAGE, PAGE_SETTINGS, PAGE_COUNT };
static void showTab(int pg) {
    if (pg < 0 || pg >= PAGE_COUNT) pg = 0;
    for (int i = 0; i < PAGE_COUNT; i++)
        for (HWND h : g_pages[i])
            ShowWindow(h, i == pg ? SW_SHOW : SW_HIDE);
    // keep the View menu radio marks in step with the tab strip
    if (g_ctl) {
        if (HMENU m = GetMenu(g_ctl)) {
            for (int i = 0; i < PAGE_COUNT; i++)
                CheckMenuItem(m, IDM_VIEW_DISP + i, MF_BYCOMMAND |
                              (i == pg ? MF_CHECKED : MF_UNCHECKED));
        }
    }
}

// tray ids must not collide with control ids 1001..1021
#define IDM_TRAY_SHOW  2001
#define IDM_TRAY_START 2002
#define IDM_TRAY_STOP  2003
#define IDM_TRAY_INPLACE 2005  // in-place overlay toggle (checkable)
#define IDM_TRAY_EXIT  2004
#define IDM_TRAY_PRESET 2200   // +0..31: dynamic preset submenu (not used by dialog ids)
#define TRAY_PRESET_MAX 32

// cross-thread capture-source change (CMDNET runs on the socket thread).
// constexpr so it can be used as a switch case label.
constexpr UINT WM_SRCSET = WM_APP + 9;
constexpr UINT WM_LAYSET = WM_APP + 10;
constexpr UINT WM_INPLACESET = WM_APP + 11;   // in-place overlay toggled off-thread
constexpr UINT WM_MAGTEST = WM_APP + 14;      // test pattern changed off-thread
constexpr UINT WM_TGTREFRESH = WM_APP + 15;   // refresh the Target combo (off-thread)
constexpr UINT WM_DISPCHANGE = WM_APP + 16;   // deferred output rebuild after
constexpr UINT WM_MAGPROBE = WM_APP + 17;     // probe markers toggled off-thread
constexpr UINT WM_BLACKOUT = WM_APP + 18;     // blackout toggled off-thread
                                              // WM_DISPLAYCHANGE (the handler
                                              // must not do heavy work inline)
static DWORD uiThreadId = 0;
constexpr UINT WM_TRAYICON = WM_APP + 7;
static std::mutex g_layMsgMutex;
struct LayMsg { int r = 0, c = 0, ov = 0; };
static LayMsg g_layMsg;

// ---------------- tray icon ----------------
static NOTIFYICONDATAW g_nid{};
static bool g_trayOn = false;

// nth preset name (alphabetical order as returned by FindFirstFile); shared by
// the tray submenu build and its click handler so indices stay consistent
static bool presetNameByIndex(int idx, std::wstring& out) {
    std::wstring file = presetPath("x");
    std::wstring dir = file.substr(0, file.find_last_of(L'\\') + 1);
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cfg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring nm = fd.cFileName;
            if (nm.size() > 4 && nm.rfind(L".cfg") == nm.size() - 4) nm = nm.substr(0, nm.size() - 4);
            if (n++ == idx) { out = nm; FindClose(h); return true; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return false;
}

static void trayInit(HWND hwnd) {
    if (g_trayOn) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, APP_NAME, 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayOn = true;
}

static void trayRemove() {
    if (!g_trayOn) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayOn = false;
}

static void showTrayMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    bool visible = IsWindowVisible(hwnd);
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, visible ? L"Hide console" : L"Show console");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_START, L"Start Output");
    AppendMenuW(m, MF_STRING, IDM_TRAY_STOP, L"Stop Output");
    AppendMenuW(m, MF_STRING | (g_inplace.load() ? MF_CHECKED : 0),
                IDM_TRAY_INPLACE, L"In-place overlay (Ctrl+F9)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // named presets: one-click scene switching (same files as CMDNET SAVEAS/LOADAS)
    {
        HMENU pm = CreatePopupMenu();
        int n = 0;
        std::wstring nm;
        while (n < TRAY_PRESET_MAX && presetNameByIndex(n, nm)) {
            AppendMenuW(pm, MF_STRING, IDM_TRAY_PRESET + n, nm.c_str());
            n++;
        }
        if (!n) AppendMenuW(pm, MF_STRING | MF_GRAYED, 0, L"(no presets)");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)pm, L"Load preset");
    }
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);   // required so the menu dismisses on outside click
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

// ---------------- autostart (HKCU Run, no admin needed) ----------------
void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring v = L"\"" + std::wstring(path) + L"\"";
            RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, APP_NAME);
        }
        RegCloseKey(k);
    }
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.autostart = on ? 1 : 0;
}

bool queryAutostart() {
    HKEY k;
    bool on = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}
static HWND g_cbImg = nullptr;

static void setStatus(const wchar_t* s) {
    if (g_status) SetWindowTextW(g_status, s);
}

// Structured status block, same information the original shows at the bottom
// of its console: "Projector: 1 [1]" / mode / "Color balancing: R() G() B()".
// Never call while holding g_cfgMutex.
void refreshStatusBar() {
    if (!g_statusInfo) return;
    int nProj = 0, sel = g_list ? ListBox_GetCurSel(g_list) : -1;
    ColorAdj c;
    int edges = 15;
    std::wstring srcDev;
    bool snapshot = g_snapshot.load();
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) {
            if (!o.enabled) continue;
            if (nProj == 0) { c = o.color; edges = o.edges; }
            nProj++;
        }
        srcDev = g_cfg.sourceDev;
    }
    wchar_t l1[192], l2[160], l3[192], l4[128], l5[160];
    // [n] = which projector is being edited (1-based). refreshList() resets the
    // listbox selection, so fall back to 1 instead of showing [0].
    int cur = (sel >= 0 && sel < nProj) ? sel + 1 : (nProj > 0 ? 1 : 0);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        wsprintfW(l1, L"Projector: %d [%d]%s%s%s%s%s", nProj, cur,
                  g_previewMode.load() ? L"   Preview: ON" : L"",
                  g_inplace.load() ? (g_calib.load() ? L"   In-place: CALIB (click-through bypassed)"
                                                     : L"   In-place: ON") : L"",
                  g_cfg.engine == 1 ? L"   Engine: MAG" : L"",
                  g_outCount.load() > 0 ? L"   Warping: ACTIVE" : L"   Warping: idle",
                  g_blackout.load() ? L"   Blackout: ON" : L"");
    }
    // wsprintfW (user32) has no floating point support -> use the CRT here
    _snwprintf_s(l2, 160, _TRUNCATE, L"Mode: %s   Snapshot: %s",
                 nProj == 0 ? L"- (no projector assigned)"
                            : (g_calibMode.load() == 1 ? L"Edge blending" : L"Geometrical correction"),
                 snapshot ? L"ON (frozen)" : L"off");
    _snwprintf_s(l3, 192, _TRUNCATE, L"Color balancing: R(%.3f) G(%.3f) B(%.3f)",
                 c.gammaR, c.gammaG, c.gammaB);
    // human readable edge mask for the first enabled projector
    static const wchar_t* side[4] = { L"L", L"R", L"T", L"B" };
    wchar_t buf[32] = L"";
    if (edges == 15) lstrcpyW(buf, L"ALL");
    else if (edges == 0) lstrcpyW(buf, L"NONE");
    else for (int i = 0; i < 4; i++) if (edges & (1 << i)) { lstrcatW(buf, side[i]); }
    wsprintfW(l4, L"Blend edges: %s", buf);
    if (srcDev.empty())
        lstrcpyW(l5, L"Capture source: auto (first non-output display)");
    else
        wsprintfW(l5, L"Capture source: %s (pinned)", srcDev.c_str());

    // live render health (same numbers the CMDNET GET perf reports)
    wchar_t l6[128];
    {
        unsigned long long up = engineUptimeMs() / 1000;
        _snwprintf_s(l6, 128, _TRUNCATE,
                     L"Engine: %.1f fps   frame %.2f ms (cap %.2f + draw %.2f)   uptime %02u:%02u:%02u",
                     engineFps(), engineFrameMs(), engineCapMs(), engineDrawMs(),
                     (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60));
    }

    wchar_t all[960];
    wsprintfW(all, L"%s\r\n%s\r\n%s\r\n%s\r\n%s\r\n%s", l1, l2, l3, l4, l5, l6);
    SetWindowTextW(g_statusInfo, all);
}

// Click-through bypass: while calibration is active, the in-place overlay
// windows drop WS_EX_TRANSPARENT so the user can drag warp/blend handles
// directly on the overlay (1x1 main-screen correction without toggling the
// overlay off first). Leaving calibration restores the pass-through.
static void refreshInplaceStyles() {
    bool transparent = g_inplace.load() && !g_calib.load();
    for (auto& w : g_outWins) {
        LONG_PTR ex = GetWindowLongPtrW(w.first, GWL_EXSTYLE);
        LONG_PTR want = transparent ? (ex | WS_EX_TRANSPARENT)
                                    : (ex & ~WS_EX_TRANSPARENT);
        if (want != ex) SetWindowLongPtrW(w.first, GWL_EXSTYLE, want);
    }
}

// Hotkey registration must happen on the thread that pumps messages (the UI
// thread): hotkeys registered elsewhere post WM_HOTKEY into a queue nobody
// reads. Called from setCalibrationMode (UI thread) and the WM_CALIBSET
// handler (CMDNET path). Idempotent: duplicate register/unregister attempts
// simply fail and are ignored.
void calibrationHotkeysSync(bool on) {
    if (on) {
        RegisterHotKey(nullptr, 3, 0, VK_ESCAPE);
        // D opens the advanced RGB dialog, exactly like the original. Only
        // registered during calibration so normal typing never hits it.
        RegisterHotKey(nullptr, 6, 0, 'D');
    } else {
        UnregisterHotKey(nullptr, 3);
        UnregisterHotKey(nullptr, 6);
    }
}

void requestCalibrationSync() {
    // socket thread: the state atom is already updated, the UI thread only
    // owns the hotkey (un)registration + the follow-up UI refresh
    if (g_ctl) PostMessageW(g_ctl, WM_CALIBSET, 0, 0);
}

void setCalibrationMode(bool on) {
    bool was = g_calib.exchange(on);
    if (was == on) return;
    calibrationHotkeysSync(on);
    refreshInplaceStyles();   // drop / restore click-through (no-op if off)
    refreshStatusBar();
}

void toggleCalibration() {
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (g_cfg.engine == 1 && !g_calib.load()) {
            // MAG mode has no mesh: warp/blend calibration is meaningless
            setStatus(L"Calibration needs the Desktop Duplication engine (pick it in the engine dropdown).");
            return;
        }
    }
    if (g_calib.load()) {
        setCalibrationMode(false);
        setStatus(L"Calibration OFF.");
    } else {
        if (g_outWins.empty()) createOutputWindows();
        setCalibrationMode(true);
        setStatus(g_inplace.load()
            ? L"Calibration ON: click-through bypassed, drag the points directly. Esc to exit."
            : L"Calibration ON: drag green points (warp), F10 = blend mode, Esc = exit.");
    }
}

void switchSubModeAll() {
    if (!g_calib.load()) { setStatus(L"Press F3 (or Configure alignment) first."); return; }
    int m = g_calibMode.fetch_xor(1) ^ 1;   // 0 warp <-> 1 blend
    for (auto& p : g_outWins) p.second->subMode = m;
    setStatus(m == 1 ? L"Mode: Edge blending (red = simple handles, blue = advanced). Wheel = gamma, Ctrl+wheel = gain, Shift+wheel = slope. F10 to switch back."
                     : L"Mode: Geometrical correction (drag green points). F10 to switch.");
    refreshStatusBar();
}

// F12: cycle which edges take part in blending for all projectors.
// all -> left -> right -> top -> bottom -> none -> all
void cycleBlendEdges() {
    static const int seq[6] = { 15, 1, 2, 4, 8, 0 };
    static const wchar_t* name[6] = {
        L"ALL (left+right+top+bottom)", L"LEFT only", L"RIGHT only",
        L"TOP only", L"BOTTOM only", L"NONE (blending off)" };
    int cur = 15, idx = 0;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) if (o.enabled) { cur = o.edges; break; }
    }
    for (int i = 0; i < 6; i++) if (seq[i] == cur) { idx = (i + 1) % 6; break; }
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) if (o.enabled) o.edges = seq[idx];
        g_rev++;
    }
    wchar_t msg[160];
    wsprintfW(msg, L"Blend edges: %s", name[idx]);
    setStatus(msg);
    refreshStatusBar();
}

// explicit edge set for CMDNET (complements the F12 cycle)
void setBlendEdgesMask(int mask) {
    mask &= 15;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) if (o.enabled) o.edges = mask;
        g_rev++;
    }
    wchar_t msg[96], e[16] = L"";
    if (mask & 1) lstrcatW(e, L"L");
    if (mask & 2) lstrcatW(e, e[0] ? L"+R" : L"R");
    if (mask & 4) lstrcatW(e, e[0] ? L"+T" : L"T");
    if (mask & 8) lstrcatW(e, e[0] ? L"+B" : L"B");
    if (mask == 15) wsprintfW(msg, L"Blend edges: ALL");
    else if (!mask) wsprintfW(msg, L"Blend edges: NONE");
    else wsprintfW(msg, L"Blend edges: %s", e);
    setStatus(msg);
    refreshStatusBar();
    requestMagBlendSync();   // engine=1: fade-bar overlays follow the mask
}

// Ctrl+F3: freeze the captured desktop so alignment is done on a still image.
void toggleSnapshot() {
    bool on = !g_snapshot.load();
    g_snapshot.store(on);
    if (on && g_outWins.empty()) { setStatus(L"Snapshot: no output running."); refreshStatusBar(); return; }
    setStatus(on ? L"Snapshot ON: captured frame frozen. Align on the still image, Ctrl+F3 to resume live."
                 : L"Snapshot OFF: live desktop capture resumed.");
    refreshStatusBar();
}

// In-place overlay safety switch, shared by the global Ctrl+F9 hotkey, the
// View menu item and the tray menu. Click-through output windows cannot be
// clicked themselves, so an always-reachable off switch matters (the console
// is topmost anyway, this is the extra "get me out" path).
void toggleInplaceOverlay() {
    bool on = !g_inplace.load();
    g_inplace.store(on);
    if (!on) g_previewNoCapture.store(false);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.inplace = on;
        g_rev++;
    }
    // rebuild via the same queue entry the checkbox uses (UI + socket thread)
    if (g_ctl) PostMessageW(g_ctl, WM_INPLACESET, 0, 0);
    setStatus(on ? L"In-place overlay ON: click-through enabled. Ctrl+F9 turns it off."
                 : L"In-place overlay off: normal output windows.");
    refreshStatusBar();
}

// F4, the original's window-warping switch: start/stop the outputs and flip
// the Enabled checkbox to match. UI thread only (dispatched from WM_HOTKEY).
void toggleWindowWarping() {
    bool on;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        on = !g_cfg.enabled;
        g_cfg.enabled = on;
        g_rev++;
    }
    if (on) createOutputWindows();
    else destroyOutputWindows();
    if (g_ckEnable)
        Button_SetCheck(g_ckEnable, on ? BST_CHECKED : BST_UNCHECKED);
    setStatus(on ? L"Output running (F4). Press F3 / Configure alignment to warp."
                 : L"Output stopped (F4).");
    refreshStatusBar();
}

// keep every in-place affordance in step: checkbox, View menu check mark
// (the tray menu is rebuilt on every right-click, so it reads g_inplace live)
static void invalidateToolbarToggles();     // fwd: syncInplaceUI calls it

static void syncInplaceUI(HWND hwnd) {
    bool on = g_inplace.load();
    if (g_ckInplace)
        Button_SetCheck(g_ckInplace, on ? BST_CHECKED : BST_UNCHECKED);
    if (!hwnd && g_ctl) hwnd = g_ctl;
    if (hwnd)
        if (HMENU m = GetMenu(hwnd))
            CheckMenuItem(m, IDM_VIEW_INPLACE, MF_BYCOMMAND |
                          (on ? MF_CHECKED : MF_UNCHECKED));
    invalidateToolbarToggles();   // In-place button paints its checked state
}

// toolbar toggle buttons (In-place / Test) paint from live state, so any
// path that flips the underlying value just invalidates the buttons and
// WM_DRAWITEM draws the truth - one helper keeps all entry points honest.
static void invalidateToolbarToggles() {
    if (!g_ctl) return;
    if (HWND b = GetDlgItem(g_ctl, IDM_VIEW_INPLACE)) InvalidateRect(b, nullptr, TRUE);
    if (HWND b = GetDlgItem(g_ctl, IDM_WARP_TEST)) InvalidateRect(b, nullptr, TRUE);
}

// UI thread only: make every MAG overlay reflect g_blackout right now -
// wake the mask overlays (their 250 ms poll would also catch up, but a show
// blackout must land immediately) and repaint the probe markers (they sit
// ABOVE the mask overlay and must not shine through a blackout).
static void syncBlackoutUI(HWND hwnd = nullptr) {
    if (!hwnd && g_ctl) hwnd = g_ctl;
    if (hwnd)
        if (HMENU m = GetMenu(hwnd))
            CheckMenuItem(m, IDM_VIEW_BLACKOUT, MF_BYCOMMAND |
                          (g_blackout.load() ? MF_CHECKED : MF_UNCHECKED));
    for (HWND t : g_magMasks) PostMessageW(t, WM_TIMER, 1, 0);
    bool engineMag;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        engineMag = g_cfg.engine == 1;
    }
    if (engineMag)
        syncMagProbeOverlays(g_probe.load() && !g_outWins.empty());
}

// socket thread safe: route the resync through the UI thread's message loop
void requestBlackoutSync() {
    if (g_ctl) PostMessageW(g_ctl, WM_BLACKOUT, 0, 0);
}

void notifyOutputRaised() {
    // keep the control console visible above the (topmost) output window
    if (g_ctl) SetWindowPos(g_ctl, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

#define IDC_LIST      1001
#define IDC_TOGGLE    1002
#define IDC_START     1003
#define IDC_STOP      1004
#define IDC_CALIB     1005
#define IDC_SAVE      1006
#define IDC_LOAD      1007
#define IDC_EXIT      1008
#define IDC_CKENABLE  1009
#define IDC_CKTEST    1010
#define IDC_CKADV     1011
#define IDC_CKPREVIEW 1012
#define IDC_CKIMAGE   1013
#define IDC_LOADIMG   1014
#define IDC_CLEARIMG  1015
#define IDC_CBIMG     1016   // built-in test image pattern dropdown
#define IDC_COLOR     1017   // color adjustment dialog
#define IDC_EDGES     1018   // cycle blended edges (same as Ctrl+F12)
#define IDC_CKNET     1019   // CMDNET TCP show-control service on/off
#define IDC_CKAUTO    1020   // start with Windows
#define IDC_CBSAMP    1021   // sampling quality dropdown
#define IDC_CBSRC     1022   // capture source display dropdown
#define IDC_CBLAYOUT  1023   // NxN tiled layout dropdown
#define IDC_CBOVL     1024   // tile overlap percent dropdown
#define IDC_CBENG     1025   // warp engine dropdown
#define IDC_CFGDLG    1026   // per-projector Configure display dialog
#define IDC_CBPRESET  1027   // named preset dropdown (CMDNET SAVEAS/LOADAS files)
#define IDC_PRESETSAVE 1028
#define IDC_PRESETLOAD 1029
#define IDC_PRESETDEL  1030
#define IDC_CBTARGET   1031   // per-output command target dropdown (SET out=)
#define IDC_TABS       1032   // main-console tab strip
#define IDC_CKINPLACE  1033   // in-place overlay (fullscreen click-through)
#define IDC_SCHEMATIC  1034   // Displays page: owner-drawn output schematic
#define IDC_STATUSINFO 1035   // structured status block (testable by id)
// (menu bar IDM_* ids live above showTab, see the note there)

static void refreshList() {
    if (!g_list) return;
    ListBox_ResetContent(g_list);
    auto displays = enumerateDisplays();
    {   // scoped: refreshStatusBar() locks the same mutex
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& d : displays) {
            wchar_t line[256];
            const wchar_t* mark = L"";
            for (auto& o : g_cfg.outputs)
                if (o.enabled && o.deviceName == d.deviceName) { mark = L"  [PROJECTOR]"; break; }
            if (d.primary) wsprintfW(line, L"%s [Primary]%s", d.label.c_str(), mark);
            else wsprintfW(line, L"%s%s", d.label.c_str(), mark);
            ListBox_AddString(g_list, line);
        }
    }
    if (g_schematic) InvalidateRect(g_schematic, nullptr, TRUE);
    refreshStatusBar();
}

// ---- capture source selection (which desktop gets warped) ----
static bool g_srcBusy = false;   // guards ComboBox_ResetContent reentrancy
static void populateSourceCombo() {
    if (!g_cbSrc) return;
    g_srcBusy = true;
    ComboBox_ResetContent(g_cbSrc);
    g_srcDevs.clear();
    g_srcDevs.push_back(L"");   // index 0 = auto
    ComboBox_AddString(g_cbSrc, L"Auto (first non-output display)");
    auto displays = enumerateDisplays();
    for (auto& d : displays) {
        wchar_t line[192];
        wsprintfW(line, L"%s  %ldx%ld%s", d.label.c_str(),
                  d.desktop.right - d.desktop.left, d.desktop.bottom - d.desktop.top,
                  d.primary ? L" [Primary]" : L"");
        ComboBox_AddString(g_cbSrc, line);
        g_srcDevs.push_back(d.deviceName);
    }
    int sel = 0;
    std::wstring want;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        want = g_cfg.sourceDev;
    }
    for (size_t i = 1; i < g_srcDevs.size(); i++)
        if (sameDisplayName(g_srcDevs[i], want)) { sel = (int)i; break; }
    ComboBox_SetCurSel(g_cbSrc, sel);
    g_srcBusy = false;
}

std::wstring currentSourceDevice() {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return g_cfg.sourceDev;
}

// cfg-only update, any thread (returns true when the value actually changed)
static bool applySourceValue(const std::wstring& dev) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    bool same = (g_cfg.sourceDev.empty() && dev.empty()) ||
                sameDisplayName(g_cfg.sourceDev, dev);
    if (same) return false;
    g_cfg.sourceDev = dev;
    g_rev++;
    return true;
}

// window work, UI thread only: the duplication is bound at attach time, so a
// source change has to rebuild the outputs
static void rebuildForSource() {
    bool running;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        running = g_cfg.enabled || g_cfg.previewMode;
    }
    if (running) {
        destroyOutputWindows();
        createOutputWindows();
    }
    if (g_cbSrc) populateSourceCombo();   // resync (normalises DISPLAY2 -> \\.\DISPLAY2)
    refreshTargetComboUI();
    refreshStatusBar();
}

static void applySourceDisplay(const std::wstring& dev) {   // UI thread
    if (applySourceValue(dev)) rebuildForSource();
    else refreshStatusBar();
}

void setSourceDisplay(const std::wstring& dev) {
    // CMDNET runs on the socket thread. Update the value there (so the reply is
    // accurate) and only marshal the window rebuild: a blocking SendMessage
    // here would deadlock against cmdnetStop()'s join on the UI thread.
    if (g_ctl && uiThreadId && GetCurrentThreadId() != uiThreadId) {
        if (applySourceValue(dev)) PostMessageW(g_ctl, WM_SRCSET, 0, 0);
        return;
    }
    applySourceDisplay(dev);
}

void requestInplaceRestyle() {
    // post from any thread; the console handles WM_INPLACESET on the UI thread
    if (g_ctl) PostMessageW(g_ctl, WM_INPLACESET, 0, 0);
}

void refreshEngineComboUI() {
    // socket thread safe: PostMessage-based, mirrors refreshTargetComboUI's
    // intent (UI follows shared state without re-triggering CBN_SELCHANGE -
    // CB_SETCURSEL alone never fires the notification)
    if (g_ctl && g_cbEng)
        PostMessageW(g_cbEng, CB_SETCURSEL,
                     g_cfg.engine == 1 ? 1 : 0, 0);
}

void refreshSampComboUI() {
    // socket thread safe: same pattern as refreshEngineComboUI (CB_SETCURSEL
    // is a plain message - no CBN_SELCHANGE, no cross-thread SendMessage)
    if (g_ctl && g_cbSamp) {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        PostMessageW(g_cbSamp, CB_SETCURSEL, g_cfg.sampling, 0);
    }
}

void requestOutputRebuild() {
    // engine switches (SET engine=) and any other state that changes WHAT the
    // output windows are: destroy + recreate on the UI thread. Reuses the
    // WM_INPLACESET entry point - it is already an idempotent rebuild that
    // also resyncs the overlay checkbox.
    if (g_ctl) PostMessageW(g_ctl, WM_INPLACESET, 0, 0);
}

void requestMagTestSync() {
    // socket thread: test pattern toggled -> GDI overlays follow on the UI
    // thread (engine=1 only; the DXGI engine renders it in the shader)
    if (g_ctl) PostMessageW(g_ctl, WM_MAGTEST, 0, 0);
}

void requestMagProbeSync() {
    // socket thread: probe markers toggled -> overlays follow on the UI
    // thread (engine=1 only; the DXGI engine draws them in its overlay pass)
    if (g_ctl) PostMessageW(g_ctl, WM_MAGPROBE, 0, 0);
}

// ---- NxN tiled layout -------------------------------------------------------
// Regenerates the warp grid of every enabled output so the wall shows one
// continuous source: output k (sorted by desktop position) sits at
// (row=k/R, col=k/C) and shows the source window
//   x in [col*(1-ov), col*(1-ov)+1], y likewise for rows,
// where ov = overlap/100. Points outside [0,1] fall off the projector and are
// cropped by the rasterizer. Interior boundaries get their blend edge enabled.
static void applyLayoutLocked() {   // g_cfgMutex held
    int R = g_cfg.layoutR, C = g_cfg.layoutC;
    float ov = g_cfg.overlap / 100.0f;
    auto displays = enumerateDisplays();
    // enabled outputs in physical order (top-to-bottom, left-to-right) so the
    // tile grid matches how the projectors hang on the wall
    struct Placed { RECT rc; OutputCfg* o; };
    std::vector<Placed> outs;
    for (auto& d : displays)
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == d.deviceName)
                outs.push_back({ d.desktop, &o });
    std::sort(outs.begin(), outs.end(), [](const Placed& a, const Placed& b) {
        if (a.rc.top / 100 != b.rc.top / 100) return a.rc.top < b.rc.top;
        return a.rc.left < b.rc.left;
    });
    for (int k = 0; k < (int)outs.size(); k++) {
        OutputCfg& o = *outs[k].o;
        int r = (R > 1) ? k / C : 0, c = (R > 1) ? k % C : (C > 1 ? k : 0);
        o.initIdentity();
        float offX = c * (1.0f - ov), offY = r * (1.0f - ov);
        for (size_t i = 0; i < o.points.size(); i += 2) {
            o.points[i] -= offX;
            o.points[i + 1] -= offY;
        }
        // blend only on interior boundaries of the tile grid
        o.edges = (c > 0 ? 1 : 0) | (c < C - 1 ? 2 : 0)
                | (r > 0 ? 4 : 0) | (r < R - 1 ? 8 : 0);
        // auto blend width: interior fades follow the source overlap, so
        // adjacent tiles carry complementary ramps over the same band (with
        // the default linear curve weightA + weightB == 1 across the seam).
        // Outer edges keep their user-set fades; 1x1 touches nothing.
        if (R > 1 || C > 1) {
            float fw = clampf(ov, 0.0f, 0.5f);
            if (c > 0) o.blend[0].fade = fw;
            if (c < C - 1) o.blend[1].fade = fw;
            if (r > 0) o.blend[2].fade = fw;
            if (r < R - 1) o.blend[3].fade = fw;
        }
        if (R == 1 && C == 1) o.edges = 0;   // 1x1: no interior boundary, no
                                             // forced fade (in-place correction
                                             // on the main screen; blending can
                                             // still be set via SET edges=ALL)
    }
    g_rev++;
}

static bool applyLayoutValue(int R, int C, int ov) {   // value update, any thread
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (g_cfg.layoutR == R && g_cfg.layoutC == C && g_cfg.overlap == ov) return false;
    g_cfg.layoutR = R;
    g_cfg.layoutC = C;
    g_cfg.overlap = ov;
    applyLayoutLocked();
    return true;
}

// combo resync, UI thread
static void syncLayoutCombos() {
    if (!g_cbLayout) return;
    int R, C;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        R = g_cfg.layoutR; C = g_cfg.layoutC;
        ComboBox_SetCurSel(g_cbOvl, g_cfg.overlap / 5);
        ComboBox_SetCurSel(g_cbEng, g_cfg.engine);
    }
    wchar_t key[8];
    _snwprintf_s(key, _TRUNCATE, L"%dx%d", C, R);
    int n = (int)ComboBox_GetCount(g_cbLayout);
    for (int i = 0; i < n; i++) {
        wchar_t t[8];
        ComboBox_GetLBText(g_cbLayout, i, t);
        if (wcscmp(t, key) == 0) { ComboBox_SetCurSel(g_cbLayout, i); break; }
    }
}

static void applyLayoutUI(int R, int C, int ov) {      // UI thread
    bool magTiles = false;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        magTiles = g_cfg.engine == 1;
    }
    bool changed = applyLayoutValue(R, C, ov);
    if (changed) syncLayoutCombos();
    if (magTiles) {
        // MAG hosts carry the tile's source window in their magnifier source
        // rect, so any layout/overlap change means a window rebuild. This
        // includes the socket path: setLayout already applied the values on
        // the socket thread (GET-after-SET consistency), which made
        // applyLayoutValue report "unchanged" here - without the unconditional
        // rebuild CMDNET SET layout= never retiled the MAG hosts (real bug).
        destroyOutputWindows();
        createOutputWindows();
    }
    refreshStatusBar();
}

void setLayout(int R, int C, int overlapPct) {
    if (R < 1) R = 1; if (R > 4) R = 4;
    if (C < 1) C = 1; if (C > 4) C = 4;
    if (overlapPct < 0) overlapPct = 0;
    if (overlapPct > 40) overlapPct = 40;
    // CMDNET runs on the socket thread: the grid edit itself is mutex-safe,
    // but combo/window updates belong to the UI thread (non-blocking post)
    if (g_ctl && uiThreadId && GetCurrentThreadId() != uiThreadId) {
        if (applyLayoutValue(R, C, overlapPct)) {
            std::lock_guard<std::mutex> lk(g_layMsgMutex);
            g_layMsg = { R, C, overlapPct };
            PostMessageW(g_ctl, WM_LAYSET, 0, 0);
        }
        return;
    }
    applyLayoutUI(R, C, overlapPct);
}

// ---- color adjustment dialog (brightness / contrast / gamma R G B) ----
// Edits ColorAdj of every ENABLED output live (shader applies per frame).
static HWND g_colorDlg = nullptr;
static HWND g_tb[5] = {}, g_tbVal[5] = {};
static const wchar_t* tbName[5] = { L"Brightness", L"Contrast", L"Gamma R", L"Gamma G", L"Gamma B" };
static const int tbMin[5] = { 20, 20, 20, 20, 20 };
static const int tbMax[5] = { 200, 200, 300, 300, 300 };

static float tbPosF(int i) {
    return (float)SendMessageW(g_tb[i], TBM_GETPOS, 0, 0) / 100.0f;
}

static void colorApplyFromSliders() {
    if (!g_tb[0]) return;
    ColorAdj c;
    c.bright = tbPosF(0);
    c.contrast = tbPosF(1);
    c.gammaR = tbPosF(2);
    c.gammaG = tbPosF(3);
    c.gammaB = tbPosF(4);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) if (o.enabled) o.color = c;
        g_rev++;
    }
    wchar_t t[32];
    for (int i = 0; i < 5; i++) {
        wsprintfW(t, L"%d.%02d", (int)tbPosF(i), (int)(tbPosF(i) * 100) % 100);
        SetWindowTextW(g_tbVal[i], t);
    }
    refreshStatusBar();
}

static void colorSetSliders(const ColorAdj& c) {
    int pos[5] = { (int)(c.bright * 100), (int)(c.contrast * 100),
                   (int)(c.gammaR * 100), (int)(c.gammaG * 100), (int)(c.gammaB * 100) };
    for (int i = 0; i < 5; i++)
        SendMessageW(g_tb[i], TBM_SETPOS, TRUE, (LPARAM)pos[i]);
}

static LRESULT CALLBACK ColorDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        for (int i = 0; i < 5; i++) {
            CreateWindowW(L"STATIC", tbName[i], WS_CHILD | WS_VISIBLE,
                10, 14 + i * 42, 90, 18, hwnd, nullptr, nullptr, nullptr);
            g_tb[i] = CreateWindowW(TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ,
                104, 8 + i * 42, 186, 30, hwnd, (HMENU)(2000 + i), nullptr, nullptr);
            SendMessageW(g_tb[i], TBM_SETRANGE, TRUE, MAKELPARAM(tbMin[i], tbMax[i]));
            g_tbVal[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                298, 14 + i * 42, 50, 18, hwnd, nullptr, nullptr, nullptr);
        }
        CreateWindowW(L"BUTTON", L"Reset",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 104, 226, 90, 26,
            hwnd, (HMENU)2005, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 204, 226, 90, 26,
            hwnd, (HMENU)2006, nullptr, nullptr);
        // init from the first enabled output (or identity defaults)
        ColorAdj c;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            for (auto& o : g_cfg.outputs) if (o.enabled) { c = o.color; break; }
        }
        colorSetSliders(c);
        colorApplyFromSliders();
        return 0;
    }
    case WM_HSCROLL:
        colorApplyFromSliders();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 2005) { colorSetSliders(ColorAdj()); colorApplyFromSliders(); }
        else if (id == 2006) DestroyWindow(hwnd);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_colorDlg = nullptr;
        for (auto& h : g_tb) h = nullptr;
        for (auto& h : g_tbVal) h = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void openColorDialog(HINSTANCE hInst, HWND owner) {
    (void)owner;
    if (g_colorDlg) {
        SetWindowPos(g_colorDlg, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = ColorDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"IWN_ColorDlg";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_colorDlg = CreateWindowExW(WS_EX_TOPMOST, L"IWN_ColorDlg",
        L"Color adjustment (applies to enabled outputs)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        140, 140, 370, 300, nullptr, nullptr, hInst, nullptr);
}

void openColorDialogEx() {   // hotkey D (registered only while calibrating)
    openColorDialog(GetModuleHandleW(nullptr), g_ctl);
}

// ------------------------------------------------------------------
// Blend curve editor (the original's "Advanced blend control", Luma
// channel): shows the selected edge's response curve
// w(t) = pow(blendCurve(t, gain, slope), gamma), lets the user tweak the
// three parameters and keep a per-side history. Fade (the band width) is
// edited on the Blending page - this dialog shapes the ramp response only.
// The R/G/B channel gammas edit the same side's per-channel curve
// exponents (weight_c = pow(curve, gamma*chanC); 1/1/1 = the luma curve).
static OutputCfg* cfgOutput();   // fwd: defined in the Configure-dialog section
static HWND g_curveDlg = nullptr;
static HWND g_cuvSide = nullptr, g_cuvGain = nullptr,
            g_cuvSlope = nullptr, g_cuvGamma = nullptr,
            g_cuvChanR = nullptr, g_cuvChanG = nullptr, g_cuvChanB = nullptr;
static HWND g_cuvArea = nullptr;          // the owner-drawn plot (subclassed)
static WNDPROC g_areaProcOld = nullptr;
struct CurveSnap { float gain, slope, gamma, chanR, chanG, chanB; };
static CurveSnap g_cuvSnap{ 0, 0, 1, 1, 1, 1 };   // painted snapshot: no lock in paint
static std::vector<CurveSnap> g_cuvHist[4];  // per-side history, newest last

#define CUV_SIDE     2050
#define CUV_GAIN     2051
#define CUV_SLOPE    2052
#define CUV_GAMMA    2053
#define CUV_APPLY    2054
#define CUV_HADD     2055
#define CUV_HRESTORE 2056
#define CUV_AREA     2057   // owner-draw static: the curve plot
#define CUV_CHANR    2058   // per-channel blend gamma edits
#define CUV_CHANG    2059
#define CUV_CHANB    2060

static int cuvSide() {
    int s = g_cuvSide ? ComboBox_GetCurSel(g_cuvSide) : -1;
    return (s < 0 || s > 3) ? 0 : s;
}

static float cuvEditF(HWND edit) {
    wchar_t t[32] = L"";
    GetWindowTextW(edit, t, 32);
    return (float)_wtof(t);
}

// load the selected side's parameters from the config into the edits + snapshot
static void cuvReload() {
    CurveSnap c{ 0, 0, 1, 1, 1, 1 };
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (o) {
            const BlendSide& b = o->blend[cuvSide()];
            c = { b.gain, b.slope, b.gamma, b.chanR, b.chanG, b.chanB };
        }
    }
    wchar_t t[16];
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.gain);
    SetWindowTextW(g_cuvGain, t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.slope);
    SetWindowTextW(g_cuvSlope, t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.gamma);
    SetWindowTextW(g_cuvGamma, t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanR);
    SetWindowTextW(g_cuvChanR, t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanG);
    SetWindowTextW(g_cuvChanG, t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanB);
    SetWindowTextW(g_cuvChanB, t);
    g_cuvSnap = c;
    if (g_curveDlg) InvalidateRect(g_curveDlg, nullptr, TRUE);
}

// write the edits back into the selected side's blend params
static void cuvApply() {
    int side = cuvSide();
    CurveSnap c{ clampf(cuvEditF(g_cuvGain), 0.0f, 2.0f),
                 clampf(cuvEditF(g_cuvSlope), 0.0f, 1.0f),
                 clampf(cuvEditF(g_cuvGamma), 0.1f, 5.0f),
                 clampf(cuvEditF(g_cuvChanR), 0.1f, 5.0f),
                 clampf(cuvEditF(g_cuvChanG), 0.1f, 5.0f),
                 clampf(cuvEditF(g_cuvChanB), 0.1f, 5.0f) };
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (o) {
            o->blend[side].gain = c.gain;
            o->blend[side].slope = c.slope;
            o->blend[side].gamma = c.gamma;
            o->blend[side].chanR = c.chanR;
            o->blend[side].chanG = c.chanG;
            o->blend[side].chanB = c.chanB;
            g_rev++;
        }
    }
    requestMagBlendSync();   // MAG overlay follows (same as the CMDNET path)
    g_cuvSnap = c;
    if (g_curveDlg) InvalidateRect(g_curveDlg, nullptr, TRUE);
    refreshStatusBar();
}

// mouse drag on the plot: shape the curve through the cursor by solving the
// parametric curve equation for gain (slope/gamma keep their edit values):
// w = pow(curve(t,gain,slope), gamma)  ->  cv = w^(1/gamma)  ->
// gain = (t - cv*slope*(t-1) - cv) / (t*(cv-1)). Release commits like Apply.
static LRESULT CALLBACK AreaProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        if (msg == WM_MOUSEMOVE && GetCapture() != h) break;
        SetCapture(h);   // starts tracking on down, no-op if already captured
        RECT rc;
        GetClientRect(h, &rc);
        int w = rc.right - rc.left, hh = rc.bottom - rc.top;
        if (w < 4 || hh < 4) return 0;
        float t = clampf((float)(short)LOWORD(lp) / (w - 1), 0.02f, 0.98f);
        float wv = clampf(1.0f - (float)(short)HIWORD(lp) / (hh - 1), 0.0f, 1.0f);
        float slope = clampf(cuvEditF(g_cuvSlope), 0.0f, 1.0f);
        float gamma = clampf(cuvEditF(g_cuvGamma), 0.1f, 5.0f);
        float cv = powf(wv, 1.0f / gamma);
        float gain = g_cuvSnap.gain;
        if (fabsf(cv - 1.0f) > 1e-3f)   // w~1 carries no gain information
            gain = (t - cv * slope * (t - 1.0f) - cv) / (t * (cv - 1.0f));
        gain = clampf(gain, 0.0f, 2.0f);
        wchar_t txt[16];
        _snwprintf_s(txt, _TRUNCATE, L"%.3f", gain);
        SetWindowTextW(g_cuvGain, txt);
        g_cuvSnap.gain = gain;   // live preview in the plot
        if (g_curveDlg) InvalidateRect(g_curveDlg, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        cuvApply();   // commit the dragged curve like the Apply button
        return 0;
    }
    return CallWindowProc(g_areaProcOld, h, msg, wp, lp);
}

static LRESULT CALLBACK CurveDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Side:", WS_CHILD | WS_VISIBLE,
            246, 13, 34, 18, hwnd, nullptr, nullptr, nullptr);
        g_cuvSide = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            284, 8, 120, 200, hwnd, (HMENU)CUV_SIDE, nullptr, nullptr);
        for (const wchar_t* n : { L"Left", L"Right", L"Top", L"Bottom" })
            ComboBox_AddString(g_cuvSide, n);
        ComboBox_SetCurSel(g_cuvSide, 0);
        static const struct { const wchar_t* name; int id; HWND* slot; } prm[6] = {
            { L"Gain", CUV_GAIN, &g_cuvGain },
            { L"Slope", CUV_SLOPE, &g_cuvSlope },
            { L"Gamma", CUV_GAMMA, &g_cuvGamma },
            { L"R chan", CUV_CHANR, &g_cuvChanR },
            { L"G chan", CUV_CHANG, &g_cuvChanG },
            { L"B chan", CUV_CHANB, &g_cuvChanB } };
        for (int i = 0; i < (int)(sizeof(prm) / sizeof(prm[0])); i++) {
            CreateWindowW(L"STATIC", prm[i].name, WS_CHILD | WS_VISIBLE,
                246, 55 + i * 32, 44, 18, hwnd, nullptr, nullptr, nullptr);
            *prm[i].slot = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000",
                WS_CHILD | WS_VISIBLE, 294, 51 + i * 32, 64, 22,
                hwnd, (HMENU)(INT_PTR)prm[i].id, nullptr, nullptr);
        }
        return 0;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (!di || di->CtlID != CUV_AREA) break;
        HDC dc = di->hDC;
        RECT rc = di->rcItem;
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        // 8x8 grid
        HPEN grid = CreatePen(PS_SOLID, 1, RGB(48, 48, 48));
        HPEN oldPen = (HPEN)SelectObject(dc, grid);
        for (int i = 1; i < 8; i++) {
            int x = rc.left + (rc.right - rc.left) * i / 8;
            int y = rc.top + (rc.bottom - rc.top) * i / 8;
            MoveToEx(dc, x, rc.top, nullptr);
            LineTo(dc, x, rc.bottom);
            MoveToEx(dc, rc.left, y, nullptr);
            LineTo(dc, rc.right, y);
        }
        // the response curve, sampled at 128 points; when a channel gamma
        // deviates from 1 its per-channel weight curve is drawn beneath the
        // luma curve in that channel's color
        struct CuCurve { COLORREF col; float chan; };
        const CuCurve chans[3] = {
            { RGB(220, 60, 60), g_cuvSnap.chanR },
            { RGB(60, 200, 60), g_cuvSnap.chanG },
            { RGB(80, 120, 255), g_cuvSnap.chanB } };
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        for (const CuCurve& cc : chans) {
            if (fabsf(cc.chan - 1.0f) < 0.001f) continue;
            HPEN cp = CreatePen(PS_SOLID, 1, cc.col);
            SelectObject(dc, cp);
            for (int i = 0; i <= 128; i++) {
                float t = i / 128.0f;
                float w = powf(magBlendCurve(t, g_cuvSnap.gain, g_cuvSnap.slope),
                               g_cuvSnap.gamma * cc.chan);
                int x = rc.left + (int)(t * (W - 2)) + 1;
                int y = rc.bottom - 1 - (int)(clampf(w, 0.0f, 1.0f) * (H - 2));
                if (i == 0) MoveToEx(dc, x, y, nullptr);
                else LineTo(dc, x, y);
            }
            SelectObject(dc, oldPen);
            DeleteObject(cp);
        }
        HPEN curve = CreatePen(PS_SOLID, 2, RGB(255, 140, 0));
        SelectObject(dc, curve);
        for (int i = 0; i <= 128; i++) {
            float t = i / 128.0f;
            float w = powf(magBlendCurve(t, g_cuvSnap.gain, g_cuvSnap.slope),
                           g_cuvSnap.gamma);
            int x = rc.left + (int)(t * (W - 2)) + 1;
            int y = rc.bottom - 1 - (int)(clampf(w, 0.0f, 1.0f) * (H - 2));
            if (i == 0) MoveToEx(dc, x, y, nullptr);
            else LineTo(dc, x, y);
        }
        SelectObject(dc, oldPen);
        DeleteObject(grid);
        DeleteObject(curve);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == CUV_SIDE && HIWORD(wp) == CBN_SELCHANGE) cuvReload();
        else if (id == CUV_APPLY) cuvApply();
        else if (id == CUV_HADD) {
            auto& h = g_cuvHist[cuvSide()];
            h.push_back(g_cuvSnap);
            if (h.size() > 8) h.erase(h.begin());
        } else if (id == CUV_HRESTORE) {
            auto& h = g_cuvHist[cuvSide()];
            if (!h.empty()) {
                CurveSnap c = h.back();
                h.pop_back();
                wchar_t t[16];
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.gain);   SetWindowTextW(g_cuvGain, t);
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.slope);  SetWindowTextW(g_cuvSlope, t);
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.gamma);  SetWindowTextW(g_cuvGamma, t);
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanR);  SetWindowTextW(g_cuvChanR, t);
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanG);  SetWindowTextW(g_cuvChanG, t);
                _snwprintf_s(t, _TRUNCATE, L"%.3f", c.chanB);  SetWindowTextW(g_cuvChanB, t);
                cuvApply();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_curveDlg = nullptr;
        g_cuvSide = g_cuvGain = g_cuvSlope = g_cuvGamma = nullptr;
        g_cuvChanR = g_cuvChanG = g_cuvChanB = nullptr;
        g_cuvArea = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void openCurveDialog(HINSTANCE hInst, HWND owner) {
    (void)owner;
    if (g_curveDlg) {
        SetWindowPos(g_curveDlg, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = CurveDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"IWN_CurveDlg";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(1 + COLOR_BTNFACE);
    RegisterClassW(&wc);
    g_curveDlg = CreateWindowExW(WS_EX_TOPMOST, L"IWN_CurveDlg",
        L"Blend curve editor (selected projector)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        180, 180, 430, 400, nullptr, nullptr, hInst, nullptr);
    if (g_curveDlg) {
        // the plot + remaining buttons (created here so the dialog frame
        // exists before we ask for client-relative positions)
        HWND area = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            10, 10, 220, 220, g_curveDlg, (HMENU)CUV_AREA, nullptr, nullptr);
        // subclass the plot for drag interaction (capture the STATIC's proc)
        if (area) {
            g_cuvArea = area;
            g_areaProcOld = (WNDPROC)SetWindowLongPtrW(area, GWLP_WNDPROC,
                                                       (LONG_PTR)AreaProc);
        }
        CreateWindowW(L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 246, 244, 100, 26,
            g_curveDlg, (HMENU)CUV_APPLY, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Add to history",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 246, 276, 120, 24,
            g_curveDlg, (HMENU)CUV_HADD, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Restore history(0)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 246, 304, 120, 24,
            g_curveDlg, (HMENU)CUV_HRESTORE, nullptr, nullptr);
        cuvReload();
    }
}

// ---- Configure display dialog (per-projector, mirrors the original's
// Bezels / Align / Geometry / Blending / Color / Masking / Help tabs) ----
static HWND g_cfgDlg = nullptr;
static HWND g_cfgTab = nullptr, g_cfgProj = nullptr;
static HWND g_cfgPanel[7] = {};   // one child "page" per tab
static int g_cfgSel = 0;          // index into g_cfg.outputs shown in the combo

// control ids (must not collide with 1001..1026 console ids or tray 2001..2004;
// the color dialog uses 2000..2006, so this dialog lives at 2100+)
#define CFG_PROJ     2100
#define CFG_TAB      2101
// geometry page
#define CFG_HORIZ    2110
#define CFG_VERT     2111
#define CFG_GAPPLY   2112
#define CFG_FLIPH    2113
#define CFG_FLIPV    2114
#define CFG_ROT90    2115
#define CFG_GRESET   2116
#define CFG_CURVED   2117
// blending page
#define CFG_SIDE     2120
#define CFG_FADE     2121
#define CFG_GAMMA    2122
#define CFG_GAIN     2123
#define CFG_SLOPE    2124
#define CFG_BAPPLY   2125
#define CFG_BALL     2126
#define CFG_BCURVE   2127   // button: open the blend curve editor
// color page
#define CFG_CTB(i)   (2130 + (i))      // 5 trackbars
#define CFG_CRESET   2136
// masking page
#define CFG_MLIST    2140
#define CFG_MADD     2141
#define CFG_MDEL     2142
#define CFG_MCLEAR   2143
#define CFG_MX       2144   // edit: mask left,   percent 0..100
#define CFG_MY       2145   // edit: mask top,    percent 0..100
#define CFG_MW       2146   // edit: mask width,  percent 2..100
#define CFG_MH       2147   // edit: mask height, percent 2..100
#define CFG_MAPPLY   2148   // button: write edits into the selected mask
#define CFG_MDEF     2149   // button: reset selected mask to 10%x10% at 0,0

static const wchar_t* cfgTabs[7] = {
    L"Bezels", L"Align", L"Geometry", L"Blending", L"Color", L"Masking", L"Help"
};

static OutputCfg* cfgOutput() {   // g_cfgMutex held by caller
    if (g_cfgSel < 0 || g_cfgSel >= (int)g_cfg.outputs.size()) return nullptr;
    return &g_cfg.outputs[g_cfgSel];
}

// fill the projector combo; keeps editing the same output across refreshes
static void cfgFillProjects() {
    if (!g_cfgProj) return;
    ComboBox_ResetContent(g_cfgProj);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        int n = 0;
        for (auto& o : g_cfg.outputs) {
            wchar_t t[64];
            std::wstring lab = o.label.empty() ? o.deviceName : o.label;
            wsprintfW(t, L"%d: %s%s", ++n, lab.c_str(),
                      o.enabled ? L"  [on]" : L"  [off]");
            ComboBox_AddString(g_cfgProj, t);
        }
        if (g_cfgSel >= n) g_cfgSel = n ? n - 1 : 0;
    }
    ComboBox_SetCurSel(g_cfgProj, g_cfgSel);
}

// ---- geometry transforms on the dest grid ----
static void cfgTransformPoints(void (*fn)(float&, float&)) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    OutputCfg* o = cfgOutput();
    if (!o) return;
    for (size_t i = 0; i < o->points.size(); i += 2)
        fn(o->points[i], o->points[i + 1]);
    g_rev++;
}

static void cfgReloadGeometry() {
    if (!g_cfgPanel[2]) return;
    wchar_t t[16];
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    OutputCfg* o = cfgOutput();
    if (!o) return;
    wsprintfW(t, L"%d", o->cols);
    SetWindowTextW(GetDlgItem(g_cfgPanel[2], CFG_HORIZ), t);
    wsprintfW(t, L"%d", o->rows);
    SetWindowTextW(GetDlgItem(g_cfgPanel[2], CFG_VERT), t);
    Button_SetCheck(GetDlgItem(g_cfgPanel[2], CFG_CURVED),
                    g_cfg.curved ? BST_CHECKED : BST_UNCHECKED);
}

static void cfgReloadBlend() {
    if (!g_cfgPanel[3]) return;
    int side = ComboBox_GetCurSel(GetDlgItem(g_cfgPanel[3], CFG_SIDE));
    if (side < 0 || side > 3) side = 0;
    wchar_t t[16];
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    OutputCfg* o = cfgOutput();
    if (!o) return;
    const BlendSide& b = o->blend[side];
    _snwprintf_s(t, _TRUNCATE, L"%.3f", b.fade);  SetWindowTextW(GetDlgItem(g_cfgPanel[3], CFG_FADE), t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", b.gamma); SetWindowTextW(GetDlgItem(g_cfgPanel[3], CFG_GAMMA), t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", b.gain);  SetWindowTextW(GetDlgItem(g_cfgPanel[3], CFG_GAIN), t);
    _snwprintf_s(t, _TRUNCATE, L"%.3f", b.slope); SetWindowTextW(GetDlgItem(g_cfgPanel[3], CFG_SLOPE), t);
}

static void cfgReloadColor() {
    if (!g_cfgPanel[4]) return;
    ColorAdj c;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (!o) return;
        c = o->color;
    }
    int pos[5] = { (int)(c.bright * 100), (int)(c.contrast * 100),
                   (int)(c.gammaR * 100), (int)(c.gammaG * 100), (int)(c.gammaB * 100) };
    for (int i = 0; i < 5; i++)
        SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(i)), TBM_SETPOS, TRUE, (LPARAM)pos[i]);
}

static void cfgReloadMaskEdits() {   // fill X/Y/W/H edits from the list selection
    if (!g_cfgPanel[5]) return;
    HWND lb = GetDlgItem(g_cfgPanel[5], CFG_MLIST);
    int sel = ListBox_GetCurSel(lb);
    MaskRect m;
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (o && sel >= 0 && sel < (int)o->masks.size()) { m = o->masks[sel]; has = true; }
    }
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, L"%.1f", m.x * 100.0f); SetWindowTextW(GetDlgItem(g_cfgPanel[5], CFG_MX), t);
    _snwprintf_s(t, _TRUNCATE, L"%.1f", m.y * 100.0f); SetWindowTextW(GetDlgItem(g_cfgPanel[5], CFG_MY), t);
    _snwprintf_s(t, _TRUNCATE, L"%.1f", m.w * 100.0f); SetWindowTextW(GetDlgItem(g_cfgPanel[5], CFG_MW), t);
    _snwprintf_s(t, _TRUNCATE, L"%.1f", m.h * 100.0f); SetWindowTextW(GetDlgItem(g_cfgPanel[5], CFG_MH), t);
    // grey out the editor when nothing is selected
    EnableWindow(GetDlgItem(g_cfgPanel[5], CFG_MAPPLY), has);
    EnableWindow(GetDlgItem(g_cfgPanel[5], CFG_MDEF), has);
}

static void cfgReloadMasks() {
    if (!g_cfgPanel[5]) return;
    HWND lb = GetDlgItem(g_cfgPanel[5], CFG_MLIST);
    int keep = ListBox_GetCurSel(lb);
    ListBox_ResetContent(lb);
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (!o) return;
        n = (int)o->masks.size();
        int idx = 1;
        for (auto& m : o->masks) {
            wchar_t t[96];
            // wsprintfW has no %f support -> CRT formatter (same trap as the status bar)
            _snwprintf_s(t, _TRUNCATE, L"#%d  X %.1f%%  Y %.1f%%  %.1f%% x %.1f%%", idx++,
                         m.x * 100.0f, m.y * 100.0f, m.w * 100.0f, m.h * 100.0f);
            ListBox_AddString(lb, t);
        }
    }
    if (n == 0) keep = -1;
    else if (keep < 0 || keep >= n) keep = n - 1;
    if (keep >= 0) ListBox_SetCurSel(lb, keep);
    cfgReloadMaskEdits();
}

static void cfgReloadAll() {
    cfgFillProjects();
    cfgReloadGeometry();
    cfgReloadBlend();
    cfgReloadColor();
    cfgReloadMasks();
}

// show/hide the page belonging to each tab
static void cfgShowPage() {
    int sel = TabCtrl_GetCurSel(g_cfgTab);
    for (int i = 0; i < 7; i++)
        ShowWindow(g_cfgPanel[i], (i == sel) ? SW_SHOW : SW_HIDE);
}

static void cfgApplyColor() {
    if (!g_cfgPanel[4]) return;
    ColorAdj c;
    c.bright   = (float)SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(0)), TBM_GETPOS, 0, 0) / 100.0f;
    c.contrast = (float)SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(1)), TBM_GETPOS, 0, 0) / 100.0f;
    c.gammaR   = (float)SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(2)), TBM_GETPOS, 0, 0) / 100.0f;
    c.gammaG   = (float)SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(3)), TBM_GETPOS, 0, 0) / 100.0f;
    c.gammaB   = (float)SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(4)), TBM_GETPOS, 0, 0) / 100.0f;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        OutputCfg* o = cfgOutput();
        if (o) o->color = c;
        g_rev++;
    }
    refreshStatusBar();
}

static float cfgEditF(HWND panel, int id) {
    wchar_t t[32] = L"";
    GetWindowTextW(GetDlgItem(panel, id), t, 32);
    return (float)_wtof(t);
}

static LRESULT CALLBACK CfgDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Projector:", WS_CHILD | WS_VISIBLE,
            10, 13, 62, 18, hwnd, nullptr, nullptr, nullptr);
        g_cfgProj = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            74, 9, 220, 200, hwnd, (HMENU)CFG_PROJ, nullptr, nullptr);
        g_cfgTab = CreateWindowW(WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
            6, 38, 402, 356, hwnd, (HMENU)CFG_TAB, nullptr, nullptr);
        // one page per tab; pages are plain containers positioned inside the tab
        for (int i = 0; i < 7; i++) {
            g_cfgPanel[i] = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_CLIPCHILDREN,   // hidden until its tab is picked
                14, 64, 386, 322, hwnd, nullptr, nullptr, nullptr);
        }
        // --- Geometry page (index 2) ---
        {
            HWND p = g_cfgPanel[2];
            CreateWindowW(L"STATIC", L"Control points:", WS_CHILD | WS_VISIBLE,
                10, 10, 90, 18, p, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Horiz", WS_CHILD | WS_VISIBLE, 104, 10, 34, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"12",
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 140, 7, 40, 22, p, (HMENU)CFG_HORIZ, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Vert", WS_CHILD | WS_VISIBLE, 190, 10, 30, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10",
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 222, 7, 40, 22, p, (HMENU)CFG_VERT, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Apply (resets grid)",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 272, 6, 104, 24, p, (HMENU)CFG_GAPPLY, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Flip horizontal", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 44, 114, 26, p, (HMENU)CFG_FLIPH, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Flip vertical", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                132, 44, 114, 26, p, (HMENU)CFG_FLIPV, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Rotate 90", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                254, 44, 114, 26, p, (HMENU)CFG_ROT90, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Reset grid", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 78, 114, 26, p, (HMENU)CFG_GRESET, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Curved grid (Catmull-Rom)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 132, 80, 220, 22, p,
                (HMENU)CFG_CURVED, nullptr, nullptr);
            CreateWindowW(L"STATIC",
                L"Curved: Catmull-Rom interpolation between control points\n"
                L"(needs Horiz/Vert >= 4, linear below). Per-point Move modes\n"
                L"live on the output window (F3): drag green points, arrows nudge.",
                WS_CHILD | WS_VISIBLE, 10, 120, 360, 40, p, nullptr, nullptr, nullptr);
        }
        // --- Blending page (index 3) ---
        {
            HWND p = g_cfgPanel[3];
            CreateWindowW(L"STATIC", L"Side:", WS_CHILD | WS_VISIBLE, 10, 12, 34, 18, p, nullptr, nullptr, nullptr);
            HWND cb = CreateWindowW(L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                48, 8, 110, 200, p, (HMENU)CFG_SIDE, nullptr, nullptr);
            ComboBox_AddString(cb, L"Left");
            ComboBox_AddString(cb, L"Right");
            ComboBox_AddString(cb, L"Top");
            ComboBox_AddString(cb, L"Bottom");
            ComboBox_SetCurSel(cb, 0);
            CreateWindowW(L"STATIC", L"Fade", WS_CHILD | WS_VISIBLE, 10, 46, 90, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.200", WS_CHILD | WS_VISIBLE, 104, 43, 70, 22, p, (HMENU)CFG_FADE, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Gamma", WS_CHILD | WS_VISIBLE, 10, 76, 90, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1.000", WS_CHILD | WS_VISIBLE, 104, 73, 70, 22, p, (HMENU)CFG_GAMMA, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Gain", WS_CHILD | WS_VISIBLE, 10, 106, 90, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000", WS_CHILD | WS_VISIBLE, 104, 103, 70, 22, p, (HMENU)CFG_GAIN, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Slope", WS_CHILD | WS_VISIBLE, 10, 136, 90, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000", WS_CHILD | WS_VISIBLE, 104, 133, 70, 22, p, (HMENU)CFG_SLOPE, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                190, 43, 96, 24, p, (HMENU)CFG_BAPPLY, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Apply to all 4 edges", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                190, 78, 150, 22, p, (HMENU)CFG_BALL, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Curve editor...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                190, 112, 150, 24, p, (HMENU)CFG_BCURVE, nullptr, nullptr);
            CreateWindowW(L"STATIC",
                L"Fade = soft ramp width, Gamma = ramp response.\n"
                L"Gain/Slope shape the curve: f(x)=(g+1)x / (slope(x-1)+gx+1).\n"
                L"Live editing with red points is available in blend mode (F10).",
                WS_CHILD | WS_VISIBLE, 10, 180, 360, 60, p, nullptr, nullptr, nullptr);
        }
        // --- Color page (index 4) ---
        {
            HWND p = g_cfgPanel[4];
            static const wchar_t* names[5] = { L"Brightness", L"Contrast", L"Gamma R", L"Gamma G", L"Gamma B" };
            for (int i = 0; i < 5; i++) {
                CreateWindowW(L"STATIC", names[i], WS_CHILD | WS_VISIBLE,
                    10, 14 + i * 40, 90, 18, p, nullptr, nullptr, nullptr);
                CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                    104, 8 + i * 40, 190, 30, p, (HMENU)CFG_CTB(i), nullptr, nullptr);
                SendMessageW(GetDlgItem(p, CFG_CTB(i)), TBM_SETRANGE, TRUE, MAKELPARAM(20, 300));
            }
            CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                104, 214, 90, 26, p, (HMENU)CFG_CRESET, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"(this projector only)",
                WS_CHILD | WS_VISIBLE, 204, 219, 150, 18, p, nullptr, nullptr, nullptr);
        }
        // --- Masking page (index 5) ---
        {
            HWND p = g_cfgPanel[5];
            CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                10, 10, 200, 140, p, (HMENU)CFG_MLIST, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Add 10% x 10%", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                218, 10, 152, 24, p, (HMENU)CFG_MADD, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Delete selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                218, 38, 152, 24, p, (HMENU)CFG_MDEL, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Clear all", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                218, 66, 152, 24, p, (HMENU)CFG_MCLEAR, nullptr, nullptr);

            CreateWindowW(L"STATIC", L"Mask rectangle (percent of screen):",
                WS_CHILD | WS_VISIBLE, 10, 158, 240, 18, p, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"X", WS_CHILD | WS_VISIBLE, 10, 186, 12, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.0",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 24, 183, 52, 22, p, (HMENU)CFG_MX, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Y", WS_CHILD | WS_VISIBLE, 84, 186, 12, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.0",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 98, 183, 52, 22, p, (HMENU)CFG_MY, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"W", WS_CHILD | WS_VISIBLE, 158, 186, 14, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10.0",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 174, 183, 52, 22, p, (HMENU)CFG_MW, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"H", WS_CHILD | WS_VISIBLE, 234, 186, 14, 18, p, nullptr, nullptr, nullptr);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10.0",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 250, 183, 52, 22, p, (HMENU)CFG_MH, nullptr, nullptr);

            CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 214, 110, 26, p, (HMENU)CFG_MAPPLY, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Reset 10%", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                128, 214, 110, 26, p, (HMENU)CFG_MDEF, nullptr, nullptr);
            CreateWindowW(L"STATIC",
                L"Mask rectangles render black on the output (up to 8).\n"
                L"Pick one in the list, type X/Y/W/H in percent and press Apply\n"
                L"(values are clamped so the rectangle stays on screen).",
                WS_CHILD | WS_VISIBLE, 10, 250, 360, 60, p, nullptr, nullptr, nullptr);
        }
        // --- Bezels / Align / Help placeholders (0, 1, 6) ---
        CreateWindowW(L"STATIC",
            L"Bezel compensation (shift the image by the monitor bezel width)\n"
            L"is not implemented yet. Overlap-driven tiling is on the console.",
            WS_CHILD | WS_VISIBLE, 20, 20, 350, 40, g_cfgPanel[0], nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC",
            L"Automatic camera alignment is not implemented.\n"
            L"Use Ctrl+F3 (screenshot calibration) + the output window's grid.",
            WS_CHILD | WS_VISIBLE, 20, 20, 350, 40, g_cfgPanel[1], nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC",
            L"F3  enter/exit calibration          F10  warp <-> blend mode\n"
            L"F12 (Ctrl+F12 global)  cycle blend edges\n"
            L"Ctrl+F3  freeze frame for alignment  D  color dialog\n"
            L"Arrows  nudge point 1px / Shift+Arrows 10px\n"
            L"Blend mode wheel: gamma | Ctrl+wheel gain | Shift+wheel slope",
            WS_CHILD | WS_VISIBLE, 20, 20, 350, 100, g_cfgPanel[6], nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            318, 400, 90, 26, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        cfgReloadAll();
        cfgShowPage();
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == CFG_TAB && nm->code == TCN_SELCHANGE) { cfgShowPage(); return 0; }
        break;
    }
    case WM_HSCROLL:
        cfgApplyColor();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == CFG_PROJ && HIWORD(wp) == CBN_SELCHANGE) {
            g_cfgSel = ComboBox_GetCurSel(g_cfgProj);
            cfgReloadGeometry();
            cfgReloadBlend();
            cfgReloadColor();
            cfgReloadMasks();
            return 0;
        }
        if (id == CFG_GAPPLY) {   // rebuild the grid at the new density
            int cols = (int)cfgEditF(g_cfgPanel[2], CFG_HORIZ);
            int rows = (int)cfgEditF(g_cfgPanel[2], CFG_VERT);
            cols = cols < 3 ? 3 : (cols > 24 ? 24 : cols);
            rows = rows < 3 ? 3 : (rows > 24 ? 24 : rows);
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = cfgOutput();
                if (o) { o->cols = cols; o->rows = rows; o->initIdentity(); g_rev++; }
            }
            cfgReloadGeometry();
            refreshList();
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_FLIPH) {
            cfgTransformPoints([](float& x, float& y) { (void)y; x = 1.0f - x; });
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_FLIPV) {
            cfgTransformPoints([](float& x, float& y) { (void)x; y = 1.0f - y; });
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_ROT90) {
            cfgTransformPoints([](float& x, float& y) { float t = x; x = 1.0f - y; y = t; });
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_GRESET) {
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = cfgOutput();
                if (o) { o->initIdentity(); g_rev++; }
            }
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_CURVED) {   // curved (Catmull-Rom) vs bilinear mesh
            bool on = Button_GetCheck(GetDlgItem(g_cfgPanel[2], CFG_CURVED)) == BST_CHECKED;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                if (g_cfg.curved != (on ? 1 : 0)) { g_cfg.curved = on ? 1 : 0; g_rev++; }
            }
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_BAPPLY) {
            int side = ComboBox_GetCurSel(GetDlgItem(g_cfgPanel[3], CFG_SIDE));
            if (side < 0 || side > 3) side = 0;
            bool all = Button_GetCheck(GetDlgItem(g_cfgPanel[3], CFG_BALL)) == BST_CHECKED;
            float fade = cfgEditF(g_cfgPanel[3], CFG_FADE);
            float gamma = cfgEditF(g_cfgPanel[3], CFG_GAMMA);
            float gain = cfgEditF(g_cfgPanel[3], CFG_GAIN);
            float slope = cfgEditF(g_cfgPanel[3], CFG_SLOPE);
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = cfgOutput();
                if (o) {
                    BlendSide b;
                    b.fade = clampf(fade, 0.0f, 0.5f);
                    b.gamma = clampf(gamma, 0.1f, 5.0f);
                    b.gain = clampf(gain, 0.0f, 2.0f);
                    b.slope = clampf(slope, 0.0f, 1.0f);
                    for (int s = 0; s < 4; s++)
                        if (all || s == side) o->blend[s] = b;
                    g_rev++;
                }
            }
            requestMagBlendSync();   // MAG overlay follows (was missing: the
                                     // dialog path never reached the overlays)
            cfgReloadBlend();
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_BCURVE) {
            openCurveDialog(GetModuleHandleW(nullptr), g_cfgDlg);
            return 0;
        }
        if (id == CFG_CRESET) {
            for (int i = 0; i < 5; i++)
                SendMessageW(GetDlgItem(g_cfgPanel[4], CFG_CTB(i)), TBM_SETPOS, TRUE, 100);
            cfgApplyColor();
            return 0;
        }
        if (id == CFG_MADD || id == CFG_MDEL || id == CFG_MCLEAR) {
            int added = -1;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = cfgOutput();
                if (o) {
                    if (id == CFG_MADD) {
                        if (o->masks.size() < MAX_MASKS) {
                            o->masks.push_back(MaskRect{});   // 10%x10% at top-left
                            added = (int)o->masks.size() - 1;
                        }
                    } else if (id == CFG_MDEL) {
                        int sel = ListBox_GetCurSel(GetDlgItem(g_cfgPanel[5], CFG_MLIST));
                        if (sel >= 0 && sel < (int)o->masks.size())
                            o->masks.erase(o->masks.begin() + sel);
                    } else {
                        o->masks.clear();
                    }
                    g_rev++;
                }
            }
            cfgReloadMasks();
            if (added >= 0) {   // select the mask we just created so it can be edited
                ListBox_SetCurSel(GetDlgItem(g_cfgPanel[5], CFG_MLIST), added);
                cfgReloadMaskEdits();
            }
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_MAPPLY || id == CFG_MDEF) {
            int sel = ListBox_GetCurSel(GetDlgItem(g_cfgPanel[5], CFG_MLIST));
            float x = cfgEditF(g_cfgPanel[5], CFG_MX) / 100.0f;
            float y = cfgEditF(g_cfgPanel[5], CFG_MY) / 100.0f;
            float w = cfgEditF(g_cfgPanel[5], CFG_MW) / 100.0f;
            float h = cfgEditF(g_cfgPanel[5], CFG_MH) / 100.0f;
            if (id == CFG_MDEF) { x = 0.0f; y = 0.0f; w = 0.1f; h = 0.1f; }
            w = clampf(w, 0.02f, 1.0f);
            h = clampf(h, 0.02f, 1.0f);
            x = clampf(x, 0.0f, 1.0f - w);   // keep the rectangle fully on screen
            y = clampf(y, 0.0f, 1.0f - h);
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = cfgOutput();
                if (o && sel >= 0 && sel < (int)o->masks.size()) {
                    MaskRect& m = o->masks[sel];
                    m.x = x; m.y = y; m.w = w; m.h = h;
                    g_rev++;
                }
            }
            cfgReloadMasks();
            refreshStatusBar();
            return 0;
        }
        if (id == CFG_MLIST && HIWORD(wp) == LBN_SELCHANGE) {
            cfgReloadMaskEdits();
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_cfgDlg = nullptr;
        g_cfgTab = nullptr;
        g_cfgProj = nullptr;
        for (auto& h : g_cfgPanel) h = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void openConfigDialogInternal(HINSTANCE hInst) {
    if (g_cfgDlg) {
        SetWindowPos(g_cfgDlg, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    // start on the output selected in the console list, if any
    int sel = ListBox_GetCurSel(g_list);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfgSel = 0;
        auto displays = enumerateDisplays();
        if (sel >= 0 && sel < (int)displays.size()) {
            int n = 0;
            for (auto& o : g_cfg.outputs) {
                if (sameDisplayName(o.deviceName, displays[sel].deviceName))
                    g_cfgSel = n;
                n++;
            }
            if (g_cfgSel >= n) g_cfgSel = n ? n - 1 : 0;
        }
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = CfgDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"IWN_CfgDlg";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_cfgDlg = CreateWindowExW(WS_EX_TOPMOST, L"IWN_CfgDlg",
        L"Configure display",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        160, 120, 424, 474, nullptr, nullptr, hInst, nullptr);
}

void openConfigDialog() {
    openConfigDialogInternal(GetModuleHandleW(nullptr));
}

// named presets live in %APPDATA%\ImmersiveWarpNative\presets and are shared
// with the CMDNET SAVEAS/LOADAS commands
static std::string toNarrowStr(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static void refreshPresetCombo() {
    if (!g_cbPreset) return;
    wchar_t keep[64] = L"";
    int sel = ComboBox_GetCurSel(g_cbPreset);
    if (sel >= 0) ComboBox_GetLBText(g_cbPreset, sel, keep);
    ComboBox_ResetContent(g_cbPreset);
    std::wstring file = presetPath("x");
    std::wstring dir = file.substr(0, file.find_last_of(L'\\') + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cfg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            if (n.size() > 4 && n.rfind(L".cfg") == n.size() - 4) n = n.substr(0, n.size() - 4);
            ComboBox_AddString(g_cbPreset, n.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (keep[0]) {   // keep the previous selection if that preset still exists
        int cnt = ComboBox_GetCount(g_cbPreset);
        for (int i = 0; i < cnt; i++) {
            wchar_t t[64];
            ComboBox_GetLBText(g_cbPreset, i, t);
            if (wcscmp(t, keep) == 0) { ComboBox_SetCurSel(g_cbPreset, i); return; }
        }
    }
    ComboBox_SetCurSel(g_cbPreset, 0);
}

// Console "Target:" dropdown: one entry per enabled output plus AUTO.
// Reads the shared command target (cmdGetTarget), so network and UI always
// agree. Safe on the socket thread (CMDNET SET out= calls it) -- it takes
// g_cfgMutex itself; never call it while that mutex is held.
static void refreshTargetComboNow() {
    if (!g_cbTarget) return;
    g_tgtBusy = true;
    ComboBox_ResetContent(g_cbTarget);
    g_tgtShort.clear();
    ComboBox_AddString(g_cbTarget, L"Auto (first enabled output)");
    g_tgtShort.push_back("");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) {
            if (!o.enabled) continue;
            std::wstring s = o.deviceName;
            size_t p = s.find(L"\\\\.\\");     // "\\.\DISPLAY2" -> "DISPLAY2"
            if (p != std::wstring::npos) s = s.substr(p + 4);
            wchar_t line[192];
            wsprintfW(line, L"%s", s.c_str());
            ComboBox_AddString(g_cbTarget, line);
            g_tgtShort.push_back(toNarrowStr(s.c_str()));
        }
    }
    std::string cur = cmdGetTarget();
    int sel = 0;
    for (size_t i = 0; i < g_tgtShort.size(); i++)
        if (_stricmp(g_tgtShort[i].c_str(), cur.c_str()) == 0) { sel = (int)i; break; }
    ComboBox_SetCurSel(g_cbTarget, sel);
    g_tgtBusy = false;
}

void refreshTargetComboUI() {
    // combo calls are SendMessage to a UI-thread window: from the socket
    // thread they block until the UI thread pumps messages - and while it is
    // busy rebuilding outputs that wait is unbounded (SET out= would hang).
    // Marshal to the UI thread instead, mirroring refreshEngineComboUI.
    if (g_ctl && uiThreadId && GetCurrentThreadId() != uiThreadId) {
        PostMessageW(g_ctl, WM_TGTREFRESH, 0, 0);
        return;
    }
    refreshTargetComboNow();
}

// shared by Load Config and preset load: resync all UI + engine state after
// loadConfig() swapped g_cfg. Must be called WITHOUT g_cfgMutex held.
static void applyLoadedConfig() {
    refreshList();
    g_previewMode.store(g_cfg.previewMode);
    g_inplace.store(g_cfg.inplace);
    Button_SetCheck(g_ckPreview, g_cfg.previewMode ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_ckInplace, g_cfg.inplace ? BST_CHECKED : BST_UNCHECKED);
    PostMessageW(g_ctl, WM_INPLACESET, 0, 0);   // style change needs a rebuild
    PostMessageW(g_ctl, WM_MAGTEST, 0, 0);      // MAG overlays + Test checkbox follow
    requestMagBlendSync();                      // MAG fade-bar overlays follow
    invalidateToolbarToggles();                 // Test button checked frame
    Button_SetCheck(g_ckImage, (g_cfg.useImage && engineHasImage()) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_ckAuto, queryAutostart() ? BST_CHECKED : BST_UNCHECKED);
    Engine::inst().setSampling(g_cfg.sampling);
    ComboBox_SetCurSel(g_cbSamp, g_cfg.sampling);
    ComboBox_SetCurSel(g_cbImg, g_cfg.testImg);
    syncLayoutCombos();
    refreshTargetComboUI();   // enabled outputs may have changed with the config
    if (!g_cfg.imagePath.empty() && engineLoadImage(g_cfg.imagePath)) {
        const wchar_t* base = g_cfg.imagePath.c_str();
        for (const wchar_t* p = g_cfg.imagePath.c_str(); *p; p++)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        wchar_t line[512];
        wsprintfW(line, L"Image: %s  (%d x %d)", base,
                  Engine::inst().imageWidth(), Engine::inst().imageHeight());
        SetWindowTextW(g_imgLabel, line);
    }
    refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
}

// preset enumeration in name order (Ctrl+Shift+1..8 pick deterministically)
static std::vector<std::wstring> presetNamesSorted() {
    std::vector<std::wstring> names;
    std::wstring file = presetPath("x");
    std::wstring dir = file.substr(0, file.find_last_of(L'\\') + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cfg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            if (n.size() > 4 && n.rfind(L".cfg") == n.size() - 4) n = n.substr(0, n.size() - 4);
            names.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Ctrl+Shift+1..8, the original's config-set hotkeys: load the Nth preset in
// name order. UI thread only (dispatched from WM_HOTKEY in main.cpp).
void loadPresetByIndex(int idx) {   // 0-based
    auto names = presetNamesSorted();
    if (idx < 0 || idx >= (int)names.size()) {
        wchar_t m[128];
        wsprintfW(m, L"No preset #%d (%d found). Ctrl+Shift+1..8 load presets in name order.",
                  idx + 1, (int)names.size());
        setStatus(m);
        return;
    }
    if (loadConfig(presetPath(toNarrowStr(names[idx].c_str())))) {
        applyLoadedConfig();
        wchar_t m[96];
        wsprintfW(m, L"Preset '%s' loaded (Ctrl+Shift+%d).", names[idx].c_str(), idx + 1);
        setStatus(m);
    } else {
        setStatus(L"Preset load failed.");
    }
}

// ---------------- menu bar (File/View/Warping/Help, 1:1 with the original) ----------------
static HMENU buildMenuBar() {
    HMENU mb = CreateMenu();
    HMENU mFile = CreatePopupMenu();
    AppendMenuW(mFile, MF_STRING, IDM_FILE_LOAD, L"Load Config");
    AppendMenuW(mFile, MF_STRING, IDM_FILE_SAVE, L"Save Config");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mFile, MF_STRING, IDM_FILE_EXIT, L"Exit");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)mFile, L"&File");

    HMENU mView = CreatePopupMenu();
    static const wchar_t* pageNames[PAGE_COUNT] =
        { L"Displays", L"Warp && Blend", L"Image && Test", L"Settings" };
    for (int i = 0; i < PAGE_COUNT; i++)
        AppendMenuW(mView, MF_STRING, IDM_VIEW_DISP + i, pageNames[i]);
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mView, MF_STRING, IDM_VIEW_INPLACE, L"In-place overlay (click-through)\tCtrl+F9");
    AppendMenuW(mView, MF_STRING, IDM_VIEW_BLACKOUT, L"Blackout (all outputs black)");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mView, MF_STRING, IDM_VIEW_HIDE, L"Hide console to tray");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)mView, L"&View");

    HMENU mWarp = CreatePopupMenu();
    AppendMenuW(mWarp, MF_STRING, IDM_WARP_ALIGN, L"Configure alignment\tF3");
    AppendMenuW(mWarp, MF_STRING, IDM_WARP_EDGES, L"Cycle blend edges\tCtrl+F12");
    AppendMenuW(mWarp, MF_STRING, IDM_WARP_SNAP, L"Snapshot freeze\tCtrl+F3");
    AppendMenuW(mWarp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mWarp, MF_STRING, IDM_WARP_TEST, L"Test pattern");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)mWarp, L"&Warping");

    HMENU mHelp = CreatePopupMenu();
    AppendMenuW(mHelp, MF_STRING, IDM_HELP_ABOUT, L"About...");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)mHelp, L"&Help");
    return mb;
}

// full dark mode for the common controls (DarkMode_Explorer, Win10 1809+;
// a plain no-op on older systems or unsupported controls)
static void applyDarkThemeRecursive(HWND root) {
    EnumChildWindows(root, [](HWND h, LPARAM) -> BOOL {
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        return TRUE;
    }, 0);
    if (g_tab) SetWindowTheme(g_tab, L"DarkMode_Explorer", nullptr);
}

// ---------------- Displays page schematic (owner-drawn static) ----------------
// Dark panel with one dashed rectangle per display at its real relative
// position/size: dashed orange + [PROJECTOR] for enabled outputs, dashed gray
// for displays that only take part as capture source.
static void drawDisplaySchematic(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    auto displays = enumerateDisplays();
    std::vector<bool> proj(displays.size(), false);
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (size_t i = 0; i < displays.size(); i++)
            for (auto& o : g_cfg.outputs)
                if (o.enabled && o.deviceName == displays[i].deviceName) { proj[i] = true; break; }
    }
    if (displays.empty()) return;
    LONG x0 = MAXLONG, y0 = MAXLONG, x1 = -MAXLONG, y1 = -MAXLONG;
    for (auto& d : displays) {
        x0 = (std::min)(x0, d.desktop.left);  y0 = (std::min)(y0, d.desktop.top);
        x1 = (std::max)(x1, d.desktop.right); y1 = (std::max)(y1, d.desktop.bottom);
    }
    LONG vw = x1 - x0, vh = y1 - y0;
    if (vw < 1 || vh < 1) return;
    int pad = 8;
    int aw = rc.right - rc.left - 2 * pad, ah = rc.bottom - rc.top - 2 * pad;
    float s = (std::min)(aw / (float)vw, ah / (float)vh);
    HPEN penP = CreatePen(PS_DASH, 1, RGB(232, 148, 10));
    HPEN penN = CreatePen(PS_DASH, 1, RGB(120, 120, 128));
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ op = SelectObject(dc, penN), of = SelectObject(dc, f);
    HBRUSH hollow = (HBRUSH)GetStockObject(NULL_BRUSH);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < displays.size(); i++) {
        RECT& d = displays[i].desktop;
        int l = rc.left + pad + (int)((d.left - x0) * s);
        int t = rc.top + pad + (int)((d.top - y0) * s);
        int r = rc.left + pad + (int)((d.right - x0) * s);
        int b = rc.top + pad + (int)((d.bottom - y0) * s);
        if (r - l < 3) r = l + 3;
        if (b - t < 3) b = t + 3;
        SelectObject(dc, proj[i] ? penP : penN);
        SelectObject(dc, hollow);
        Rectangle(dc, l, t, r, b);
        SetTextColor(dc, proj[i] ? RGB(232, 148, 10) : RGB(160, 160, 168));
        wchar_t lbl[192];
        wsprintfW(lbl, L"%s%s%s", displays[i].label.c_str(),
                  displays[i].primary ? L" [Primary]" : L"",
                  proj[i] ? L"  [PROJECTOR]" : L"");
        RECT tr = { l + 3, t + 2, r - 2, t + 18 };
        DrawTextW(dc, lbl, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, op);
    SelectObject(dc, of);
    DeleteObject(penP);
    DeleteObject(penN);
    DeleteObject(f);
}

// toolbar owner-draw: dark button, orange glyph, light label. The glyph set
// echoes the original's icon strip (folder / floppy / checker / fullscreen /
// crosshair / play / stop). Command ids are shared with the menu and the
// pages, so only the LOOK is toolbar-specific.
static bool isToolbarCmd(int id) {
    return id == IDC_LOAD || id == IDC_SAVE || id == IDM_WARP_TEST ||
           id == IDM_VIEW_INPLACE || id == IDC_CALIB ||
           id == IDC_START || id == IDC_STOP || id == IDC_EXIT;
}

static void drawToolbarGlyph(HDC dc, int id, int x, int y, int s) {
    HBRUSH acc = CreateSolidBrush(RGB(232, 148, 10));   // orange accent
    HBRUSH lite = CreateSolidBrush(RGB(200, 200, 205));
    HBRUSH dark = CreateSolidBrush(RGB(60, 60, 66));
    HGDIOBJ of = SelectObject(dc, acc);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    switch (id) {
    case IDC_LOAD: {                       // open folder
        RECT body{ x, y + s / 3, x + s, y + s };
        FillRect(dc, &body, acc);
        RECT tab{ x, y, x + s / 2, y + s / 3 + 1 };
        FillRect(dc, &tab, acc);
        break;
    }
    case IDC_SAVE: {                       // floppy
        RECT body{ x, y, x + s, y + s };
        FillRect(dc, &body, lite);
        RECT shut{ x + s / 4, y, x + 3 * s / 4, y + s / 3 };
        FillRect(dc, &shut, dark);
        RECT lbl{ x + s / 4, y + 2 * s / 3, x + 3 * s / 4, y + s };
        FillRect(dc, &lbl, dark);
        break;
    }
    case IDM_WARP_TEST: {                  // mini checkerboard
        int c = s / 4;
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                RECT cell{ x + k * c, y + r * c, x + (k + 1) * c, y + (r + 1) * c };
                FillRect(dc, &cell, ((r + k) & 1) ? lite : acc);
            }
        break;
    }
    case IDM_VIEW_INPLACE: {               // dashed fullscreen over a dot
        HPEN dash = CreatePen(PS_DASH, 1, RGB(232, 148, 10));
        HGDIOBJ od = SelectObject(dc, dash);
        SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, x, y, x + s, y + s);
        SelectObject(dc, acc);
        RECT dot{ x + s / 3, y + s / 3, x + 2 * s / 3, y + 2 * s / 3 };
        FillRect(dc, &dot, acc);
        SelectObject(dc, od);
        DeleteObject(dash);
        break;
    }
    case IDC_CALIB: {                      // crosshair
        RECT h{ x, y + s / 2 - 1, x + s, y + s / 2 + 1 };
        RECT v{ x + s / 2 - 1, y, x + s / 2 + 1, y + s };
        FillRect(dc, &h, acc);
        FillRect(dc, &v, acc);
        SelectObject(dc, lite);
        RECT c{ x + s / 3, y + s / 3, x + 2 * s / 3, y + 2 * s / 3 };
        FillRect(dc, &c, lite);
        break;
    }
    case IDC_START: {                      // play triangle
        POINT tri[3] = { { x, y }, { x, y + s }, { x + s, y + s / 2 } };
        HGDIOBJ ob = SelectObject(dc, acc);
        Polygon(dc, tri, 3);
        SelectObject(dc, ob);
        break;
    }
    case IDC_STOP: {                       // stop square
        RECT q{ x, y, x + s, y + s };
        FillRect(dc, &q, acc);
        break;
    }
    case IDC_EXIT: {                       // X mark (original: toolbar right end)
        HPEN xp = CreatePen(PS_SOLID, 2, RGB(232, 148, 10));
        HGDIOBJ op2 = SelectObject(dc, xp);
        MoveToEx(dc, x + 1, y + 1, nullptr);
        LineTo(dc, x + s - 1, y + s - 1);
        MoveToEx(dc, x + s - 1, y + 1, nullptr);
        LineTo(dc, x + 1, y + s - 1);
        SelectObject(dc, op2);
        DeleteObject(xp);
        break;
    }
    }
    SelectObject(dc, of);
    SelectObject(dc, op);
    DeleteObject(acc);
    DeleteObject(lite);
    DeleteObject(dark);
}

static void drawToolbarButton(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool hot = (dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    // toggle state read WITHOUT g_cfgMutex: WM_DRAWITEM can be dispatched
    // synchronously inside paint, and this thread must never wait on a lock
    // another thread may hold while posting UI work (benign int race).
    bool active = false;
    if (dis->CtlID == IDM_VIEW_INPLACE) active = g_inplace.load();
    else if (dis->CtlID == IDM_WARP_TEST) active = g_cfg.testPattern != 0;
    HBRUSH bg = CreateSolidBrush(hot ? RGB(70, 70, 78)
                                     : active ? RGB(58, 54, 40) : RGB(45, 45, 48));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (active) {                       // orange frame marks the "on" toggles
        HPEN fr = CreatePen(PS_SOLID, 1, RGB(232, 148, 10));
        HGDIOBJ op = SelectObject(dc, fr);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(fr);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 225));
    wchar_t lbl[64];
    GetWindowTextW(dis->hwndItem, lbl, 64);
    int s = 14;
    drawToolbarGlyph(dc, dis->CtlID, rc.left + 8, rc.top + (rc.bottom - rc.top - s) / 2, s);
    RECT tr{ rc.left + 8 + s + 6, rc.top, rc.right - 4, rc.bottom };
    DrawTextW(dc, lbl, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
}

static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // 1:1 with the original console: menu bar on top, multi-page tab strip,
        // always-visible bottom strip (Configure alignment + Enable Desktop
        // warping + warp-engine selector) + status block. Controls keep their
        // dialog ids; pages only toggle visibility, so WM_COMMAND automation
        // still works.
        SetMenu(hwnd, buildMenuBar());   // File / View / Warping / Help

        // toolbar band (1:1 with the original's top icon strip): flat text
        // buttons that reuse the existing command ids, so menu handlers and
        // unattended tests drive the exact same code paths.
        struct TbBtn { int id; const wchar_t* label; int w; };
        static const TbBtn tbBtns[] = {
            { IDC_LOAD,       L"Load",       62 },
            { IDC_SAVE,       L"Save",       62 },
            { IDM_WARP_TEST,  L"Test",       58 },
            { IDM_VIEW_INPLACE, L"In-place", 76 },
            { IDC_CALIB,      L"Calibrate",  78 },
            { IDC_START,      L"Start",      62 },
            { IDC_STOP,       L"Stop",       58 },
        };
        int tbx = 10;
        for (const auto& b : tbBtns) {
            CreateWindowW(L"BUTTON", b.label,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                tbx, 8, b.w, 26, hwnd, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
            tbx += b.w + 6;
        }
        // Exit pinned to the right end, like the original's icon strip
        CreateWindowW(L"BUTTON", L"Exit",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            632 - 58 - 10, 8, 58, 26, hwnd, (HMENU)(INT_PTR)IDC_EXIT, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 8, 38, 622, 2,
            hwnd, nullptr, nullptr, nullptr);

        g_tab = CreateWindowW(WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
            6, 44, 622, 396, hwnd, (HMENU)IDC_TABS, nullptr, nullptr);
        static const wchar_t* tabTitles[PAGE_COUNT] =
            { L"Displays", L"Warp & Blend", L"Image & Test", L"Settings" };
        for (int i = 0; i < PAGE_COUNT; i++) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)tabTitles[i];
            TabCtrl_InsertItem(g_tab, i, &ti);
        }
        auto add = [](int pg, HWND h) { if (h) g_pages[pg].push_back(h); return h; };

        // --- Displays page: output list + projector controls + capture source
        add(PAGE_DISPLAYS, g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | LBS_NOTIFY | WS_VSCROLL,
            16, 44, 410, 180, hwnd, (HMENU)IDC_LIST, nullptr, nullptr));
        add(PAGE_DISPLAYS, CreateWindowW(L"BUTTON", L"Toggle selected as projector",
            WS_CHILD | BS_PUSHBUTTON, 436, 44, 178, 28,
            hwnd, (HMENU)IDC_TOGGLE, nullptr, nullptr));
        add(PAGE_DISPLAYS, CreateWindowW(L"BUTTON", L"Start Output",
            WS_CHILD | BS_PUSHBUTTON, 436, 80, 178, 28,
            hwnd, (HMENU)IDC_START, nullptr, nullptr));
        add(PAGE_DISPLAYS, CreateWindowW(L"BUTTON", L"Stop Output",
            WS_CHILD | BS_PUSHBUTTON, 436, 116, 178, 28,
            hwnd, (HMENU)IDC_STOP, nullptr, nullptr));
        add(PAGE_DISPLAYS, CreateWindowW(L"STATIC", L"Source:",
            WS_CHILD | SS_LEFT, 16, 240, 54, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_DISPLAYS, g_cbSrc = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            72, 236, 290, 220,
            hwnd, (HMENU)IDC_CBSRC, nullptr, nullptr));
        add(PAGE_DISPLAYS, CreateWindowW(L"STATIC", L"(auto = first display that is not an output)",
            WS_CHILD | SS_LEFT, 16, 266, 440, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_DISPLAYS, g_ckPreview = CreateWindowW(L"BUTTON", L"Windowed preview (single screen)",
            WS_CHILD | BS_AUTOCHECKBOX, 380, 236, 232, 24,
            hwnd, (HMENU)IDC_CKPREVIEW, nullptr, nullptr));
        add(PAGE_DISPLAYS, g_ckInplace = CreateWindowW(L"BUTTON",
            L"In-place overlay (fullscreen, click-through)",
            WS_CHILD | BS_AUTOCHECKBOX, 16, 296, 320, 24,
            hwnd, (HMENU)IDC_CKINPLACE, nullptr, nullptr));
        // output schematic: one dashed box per display at its real relative
        // position, orange for enabled projectors (owner-draw via WM_DRAWITEM)
        add(PAGE_DISPLAYS, g_schematic = CreateWindowW(L"STATIC", L"outputs",
            WS_CHILD | SS_OWNERDRAW, 16, 322, 612, 78,
            hwnd, (HMENU)IDC_SCHEMATIC, nullptr, nullptr));

        // --- Warp & Blend page
        add(PAGE_WARP, CreateWindowW(L"BUTTON", L"Cycle blend edges (Ctrl+F12)",
            WS_CHILD | BS_PUSHBUTTON, 16, 44, 200, 28,
            hwnd, (HMENU)IDC_EDGES, nullptr, nullptr));
        add(PAGE_WARP, CreateWindowW(L"STATIC", L"Sampling:",
            WS_CHILD | SS_LEFT, 16, 92, 62, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_WARP, g_cbSamp = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            82, 88, 150, 200,
            hwnd, (HMENU)IDC_CBSAMP, nullptr, nullptr));
        ComboBox_AddString(g_cbSamp, L"Trilinear");
        ComboBox_AddString(g_cbSamp, L"Anisotropic 4x");
        ComboBox_AddString(g_cbSamp, L"Anisotropic 8x");
        ComboBox_AddString(g_cbSamp, L"Bi-cubic (Catmull-Rom)");
        add(PAGE_WARP, CreateWindowW(L"STATIC", L"Layout:",
            WS_CHILD | SS_LEFT, 260, 92, 50, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_WARP, g_cbLayout = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            312, 88, 74, 220,
            hwnd, (HMENU)IDC_CBLAYOUT, nullptr, nullptr));
        for (int R = 1; R <= 4; R++)
            for (int C = 1; C <= 4; C++) {
                // 1x1 included: single-output in-place correction on the main
                // screen (same entry the original shows in its NxN menu)
                wchar_t t[8];
                _snwprintf_s(t, _TRUNCATE, L"%dx%d", C, R);
                ComboBox_AddString(g_cbLayout, t);
            }
        add(PAGE_WARP, CreateWindowW(L"STATIC", L"Overlap:",
            WS_CHILD | SS_LEFT, 400, 92, 56, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_WARP, g_cbOvl = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            458, 88, 66, 220,
            hwnd, (HMENU)IDC_CBOVL, nullptr, nullptr));
        for (int p = 0; p <= 40; p += 5) {
            wchar_t t[8];
            wsprintfW(t, L"%d%%", p);
            ComboBox_AddString(g_cbOvl, t);
        }
        add(PAGE_WARP, CreateWindowW(L"BUTTON", L"Configure display...",
            WS_CHILD | BS_PUSHBUTTON, 16, 132, 180, 28,
            hwnd, (HMENU)IDC_CFGDLG, nullptr, nullptr));
        add(PAGE_WARP, CreateWindowW(L"BUTTON", L"Color adjustment...",
            WS_CHILD | BS_PUSHBUTTON, 206, 132, 170, 28,
            hwnd, (HMENU)IDC_COLOR, nullptr, nullptr));
        add(PAGE_WARP, CreateWindowW(L"STATIC", L"1x1 = single output: in-place warp / blend correction on the main screen.",
            WS_CHILD | SS_LEFT, 16, 176, 560, 18,
            hwnd, nullptr, nullptr, nullptr));

        // --- Image & Test page
        add(PAGE_IMAGE, g_ckTest = CreateWindowW(L"BUTTON", L"Test pattern",
            WS_CHILD | BS_AUTOCHECKBOX, 16, 44, 140, 24,
            hwnd, (HMENU)IDC_CKTEST, nullptr, nullptr));
        add(PAGE_IMAGE, g_ckAdv = CreateWindowW(L"BUTTON", L"Advanced blending",
            WS_CHILD | BS_AUTOCHECKBOX, 166, 44, 160, 24,
            hwnd, (HMENU)IDC_CKADV, nullptr, nullptr));
        add(PAGE_IMAGE, g_ckImage = CreateWindowW(L"BUTTON", L"Image source",
            WS_CHILD | BS_AUTOCHECKBOX, 336, 44, 120, 24,
            hwnd, (HMENU)IDC_CKIMAGE, nullptr, nullptr));
        add(PAGE_IMAGE, CreateWindowW(L"BUTTON", L"Load image...",
            WS_CHILD | BS_PUSHBUTTON, 462, 42, 96, 26,
            hwnd, (HMENU)IDC_LOADIMG, nullptr, nullptr));
        add(PAGE_IMAGE, CreateWindowW(L"BUTTON", L"Clear",
            WS_CHILD | BS_PUSHBUTTON, 564, 42, 56, 26,
            hwnd, (HMENU)IDC_CLEARIMG, nullptr, nullptr));
        add(PAGE_IMAGE, g_imgLabel = CreateWindowW(L"STATIC", L"Image: (none)",
            WS_CHILD | SS_LEFT, 16, 82, 320, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_IMAGE, CreateWindowW(L"STATIC", L"Patterns:",
            WS_CHILD | SS_LEFT, 346, 82, 62, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_IMAGE, g_cbImg = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            410, 78, 200, 200,
            hwnd, (HMENU)IDC_CBIMG, nullptr, nullptr));
        ComboBox_AddString(g_cbImg, L"Color bars");
        ComboBox_AddString(g_cbImg, L"Checkerboard");
        ComboBox_AddString(g_cbImg, L"Gray ramp");
        ComboBox_AddString(g_cbImg, L"Identity grid");

        // --- Settings page
        add(PAGE_SETTINGS, g_ckNet = CreateWindowW(L"BUTTON", L"TCP control (CMDNET)",
            WS_CHILD | BS_AUTOCHECKBOX, 16, 44, 190, 24,
            hwnd, (HMENU)IDC_CKNET, nullptr, nullptr));
        add(PAGE_SETTINGS, g_ckAuto = CreateWindowW(L"BUTTON", L"Start with Windows",
            WS_CHILD | BS_AUTOCHECKBOX, 216, 44, 170, 24,
            hwnd, (HMENU)IDC_CKAUTO, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"BUTTON", L"Save Config",
            WS_CHILD | BS_PUSHBUTTON, 400, 40, 100, 30,
            hwnd, (HMENU)IDC_SAVE, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"BUTTON", L"Load Config",
            WS_CHILD | BS_PUSHBUTTON, 506, 40, 100, 30,
            hwnd, (HMENU)IDC_LOAD, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"STATIC", L"Preset:",
            WS_CHILD | SS_LEFT, 16, 92, 52, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_SETTINGS, g_cbPreset = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWN | WS_VSCROLL,   // editable: type a new name then Save
            70, 88, 200, 220,
            hwnd, (HMENU)IDC_CBPRESET, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"BUTTON", L"Save",
            WS_CHILD | BS_PUSHBUTTON, 278, 86, 74, 26,
            hwnd, (HMENU)IDC_PRESETSAVE, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"BUTTON", L"Load",
            WS_CHILD | BS_PUSHBUTTON, 356, 86, 74, 26,
            hwnd, (HMENU)IDC_PRESETLOAD, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"BUTTON", L"Delete",
            WS_CHILD | BS_PUSHBUTTON, 434, 86, 90, 26,
            hwnd, (HMENU)IDC_PRESETDEL, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"STATIC", L"(CMDNET shared)",
            WS_CHILD | SS_LEFT, 530, 92, 90, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_SETTINGS, CreateWindowW(L"STATIC", L"Target:",
            WS_CHILD | SS_LEFT, 16, 128, 52, 18,
            hwnd, nullptr, nullptr, nullptr));
        add(PAGE_SETTINGS, g_cbTarget = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            70, 124, 200, 220,
            hwnd, (HMENU)IDC_CBTARGET, nullptr, nullptr));

        // --- bottom strip, always visible (mirrors the original's bottom bar)
        CreateWindowW(L"BUTTON", L"Configure alignment (F3)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 16, 416, 190, 30,
            hwnd, (HMENU)IDC_CALIB, nullptr, nullptr);
        g_ckEnable = CreateWindowW(L"BUTTON", L"Enable Desktop warping",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 216, 420, 190, 24,
            hwnd, (HMENU)IDC_CKENABLE, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Engine:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 420, 424, 44, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbEng = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            466, 420, 152, 220,
            hwnd, (HMENU)IDC_CBENG, nullptr, nullptr);
        ComboBox_AddString(g_cbEng, L"Desktop Duplication (DXGI)");
        ComboBox_AddString(g_cbEng, L"DWM Magnification (mirror)");
        ComboBox_AddString(g_cbEng, L"NVAPI clone (n/a)");
        ComboBox_AddString(g_cbEng, L"DOPP (n/a)");
        // structured status block (Projector / Mode / Color balancing / edges
        // / capture source / Engine fps+uptime), refreshed live by a 1s timer
        g_statusInfo = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 452, 612, 74,
            hwnd, (HMENU)IDC_STATUSINFO, nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 530, 612, 22,
            hwnd, nullptr, nullptr, nullptr);
        // shift every page control below the toolbar band (their creation
        // coordinates predate the toolbar; one loop beats thirty edits)
        for (int pg = 0; pg < PAGE_COUNT; pg++)
            for (HWND h : g_pages[pg]) {
                RECT rc{};
                GetWindowRect(h, &rc);
                MapWindowPoints(nullptr, hwnd, (POINT*)&rc, 2);
                SetWindowPos(h, nullptr, rc.left, rc.top + 38,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }

        applyDarkThemeRecursive(hwnd);   // DarkMode_Explorer on every child
        syncInplaceUI(hwnd);             // initial View-menu check state
        magSetConsoleWindow(hwnd);       // console excluded from the mag feed
        showTab(PAGE_DISPLAYS);
        SetTimer(hwnd, 1, 1000, nullptr);   // 1s: refresh Engine fps/uptime line
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            Button_SetCheck(g_ckTest, g_cfg.testPattern ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAdv, g_cfg.advancedBlend ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckEnable, g_cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckPreview, g_cfg.previewMode ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckInplace, g_cfg.inplace ? BST_CHECKED : BST_UNCHECKED);
            g_inplace.store(g_cfg.inplace);
            Button_SetCheck(g_ckImage, (g_cfg.useImage && engineHasImage()) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckNet, g_cfg.cmdnet ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAuto, queryAutostart() ? BST_CHECKED : BST_UNCHECKED);
            Engine::inst().setSampling(g_cfg.sampling);
            ComboBox_SetCurSel(g_cbSamp, g_cfg.sampling);
        }
        populateSourceCombo();   // locks g_cfgMutex itself
        syncLayoutCombos();
        refreshTargetComboUI();
        trayInit(hwnd);
        refreshPresetCombo();
        refreshStatusBar();
        return 0;
    }
    case WM_SRCSET: {   // capture source changed from the socket thread
        rebuildForSource();
        return 0;
    }
    case WM_INPLACESET: {   // in-place overlay toggled (UI or socket thread):
        // only the window style changes, but CreateWindowExW takes the full
        // ex-style, so rebuild the output windows (idempotent)
        if (!g_outWins.empty()) { destroyOutputWindows(); createOutputWindows(); }
        // checkbox + View menu follow whichever entry point toggled it
        syncInplaceUI(hwnd);
        return 0;
    }
    case WM_DISPLAYCHANGE:  // resolution change / monitor hotplug: count it
        g_dispChange++;     // here, do the heavy rebuild on the repost below
        PostMessageW(hwnd, WM_DISPCHANGE, wp, lp);
        return 0;
    case WM_DISPCHANGE: {   // deferred: outputs live on coordinates/sizes that
        // may no longer exist (DXGI duplications die on mode change anyway,
        // MAG hosts keep stale rects) - rebuild them while still running
        dbgLogLine("WM_DISPCHANGE: rebuilding outputs, dispChange=" +
                   std::to_string(g_dispChange.load()));
        if (!g_outWins.empty()) { destroyOutputWindows(); createOutputWindows(); }
        refreshStatusBar();
        setStatus(L"Display change: output windows rebuilt.");
        return 0;
    }
    case WM_MAGTEST: {      // test pattern toggled from the socket thread
        bool on;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            on = g_cfg.engine == 1 && g_cfg.testPattern != 0;
        }
        // one place syncs the checkbox for BOTH load paths (UI Load button and
        // remote LOAD) - applyLoadedConfig only invalidates the toolbar
        if (g_ckTest)
            Button_SetCheck(g_ckTest, g_cfg.testPattern ? BST_CHECKED : BST_UNCHECKED);
        syncMagTestOverlays(on);
        invalidateToolbarToggles();   // Test button checked frame
        return 0;
    }
    case WM_TGTREFRESH: {   // Target dropdown refresh marshalled from socket
        refreshTargetComboNow();
        return 0;
    }
    case WM_MAGBLEND: {     // MAG edge-blend overlays: create/remove/repaint
        syncMagBlendOverlays();
        return 0;
    }
    case WM_MAGPROBE: {     // probe markers toggled from the socket thread
        bool on;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            on = g_cfg.engine == 1;
        }
        syncMagProbeOverlays(on && g_probe.load() && !g_outWins.empty());
        return 0;
    }
    case WM_BLACKOUT: {     // blackout toggled from the socket thread
        syncBlackoutUI();
        return 0;
    }
    case WM_LAYSET: {   // tiled layout changed from the socket thread
        LayMsg m;
        {
            std::lock_guard<std::mutex> lk(g_layMsgMutex);
            m = g_layMsg;
        }
        applyLayoutUI(m.r, m.c, m.ov);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_EXIT) {   // toolbar Exit (id 1008: OUTSIDE the menu-id
            DestroyWindow(hwnd);   // range below - a range check would eat it)
            return 0;
        }
        if (id >= IDM_FILE_LOAD && id <= IDM_HELP_ABOUT) {
            // menu bar commands (HIWORD(wp) is 0 for menus, so these are
            // handled in their own branch before the control chain)
            if (id == IDM_FILE_LOAD) {
                setStatus(loadConfig(L"") ? L"Config loaded." : L"Load failed (no file?).");
                applyLoadedConfig();
            } else if (id == IDM_FILE_SAVE) {
                setStatus(saveConfig(L"") ? L"Config saved." : L"Save failed.");
            } else if (id == IDM_FILE_EXIT) {
                DestroyWindow(hwnd);
            } else if (id >= IDM_VIEW_DISP && id < IDM_VIEW_DISP + PAGE_COUNT) {
                int pg = id - IDM_VIEW_DISP;
                TabCtrl_SetCurSel(g_tab, pg);
                showTab(pg);
            } else if (id == IDM_VIEW_INPLACE) {
                toggleInplaceOverlay();
            } else if (id == IDM_VIEW_BLACKOUT) {
                g_blackout.store(!g_blackout.load());
                syncBlackoutUI(hwnd);
                setStatus(g_blackout.load() ? L"Blackout: ON (all outputs black)."
                                            : L"Blackout: off.");
            } else if (id == IDM_VIEW_HIDE) {
                ShowWindow(hwnd, SW_HIDE);
            } else if (id == IDM_WARP_ALIGN) {
                // toggle (F3 semantics): enter AND leave calibration; with the
                // in-place overlay on, leaving restores the click-through
                toggleCalibration();
            } else if (id == IDM_WARP_EDGES) {
                cycleBlendEdges();
            } else if (id == IDM_WARP_SNAP) {
                toggleSnapshot();
            } else if (id == IDM_WARP_TEST) {
                bool on = false;
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    g_cfg.testPattern = !g_cfg.testPattern;
                    on = g_cfg.testPattern != 0;
                    g_rev++;
                }
                Button_SetCheck(g_ckTest, on ? BST_CHECKED : BST_UNCHECKED);
                syncMagTestOverlays(on && g_cfg.engine == 1);
                invalidateToolbarToggles();   // Test button checked frame
                setStatus(on ? L"Test pattern ON." : L"Test pattern OFF.");
            } else if (id == IDM_HELP_ABOUT) {
                MessageBoxW(hwnd,
                    L"Immersive Warp Native\n"
                    L"Native reimplementation of Immersive Display PRO.\n\n"
                    L"Desktop Duplication capture, mesh warp, edge blending,\n"
                    L"masks, CMDNET show control on 127.0.0.1:5252.\n\n"
                    L"Hotkeys: F3 alignment, F10 warp/blend, Ctrl+F12 edges,\n"
                    L"Ctrl+F3 snapshot, F4 outputs on/off, Ctrl+F4 desktop warping,\n"
                    L"Ctrl+Shift+1..8 presets, Ctrl+F9 in-place overlay, Esc exit calibration.",
                    L"About Immersive Warp Native", MB_OK);
            }
            return 0;
        }
        if (id == IDC_TOGGLE && HIWORD(wp) == BN_CLICKED) {
            int sel = ListBox_GetCurSel(g_list);
            if (sel >= 0) {
                auto displays = enumerateDisplays();
                if (sel < (int)displays.size()) {
                    bool enabledNow = false;
                    {   // scoped lock: refreshList() below also locks g_cfgMutex,
                        // so it MUST be called after this block releases the lock
                        std::lock_guard<std::mutex> lk(g_cfgMutex);
                        OutputCfg* found = nullptr;
                        for (auto& o : g_cfg.outputs)
                            if (o.deviceName == displays[sel].deviceName) { found = &o; break; }
                        if (!found) {
                            g_cfg.outputs.emplace_back();
                            found = &g_cfg.outputs.back();
                            found->deviceName = displays[sel].deviceName;
                            found->label = displays[sel].label;
                            found->initIdentity(); // new entry needs identity grid
                        }
                        found->enabled = !found->enabled;
                        enabledNow = found->enabled;
                        g_rev++;
                    }
                    refreshList();
                    setStatus(enabledNow ? L"Display assigned as projector. Press Start Output."
                                         : L"Display unassigned.");
                }
            }
        } else if (id == IDC_START && HIWORD(wp) == BN_CLICKED) {
            createOutputWindows();
            setStatus(L"Output running. Press F3 / Calibration to align.");
        } else if (id == IDC_STOP && HIWORD(wp) == BN_CLICKED) {
            destroyOutputWindows();
            setStatus(L"Output stopped.");
        } else if (id == IDC_CALIB && HIWORD(wp) == BN_CLICKED) {
            if (g_outWins.empty()) createOutputWindows();
            toggleCalibration();
        } else if (id == IDC_SAVE && HIWORD(wp) == BN_CLICKED) {
            setStatus(saveConfig(L"") ? L"Config saved." : L"Save failed.");
        } else if (id == IDC_LOAD && HIWORD(wp) == BN_CLICKED) {
            setStatus(loadConfig(L"") ? L"Config loaded." : L"Load failed (no file?).");
            applyLoadedConfig();
        } else if (id == IDC_PRESETSAVE && HIWORD(wp) == BN_CLICKED) {
            wchar_t name[64] = L"";
            GetWindowTextW(g_cbPreset, name, 64);   // typed text or the selected item
            // only safe name characters (same rule as the CMDNET commands;
            // a typed name may contain anything, so re-check here)
            bool ok = name[0] && wcslen(name) <= 32;
            for (const wchar_t* p = name; *p; p++)
                if (!((*p >= L'A' && *p <= L'Z') || (*p >= L'0' && *p <= L'9') || *p == L'_' || *p == L'-'))
                    ok = false;
            if (!ok) { setStatus(L"Preset name: A-Z 0-9 _ - only (max 32)."); return 0; }
            setStatus(saveConfig(presetPath(toNarrowStr(name))) ? (std::wstring(L"Preset '") + name + L"' saved.").c_str()
                                                               : L"Save failed.");
            refreshPresetCombo();
        } else if (id == IDC_PRESETLOAD && HIWORD(wp) == BN_CLICKED) {
            wchar_t name[64] = L"";
            GetWindowTextW(g_cbPreset, name, 64);
            if (!name[0]) { setStatus(L"No preset selected."); return 0; }
            if (loadConfig(presetPath(toNarrowStr(name)))) {
                applyLoadedConfig();
                setStatus((std::wstring(L"Preset '") + name + L"' loaded.").c_str());
            } else {
                setStatus(L"Load failed (no such preset?).");
            }
        } else if (id == IDC_PRESETDEL && HIWORD(wp) == BN_CLICKED) {
            wchar_t name[64] = L"";
            GetWindowTextW(g_cbPreset, name, 64);
            if (!name[0]) { setStatus(L"No preset selected."); return 0; }
            setStatus(DeleteFileW(presetPath(toNarrowStr(name)).c_str()) ? (std::wstring(L"Preset '") + name + L"' deleted.").c_str()
                                                                 : L"Delete failed.");
            refreshPresetCombo();
        } else if (id == IDC_CKENABLE && HIWORD(wp) == BN_CLICKED) {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.enabled = Button_GetCheck(g_ckEnable) == BST_CHECKED;
            g_rev++;
        } else if (id == IDC_CKTEST && HIWORD(wp) == BN_CLICKED) {
            bool on;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.testPattern = Button_GetCheck(g_ckTest) == BST_CHECKED;
                on = g_cfg.engine == 1 && g_cfg.testPattern != 0;
                g_rev++;
            }
            syncMagTestOverlays(on);   // no-op outside engine=1
            invalidateToolbarToggles();   // Test button checked frame
        } else if (id == IDC_CKADV && HIWORD(wp) == BN_CLICKED) {
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.advancedBlend = Button_GetCheck(g_ckAdv) == BST_CHECKED;
                g_rev++;
            }
            // MAG blend overlays change appearance with/without the advanced
            // curves - resync them (no-op outside engine=1)
            requestMagBlendSync();
        } else if (id == IDC_CKPREVIEW && HIWORD(wp) == BN_CLICKED) {
            bool on = Button_GetCheck(g_ckPreview) == BST_CHECKED;
            g_previewMode.store(on);
            if (!on) g_previewNoCapture.store(false);
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.previewMode = on;
                g_rev++;
            }
            // the capture source depends on this mode, so rebuild the outputs
            bool running = !g_outWins.empty();
            if (running) { destroyOutputWindows(); createOutputWindows(); }
            setStatus(on ? L"Preview: 60% window, live desktop warp on a single screen. Drag the window to move it."
                         : L"Preview off: fullscreen output.");
        } else if (id == IDC_CKINPLACE && HIWORD(wp) == BN_CLICKED) {
            // single source of truth: the checkbox click feeds the same
            // toggle the Ctrl+F9 hotkey / menus use (no-op when already there)
            bool on = Button_GetCheck(g_ckInplace) == BST_CHECKED;
            if (g_inplace.load() != on) toggleInplaceOverlay();
        } else if (id == IDC_CKIMAGE && HIWORD(wp) == BN_CLICKED) {
            bool on = Button_GetCheck(g_ckImage) == BST_CHECKED;
            if (on && !engineHasImage()) {
                Button_SetCheck(g_ckImage, BST_UNCHECKED);
                setStatus(L"No image loaded yet - click \"Load image...\" first.");
                return 0;
            }
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.useImage = on;
                g_rev++;
            }
            setStatus(on ? L"Image source active." : L"Image source off (desktop capture / test pattern).");
        } else if (id == IDC_LOADIMG && HIWORD(wp) == BN_CLICKED) {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return 0;
            if (!engineLoadImage(file)) { setStatus(L"Image load failed (unsupported or unreadable file)."); return 0; }
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.useImage = true;
                g_cfg.imagePath = file;
                g_cfg.testImg = -1;   // file image replaces built-in pattern
                g_rev++;
            }
            Button_SetCheck(g_ckImage, BST_CHECKED);
            ComboBox_SetCurSel(g_cbImg, -1);
            {
                const wchar_t* base = file;
                for (const wchar_t* p = file; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
                wchar_t line[512];
                wsprintfW(line, L"Image: %s  (%d x %d)", base,
                          Engine::inst().imageWidth(), Engine::inst().imageHeight());
                SetWindowTextW(g_imgLabel, line);
            }
            setStatus(L"Image loaded and set as source.");
        } else if (id == IDC_CLEARIMG && HIWORD(wp) == BN_CLICKED) {
            engineLoadImage(L"");
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.useImage = false;
                g_cfg.imagePath.clear();
                g_cfg.testImg = -1;
                g_rev++;
            }
            Button_SetCheck(g_ckImage, BST_UNCHECKED);
            ComboBox_SetCurSel(g_cbImg, -1);
            SetWindowTextW(g_imgLabel, L"Image: (none)");
            setStatus(L"Image cleared.");
        } else if (id == IDC_CBIMG && HIWORD(wp) == CBN_SELCHANGE) {
            int idx = ComboBox_GetCurSel(g_cbImg);
            if (idx >= 0 && engineSetTestImage(idx)) {
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    g_cfg.useImage = true;
                    g_cfg.testImg = idx;
                    g_rev++;
                }
                Button_SetCheck(g_ckImage, BST_CHECKED);
                static const wchar_t* patName[4] = { L"Color bars", L"Checkerboard", L"Gray ramp", L"Identity grid" };
                wchar_t line[160];
                wsprintfW(line, L"Image: %s (built-in %dx%d)", patName[idx],
                          Engine::inst().imageWidth(), Engine::inst().imageHeight());
                SetWindowTextW(g_imgLabel, line);
                setStatus(L"Built-in test image set as source.");
            }
        } else if (id == IDC_COLOR && HIWORD(wp) == BN_CLICKED) {
            openColorDialog(GetModuleHandleW(nullptr), hwnd);
        } else if (id == IDC_CFGDLG && HIWORD(wp) == BN_CLICKED) {
            openConfigDialogInternal(GetModuleHandleW(nullptr));
        } else if (id == IDC_EDGES && HIWORD(wp) == BN_CLICKED) {
            cycleBlendEdges();
        } else if (id == IDC_CKNET && HIWORD(wp) == BN_CLICKED) {
            bool on = Button_GetCheck((HWND)lp) == BST_CHECKED;
            g_cfg.cmdnet = on;
            if (on) cmdnetStart(g_cfg.cmdnetPort); else cmdnetStop();
            setStatus(on ? L"TCP control: listening on 127.0.0.1 (CMDNET)."
                         : L"TCP control: off.");
        } else if (id == IDC_CKAUTO && HIWORD(wp) == BN_CLICKED) {
            bool on = Button_GetCheck((HWND)lp) == BST_CHECKED;
            setAutostart(on);
            setStatus(on ? L"Autostart: registered (HKCU Run)."
                         : L"Autostart: removed.");
        } else if (id == IDC_CBSAMP && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(g_cbSamp);
            if (sel >= 0) {
                { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.sampling = sel; }
                Engine::inst().setSampling(sel);
                setStatus(sel == 0 ? L"Sampling: trilinear."
                         : sel == 3 ? L"Sampling: bi-cubic."
                                    : L"Sampling: anisotropic.");
            }
        } else if (id == IDC_CBSRC && HIWORD(wp) == CBN_SELCHANGE) {
            if (g_srcBusy) return 0;   // reentrancy guard (combo repopulation)
            int sel = ComboBox_GetCurSel(g_cbSrc);
            std::wstring dev;
            if (sel > 0 && sel < (int)g_srcDevs.size()) dev = g_srcDevs[sel];
            wchar_t msg[160];
            if (dev.empty()) lstrcpyW(msg, L"Capture source: auto (first display that is not an output).");
            else wsprintfW(msg, L"Capture source pinned to %s (outputs rebuilt).", dev.c_str());
            setStatus(msg);
            setSourceDisplay(dev);
        } else if (id == IDC_CBLAYOUT && HIWORD(wp) == CBN_SELCHANGE) {
            wchar_t t[8] = L"1x1";
            ComboBox_GetLBText(g_cbLayout, ComboBox_GetCurSel(g_cbLayout), t);
            int C = t[0] - L'0', R = t[2] - L'0', ov;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                ov = g_cfg.overlap;
            }
            applyLayoutUI(R, C, ov);
            wchar_t msg[96];
            wsprintfW(msg, L"Layout %dx%d applied (%d projectors): warp grids rebuilt.",
                      C, R, R * C);
            setStatus(msg);
        } else if (id == IDC_CBOVL && HIWORD(wp) == CBN_SELCHANGE) {
            int R, C;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                R = g_cfg.layoutR; C = g_cfg.layoutC;
            }
            int ov = ComboBox_GetCurSel(g_cbOvl) * 5;
            applyLayoutUI(R, C, ov);
            wchar_t msg[96];
            wsprintfW(msg, L"Tile overlap %d%% applied: warp grids rebuilt.", ov);
            setStatus(msg);
        } else if (id == IDC_CBENG && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(g_cbEng);
            if (sel != 0 && sel != 1) {
                // remaining entries are placeholders
                ComboBox_SetCurSel(g_cbEng, g_cfg.engine == 1 ? 1 : 0);
                setStatus(L"Only Desktop Duplication and DWM Magnification are available in this build.");
            } else if (sel >= 0 && sel != g_cfg.engine) {
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    g_cfg.engine = sel;
                    g_rev++;
                }
                // engine change means a different kind of output window:
                // rebuild (idempotent - handles both DXGI and MAG paths)
                destroyOutputWindows();
                createOutputWindows();
                setStatus(sel == 1
                    ? L"Engine: DWM Magnification (1:1 desktop mirror, no mesh warp)."
                    : L"Engine: Desktop Duplication (warp + blend pipeline).");
            }
        } else if (id == IDC_CBTARGET && HIWORD(wp) == CBN_SELCHANGE) {
            // user picked a command target -> share it with CMDNET (SET out=)
            if (!g_tgtBusy && g_cbTarget) {
                int sel = ComboBox_GetCurSel(g_cbTarget);
                if (sel >= 0 && sel < (int)g_tgtShort.size()) {
                    cmdSetTarget(g_tgtShort[sel]);
                    std::string t = cmdGetTarget();
                    std::wstring w(t.begin(), t.end());
                    wchar_t msg[128];
                    wsprintfW(msg, L"Command target: %s", w.empty() ? L"AUTO" : w.c_str());
                    setStatus(msg);
                    refreshStatusBar();
                }
            }
        } else if (id >= IDM_TRAY_PRESET && id < IDM_TRAY_PRESET + TRAY_PRESET_MAX) {
            std::wstring nm;
            if (presetNameByIndex(id - IDM_TRAY_PRESET, nm) &&
                loadConfig(presetPath(toNarrowStr(nm.c_str())))) {
                applyLoadedConfig();
                wchar_t msg[96];
                wsprintfW(msg, L"Preset '%s' loaded (tray).", nm.c_str());
                setStatus(msg);
            } else {
                setStatus(L"Preset load failed.");
            }
        } else if (id == IDM_TRAY_SHOW) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        } else if (id == IDM_TRAY_START) {
            createOutputWindows();
        } else if (id == IDM_TRAY_STOP) {
            destroyOutputWindows();
        } else if (id == IDM_TRAY_INPLACE) {
            toggleInplaceOverlay();
        } else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F3) {
            if (g_outWins.empty()) createOutputWindows();
            toggleCalibration();
        } else if (wp == VK_F12) {
            cycleBlendEdges();     // console has focus: plain F12 works here
        } else if (wp == VK_F10) {
            switchSubModeAll();
        }
        return 0;
    case WM_NOTIFY: {                  // main-console tab strip
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) {
            showTab(TabCtrl_GetCurSel(g_tab));
            return 0;
        }
        break;
    }
    case WM_CALIBSET: {   // calibration on/off from the socket thread:
        // the state atom is already set; only the hotkeys (and the UI sync)
        // must run here, on the thread that owns the message loop
        calibrationHotkeysSync(g_calib.load());
        refreshInplaceStyles();
        refreshStatusBar();
        return 0;
    }
    case WM_TIMER: {                   // 1s status refresh (fps/uptime live)
        if (wp == 1 && g_statusInfo) {
            refreshStatusBar();
            // optional perf history: one snapshot line every 10 s so a real
            // machine run leaves a timing trail in debug.log
            static unsigned perfTick = 0;
            if (g_perfLog.load() && ++perfTick >= 10) {
                perfTick = 0;
                char b[160];
                _snprintf_s(b, _TRUNCATE,
                            "perf: fps=%.1f cap=%.2fms draw=%.2fms frame=%.2fms frames=%llu",
                            engineFps(), engineCapMs(), engineDrawMs(),
                            engineFrameMs(), engineFrames());
                dbgLogLine(b);
            }
        }
        return 0;
    }
    case WM_DRAWITEM: {                // owner-draw: schematic + toolbar glyphs
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        if (dis && dis->CtlID == IDC_SCHEMATIC) { drawDisplaySchematic(dis); return TRUE; }
        if (dis && isToolbarCmd((int)dis->CtlID)) { drawToolbarButton(dis); return TRUE; }
        break;
    }
    case WM_CTLCOLORSTATIC: {          // dark theme: dark panels, light text,
        HDC dc = (HDC)wp;              // orange accent on the status line
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (HWND)lp == g_status ? RGB(232, 148, 10) : RGB(224, 224, 224));
        static HBRUSH darkBrush = CreateSolidBrush(RGB(30, 30, 34));
        return (LRESULT)darkBrush;
    }
    case WM_CTLCOLORDLG: {
        static HBRUSH dlgBrush = CreateSolidBrush(RGB(30, 30, 34));
        return (LRESULT)dlgBrush;
    }
    case WM_CTLCOLOREDIT:              // combobox edit field
    case WM_CTLCOLORLISTBOX: {         // combobox dropdown / output list
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(44, 44, 50));
        SetTextColor(dc, RGB(224, 224, 224));
        static HBRUSH lbBrush = CreateSolidBrush(RGB(44, 44, 50));
        return (LRESULT)lbBrush;
    }
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP) {          // left click: toggle console
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        } else if (lp == WM_RBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            showTrayMenu(hwnd);
        }
        return 0;
    case WM_CLOSE:
        // the console closes to the tray like the original; Exit is in the
        // tray menu (or the Exit button) so a stray click never kills output
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_ctl = nullptr;
        KillTimer(hwnd, 1);
        trayRemove();
        UnregisterHotKey(nullptr, 1);
        UnregisterHotKey(nullptr, 2);
        UnregisterHotKey(nullptr, 3);
        UnregisterHotKey(nullptr, 4);
        UnregisterHotKey(nullptr, 5);
        UnregisterHotKey(nullptr, 6);
        UnregisterHotKey(nullptr, 7);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerAndCreateControlWindow(HINSTANCE hInst) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ControlWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CTL_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    uiThreadId = GetCurrentThreadId();
    dbgLogLine("uiThread tid=" + std::to_string((long)uiThreadId));   // hotkey test hook
    g_ctl = CreateWindowExW(WS_EX_TOPMOST, CTL_CLASS, L"Immersive Warp Native",
        WS_OVERLAPPEDWINDOW, 100, 100, 650, 646,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_ctl, SW_SHOW);
    UpdateWindow(g_ctl);
    showTab(PAGE_DISPLAYS);   // now that g_ctl is set: mark the View menu item
    refreshList();
    // global hotkeys so calibration keys work no matter which window has focus
    RegisterHotKey(nullptr, 1, 0, VK_F3);            // enter/exit calibration
    RegisterHotKey(nullptr, 2, 0, VK_F10);           // warp <-> blend mode
    // Plain F12 is reserved by Windows for the debugger (RegisterHotKey fails),
    // so the global binding is Ctrl+F12; plain F12 is also handled in
    // ControlWndProc::WM_KEYDOWN when the console has focus.
    RegisterHotKey(nullptr, 4, MOD_CONTROL, VK_F12);  // cycle blended edges
    RegisterHotKey(nullptr, 5, MOD_CONTROL, VK_F3);   // snapshot (freeze capture)
    RegisterHotKey(nullptr, 7, MOD_CONTROL, VK_F9);   // in-place overlay switch
    RegisterHotKey(nullptr, 8, 0, VK_F4);             // window warping on/off
    RegisterHotKey(nullptr, 9, MOD_CONTROL, VK_F4);   // desktop warping (in-place overlay)
    for (int i = 0; i < 8; i++)                       // Ctrl+Shift+1..8: presets by name order
        RegisterHotKey(nullptr, 10 + i, MOD_CONTROL | MOD_SHIFT, L'1' + i);
}

// GET_X_LPARAM / GET_Y_LPARAM come from windowsx.h (included above)
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
