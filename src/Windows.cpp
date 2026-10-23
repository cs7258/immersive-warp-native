#include "Common.h"
#include "Engine.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

// ------------------------------------------------------------------
// Output windows (fullscreen per display) + calibration interaction
// ------------------------------------------------------------------
struct OutWinState {
    int displayIdx = -1;
    int subMode = 0;              // 0 = geometrical correction, 1 = edge blending
    int dragKind = -1;            // -1 none, 0 warp point, 1 simple blend side, 2 advanced point, 3 window move
    int dragIdx = -1;             // warp point index or blend side index
    int advIdx = -1;              // advanced point index
    float lastX = 0, lastY = 0;
    int moveOffX = 0, moveOffY = 0;   // preview window drag offset
};
static const wchar_t* OUT_CLASS = L"IWN_OutputWnd";
static std::vector<std::pair<HWND, OutWinState*>> g_outWins;

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
    case WM_LBUTTONUP:
        if (st) { st->dragKind = -1; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!g_calib.load() || !st || st->dragKind < 1) break;
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
            if (g_outWins[i].first == hwnd) { delete g_outWins[i].second; g_outWins.erase(g_outWins.begin() + i); break; }
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

    auto displays = enumerateDisplays();
    dbgLogLine("createOutputWindows: displays=" + std::to_string(displays.size()) +
               " preview=" + std::to_string((int)g_previewMode.load()) +
               " tid=" + std::to_string((long)GetCurrentThreadId()));
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    int n = 0;
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
        RECT r = displays[i].desktop;
        bool preview = g_previewMode.load();
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
        // console and never steals keyboard focus (hotkeys handle F3/F10/Esc)
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, OUT_CLASS,
            cfg->label.c_str(), WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (preview) {
            // 0x11 = WDA_EXCLUDEFROMCAPTURE (Win10 2004+), 0x01 = WDA_MONITOR
            typedef BOOL (WINAPI *SetAffFn)(HWND, DWORD);
            auto fn = (SetAffFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
            BOOL ok = fn ? fn(hwnd, 0x11) : FALSE;
            if (!ok && fn) ok = fn(hwnd, 0x01);
            g_previewNoCapture.store(!ok);
            dbgLogLine(std::string("preview: window ") + (ok ? "excluded" : "NOT excluded") +
                       " from capture, rect=" + std::to_string(r.left) + "," + std::to_string(r.top) +
                       " " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top));
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        g_outWins.push_back({ hwnd, st });
        Engine::inst().attachOutput((int)i, hwnd);
    }
    // console always stays on top of the output windows (like the original)
    notifyOutputRaised();
}

void destroyOutputWindows() {
    dbgLogLine("destroyOutputWindows: " + std::to_string(g_outWins.size()) + " window(s)");
    for (auto& p : g_outWins) DestroyWindow(p.first);
    g_outWins.clear();
    Engine::inst().detachAll();
}

// ------------------------------------------------------------------
// Control window
// ------------------------------------------------------------------
static const wchar_t* CTL_CLASS = L"IWN_ControlWnd";
static HWND g_ctl = nullptr;
static HWND g_list = nullptr, g_status = nullptr, g_statusInfo = nullptr;
static HWND g_ckEnable = nullptr, g_ckTest = nullptr, g_ckAdv = nullptr;
static HWND g_ckPreview = nullptr, g_ckImage = nullptr, g_imgLabel = nullptr;
static HWND g_ckNet = nullptr;
static HWND g_ckAuto = nullptr, g_cbSamp = nullptr;
static HWND g_cbLayout = nullptr, g_cbOvl = nullptr, g_cbEng = nullptr;
static HWND g_cbSrc = nullptr;
// capture source combo: index 0 = auto, index i>0 = g_srcDevs[i]
static std::vector<std::wstring> g_srcDevs;

// tray ids must not collide with control ids 1001..1021
#define IDM_TRAY_SHOW  2001
#define IDM_TRAY_START 2002
#define IDM_TRAY_STOP  2003
#define IDM_TRAY_EXIT  2004

// cross-thread capture-source change (CMDNET runs on the socket thread).
// constexpr so it can be used as a switch case label.
constexpr UINT WM_SRCSET = WM_APP + 9;
constexpr UINT WM_LAYSET = WM_APP + 10;
static DWORD uiThreadId = 0;
constexpr UINT WM_TRAYICON = WM_APP + 7;
static std::mutex g_layMsgMutex;
struct LayMsg { int r = 0, c = 0, ov = 0; };
static LayMsg g_layMsg;

// ---------------- tray icon ----------------
static NOTIFYICONDATAW g_nid{};
static bool g_trayOn = false;

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
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
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
    wchar_t l1[128], l2[160], l3[192], l4[128], l5[160];
    // [n] = which projector is being edited (1-based). refreshList() resets the
    // listbox selection, so fall back to 1 instead of showing [0].
    int cur = (sel >= 0 && sel < nProj) ? sel + 1 : (nProj > 0 ? 1 : 0);
    wsprintfW(l1, L"Projector: %d [%d]%s", nProj, cur,
              g_previewMode.load() ? L"   Preview: ON" : L"");
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

    wchar_t all[800];
    wsprintfW(all, L"%s\r\n%s\r\n%s\r\n%s\r\n%s", l1, l2, l3, l4, l5);
    SetWindowTextW(g_statusInfo, all);
}

void setCalibrationMode(bool on) {
    bool was = g_calib.exchange(on);
    if (was == on) return;
    // Esc is captured globally only while calibrating, so it never
    // interferes with normal Escape usage in other apps
    if (on) {
        RegisterHotKey(nullptr, 3, 0, VK_ESCAPE);
        // D opens the advanced RGB dialog, exactly like the original. Only
        // registered during calibration so normal typing never hits it.
        RegisterHotKey(nullptr, 6, 0, 'D');
    } else {
        UnregisterHotKey(nullptr, 3);
        UnregisterHotKey(nullptr, 6);
    }
    refreshStatusBar();
}

void toggleCalibration() {
    if (g_calib.load()) {
        setCalibrationMode(false);
        setStatus(L"Calibration OFF.");
    } else {
        if (g_outWins.empty()) createOutputWindows();
        setCalibrationMode(true);
        setStatus(L"Calibration ON: drag green points (warp), F10 = blend mode, Esc = exit.");
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
        if (R == 1 && C == 1) o.edges = 15;   // no layout: keep all edges on
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
    if (applyLayoutValue(R, C, ov)) syncLayoutCombos();
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
            wsprintfW(t, L"#%d  X %.1f%%  Y %.1f%%  %.1f%% x %.1f%%", idx++,
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
            cfgReloadBlend();
            refreshStatusBar();
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

static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            10, 10, 420, 200, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Toggle selected as projector",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 220, 200, 30,
            hwnd, (HMENU)IDC_TOGGLE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Start Output",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 220, 220, 100, 30,
            hwnd, (HMENU)IDC_START, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Stop Output",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 330, 220, 100, 30,
            hwnd, (HMENU)IDC_STOP, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Configure alignment (F3)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 260, 200, 30,
            hwnd, (HMENU)IDC_CALIB, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Save Config",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 220, 260, 100, 30,
            hwnd, (HMENU)IDC_SAVE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Load Config",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 330, 260, 100, 30,
            hwnd, (HMENU)IDC_LOAD, nullptr, nullptr);
        g_ckEnable = CreateWindowW(L"BUTTON", L"Enable Desktop warping",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 300, 220, 24,
            hwnd, (HMENU)IDC_CKENABLE, nullptr, nullptr);
        g_ckTest = CreateWindowW(L"BUTTON", L"Test pattern",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 240, 300, 140, 24,
            hwnd, (HMENU)IDC_CKTEST, nullptr, nullptr);
        g_ckAdv = CreateWindowW(L"BUTTON", L"Advanced blending",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 390, 300, 160, 24,
            hwnd, (HMENU)IDC_CKADV, nullptr, nullptr);
        g_ckPreview = CreateWindowW(L"BUTTON", L"Windowed preview (single screen)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 330, 240, 24,
            hwnd, (HMENU)IDC_CKPREVIEW, nullptr, nullptr);
        g_ckImage = CreateWindowW(L"BUTTON", L"Image source",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 250, 330, 120, 24,
            hwnd, (HMENU)IDC_CKIMAGE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Load image...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 375, 330, 100, 24,
            hwnd, (HMENU)IDC_LOADIMG, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 330, 60, 24,
            hwnd, (HMENU)IDC_CLEARIMG, nullptr, nullptr);
        g_imgLabel = CreateWindowW(L"STATIC", L"Image: (none)",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 362, 245, 18,
            hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Patterns:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 262, 362, 62, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbImg = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            326, 358, 214, 200,
            hwnd, (HMENU)IDC_CBIMG, nullptr, nullptr);
        ComboBox_AddString(g_cbImg, L"Color bars");
        ComboBox_AddString(g_cbImg, L"Checkerboard");
        ComboBox_AddString(g_cbImg, L"Gray ramp");
        ComboBox_AddString(g_cbImg, L"Identity grid");
        CreateWindowW(L"BUTTON", L"Color adjustment...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 388, 170, 28,
            hwnd, (HMENU)IDC_COLOR, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Configure display...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 190, 388, 180, 28,
            hwnd, (HMENU)IDC_CFGDLG, nullptr, nullptr);
        g_ckNet = CreateWindowW(L"BUTTON", L"TCP control (CMDNET)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 385, 392, 190, 24,
            hwnd, (HMENU)IDC_CKNET, nullptr, nullptr);
        g_ckAuto = CreateWindowW(L"BUTTON", L"Start with Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 455, 420, 170, 24,
            hwnd, (HMENU)IDC_CKAUTO, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Sampling:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 424, 62, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbSamp = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            76, 420, 150, 200,
            hwnd, (HMENU)IDC_CBSAMP, nullptr, nullptr);
        ComboBox_AddString(g_cbSamp, L"Trilinear");
        ComboBox_AddString(g_cbSamp, L"Anisotropic 4x");
        ComboBox_AddString(g_cbSamp, L"Anisotropic 8x");
        // NOTE: this used to be placed at (190,388) where it sat exactly on top
        // of the "TCP control (CMDNET)" checkbox and was therefore invisible.
        CreateWindowW(L"BUTTON", L"Cycle blend edges (Ctrl+F12)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 240, 420, 200, 28,
            hwnd, (HMENU)IDC_EDGES, nullptr, nullptr);
        // capture source: which desktop gets warped (auto = first non-output)
        CreateWindowW(L"STATIC", L"Source:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 458, 54, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbSrc = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            66, 454, 300, 220,
            hwnd, (HMENU)IDC_CBSRC, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"(auto = first display that is not an output)",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 374, 458, 200, 32,
            hwnd, nullptr, nullptr, nullptr);
        // NxN tiled layout + overlap + warp engine
        CreateWindowW(L"STATIC", L"Layout:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 494, 50, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbLayout = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            62, 490, 74, 220,
            hwnd, (HMENU)IDC_CBLAYOUT, nullptr, nullptr);
        for (int R = 1; R <= 4; R++)
            for (int C = 1; C <= 4; C++)
                if (R * C > 1) {
                    wchar_t t[8];
                    _snwprintf_s(t, _TRUNCATE, L"%dx%d", C, R);
                    ComboBox_AddString(g_cbLayout, t);
                }
        CreateWindowW(L"STATIC", L"Overlap:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 148, 494, 56, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbOvl = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            206, 490, 66, 220,
            hwnd, (HMENU)IDC_CBOVL, nullptr, nullptr);
        for (int p = 0; p <= 40; p += 5) {
            wchar_t t[8];
            wsprintfW(t, L"%d%%", p);
            ComboBox_AddString(g_cbOvl, t);
        }
        CreateWindowW(L"STATIC", L"Engine:",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 286, 494, 48, 18,
            hwnd, nullptr, nullptr, nullptr);
        g_cbEng = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            336, 490, 238, 220,
            hwnd, (HMENU)IDC_CBENG, nullptr, nullptr);
        ComboBox_AddString(g_cbEng, L"Desktop Duplication (DXGI)");
        ComboBox_AddString(g_cbEng, L"DWM Magnification (n/a)");
        ComboBox_AddString(g_cbEng, L"NVAPI clone (n/a)");
        ComboBox_AddString(g_cbEng, L"DOPP (n/a)");
        // structured status block (Projector / Mode / Color balancing / edges)
        g_statusInfo = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 526, 620, 76,
            hwnd, nullptr, nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 598, 620, 40,
            hwnd, nullptr, nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            Button_SetCheck(g_ckTest, g_cfg.testPattern ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAdv, g_cfg.advancedBlend ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckEnable, g_cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckPreview, g_cfg.previewMode ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckImage, (g_cfg.useImage && engineHasImage()) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckNet, g_cfg.cmdnet ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAuto, queryAutostart() ? BST_CHECKED : BST_UNCHECKED);
            Engine::inst().setSampling(g_cfg.sampling);
            ComboBox_SetCurSel(g_cbSamp, g_cfg.sampling);
        }
        populateSourceCombo();   // locks g_cfgMutex itself
        syncLayoutCombos();
        trayInit(hwnd);
        refreshStatusBar();
        return 0;
    }
    case WM_SRCSET: {   // capture source changed from the socket thread
        rebuildForSource();
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
            setCalibrationMode(true);
            setStatus(L"Calibration ON: drag green points (warp), F10 = blend mode, Esc = exit.");
        } else if (id == IDC_SAVE && HIWORD(wp) == BN_CLICKED) {
            setStatus(saveConfig(L"") ? L"Config saved." : L"Save failed.");
        } else if (id == IDC_LOAD && HIWORD(wp) == BN_CLICKED) {
            setStatus(loadConfig(L"") ? L"Config loaded." : L"Load failed (no file?).");
            refreshList();
            // config may have changed preview/image state -> resync the UI
            g_previewMode.store(g_cfg.previewMode);
            Button_SetCheck(g_ckPreview, g_cfg.previewMode ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckImage, (g_cfg.useImage && engineHasImage()) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAuto, queryAutostart() ? BST_CHECKED : BST_UNCHECKED);
            Engine::inst().setSampling(g_cfg.sampling);
            ComboBox_SetCurSel(g_cbSamp, g_cfg.sampling);
            ComboBox_SetCurSel(g_cbImg, g_cfg.testImg);
            syncLayoutCombos();
            if (!g_cfg.imagePath.empty() && engineLoadImage(g_cfg.imagePath)) {
                const wchar_t* base = g_cfg.imagePath.c_str();
                for (const wchar_t* p = g_cfg.imagePath.c_str(); *p; p++)
                    if (*p == L'\\' || *p == L'/') base = p + 1;
                wchar_t line[512];
                wsprintfW(line, L"Image: %s  (%d x %d)", base,
                          Engine::inst().imageWidth(), Engine::inst().imageHeight());
                SetWindowTextW(g_imgLabel, line);
            }
        } else if (id == IDC_CKENABLE && HIWORD(wp) == BN_CLICKED) {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.enabled = Button_GetCheck(g_ckEnable) == BST_CHECKED;
            g_rev++;
        } else if (id == IDC_CKTEST && HIWORD(wp) == BN_CLICKED) {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.testPattern = Button_GetCheck(g_ckTest) == BST_CHECKED;
            g_rev++;
        } else if (id == IDC_CKADV && HIWORD(wp) == BN_CLICKED) {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.advancedBlend = Button_GetCheck(g_ckAdv) == BST_CHECKED;
            g_rev++;
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
            if (sel != 0) {
                // placeholders: only Desktop Duplication exists in this build
                ComboBox_SetCurSel(g_cbEng, 0);
                setStatus(L"Only Desktop Duplication is available in this build.");
            }
        } else if (id == IDM_TRAY_SHOW) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        } else if (id == IDM_TRAY_START) {
            createOutputWindows();
        } else if (id == IDM_TRAY_STOP) {
            destroyOutputWindows();
        } else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F3) {
            if (g_outWins.empty()) createOutputWindows();
            setCalibrationMode(true);
            setStatus(L"Calibration ON.");
        } else if (wp == VK_F12) {
            cycleBlendEdges();     // console has focus: plain F12 works here
        } else if (wp == VK_F10) {
            switchSubModeAll();
        }
        return 0;
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
        trayRemove();
        UnregisterHotKey(nullptr, 1);
        UnregisterHotKey(nullptr, 2);
        UnregisterHotKey(nullptr, 3);
        UnregisterHotKey(nullptr, 4);
        UnregisterHotKey(nullptr, 5);
        UnregisterHotKey(nullptr, 6);
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
    g_ctl = CreateWindowExW(WS_EX_TOPMOST, CTL_CLASS, L"Immersive Warp Native",
        WS_OVERLAPPEDWINDOW, 100, 100, 650, 700,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_ctl, SW_SHOW);
    UpdateWindow(g_ctl);
    refreshList();
    // global hotkeys so calibration keys work no matter which window has focus
    RegisterHotKey(nullptr, 1, 0, VK_F3);            // enter/exit calibration
    RegisterHotKey(nullptr, 2, 0, VK_F10);           // warp <-> blend mode
    // Plain F12 is reserved by Windows for the debugger (RegisterHotKey fails),
    // so the global binding is Ctrl+F12; plain F12 is also handled in
    // ControlWndProc::WM_KEYDOWN when the console has focus.
    RegisterHotKey(nullptr, 4, MOD_CONTROL, VK_F12);  // cycle blended edges
    RegisterHotKey(nullptr, 5, MOD_CONTROL, VK_F3);   // snapshot (freeze capture)
}

// GET_X_LPARAM / GET_Y_LPARAM come from windowsx.h (included above)
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
