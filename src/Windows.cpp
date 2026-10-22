#include "Common.h"
#include "Engine.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
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
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (auto* cfg = cfgForWindowNoLock(st)) {
            if (st->dragKind == 1) {
                auto& b = cfg->blend[st->dragIdx];
                b.gamma = clampf(b.gamma + dir, 0.1f, 6.0f);
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
    WNDCLASSW wc{};
    wc.lpfnWndProc = OutputWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = OUT_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    auto displays = enumerateDisplays();
    dbgLogLine("createOutputWindows: displays=" + std::to_string(displays.size()) +
               " preview=" + std::to_string((int)g_previewMode.load()));
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
    bool snapshot = g_snapshot.load();
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        for (auto& o : g_cfg.outputs) {
            if (!o.enabled) continue;
            if (nProj == 0) { c = o.color; edges = o.edges; }
            nProj++;
        }
    }
    wchar_t l1[128], l2[160], l3[192], l4[128];
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

    wchar_t all[600];
    wsprintfW(all, L"%s\r\n%s\r\n%s\r\n%s", l1, l2, l3, l4);
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
    setStatus(m == 1 ? L"Mode: Edge blending (red = simple handles, blue = advanced). F10 to switch back."
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
        // structured status block (Projector / Mode / Color balancing / edges)
        g_statusInfo = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 420, 545, 76,
            hwnd, nullptr, nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 496, 545, 40,
            hwnd, nullptr, nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            Button_SetCheck(g_ckTest, g_cfg.testPattern ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckAdv, g_cfg.advancedBlend ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckEnable, g_cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckPreview, g_cfg.previewMode ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(g_ckImage, (g_cfg.useImage && engineHasImage()) ? BST_CHECKED : BST_UNCHECKED);
        }
        refreshStatusBar();
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
            ComboBox_SetCurSel(g_cbImg, g_cfg.testImg);
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
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F3) {
            if (g_outWins.empty()) createOutputWindows();
            setCalibrationMode(true);
            setStatus(L"Calibration ON.");
        }
        return 0;
    case WM_DESTROY:
        g_ctl = nullptr;
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
    g_ctl = CreateWindowExW(WS_EX_TOPMOST, CTL_CLASS, L"Immersive Warp Native",
        WS_OVERLAPPEDWINDOW, 100, 100, 580, 600,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_ctl, SW_SHOW);
    UpdateWindow(g_ctl);
    refreshList();
    // global hotkeys so calibration keys work no matter which window has focus
    RegisterHotKey(nullptr, 1, 0, VK_F3);            // enter/exit calibration
    RegisterHotKey(nullptr, 2, 0, VK_F10);           // warp <-> blend mode
    RegisterHotKey(nullptr, 4, 0, VK_F12);           // cycle blended edges
    RegisterHotKey(nullptr, 5, MOD_CONTROL, VK_F3);  // snapshot (freeze capture)
}

// GET_X_LPARAM / GET_Y_LPARAM come from windowsx.h (included above)
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
