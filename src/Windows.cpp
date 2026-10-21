#include "Common.h"
#include "Engine.h"
#include <windowsx.h>

// ------------------------------------------------------------------
// Output windows (fullscreen per display) + calibration interaction
// ------------------------------------------------------------------
struct OutWinState {
    int displayIdx = -1;
    int subMode = 0;              // 0 = geometrical correction, 1 = edge blending
    int dragKind = -1;            // -1 none, 0 warp point, 1 simple blend side, 2 advanced point
    int dragIdx = -1;             // warp point index or blend side index
    int advIdx = -1;              // advanced point index
    float lastX = 0, lastY = 0;
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
            for (size_t i = 0; i < cfg->adv[s].size(); i++) {
                auto& p = cfg->adv[s][i];
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
            if (b.fade <= 0.001f) continue;
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
        if (!g_calib.load() || !st) break;
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
        if (!g_calib.load() || !st || st->dragKind < 0) break;
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
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    int n = 0;
    for (size_t i = 0; i < displays.size(); i++) {
        OutputCfg* cfg = nullptr;
        for (auto& o : g_cfg.outputs)
            if (o.enabled && o.deviceName == displays[i].deviceName) { cfg = &o; break; }
        if (!cfg) continue;
        n++;
        auto* st = new OutWinState();
        st->displayIdx = (int)i;
        RECT r = displays[i].desktop;
        // WS_EX_NOACTIVATE: clicking the output never raises it above the
        // console and never steals keyboard focus (hotkeys handle F3/F10/Esc)
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, OUT_CLASS,
            cfg->label.c_str(), WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
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
static HWND g_list = nullptr, g_status = nullptr;
static HWND g_ckEnable = nullptr, g_ckTest = nullptr, g_ckAdv = nullptr;

static void setStatus(const wchar_t* s) {
    if (g_status) SetWindowTextW(g_status, s);
}

void setCalibrationMode(bool on) {
    bool was = g_calib.exchange(on);
    if (was == on) return;
    // Esc is captured globally only while calibrating, so it never
    // interferes with normal Escape usage in other apps
    if (on) RegisterHotKey(nullptr, 3, 0, VK_ESCAPE);
    else UnregisterHotKey(nullptr, 3);
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
    if (!g_calib.load()) return;
    for (auto& p : g_outWins) p.second->subMode = 1 - p.second->subMode;
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

static void refreshList() {
    if (!g_list) return;
    ListBox_ResetContent(g_list);
    auto displays = enumerateDisplays();
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
        g_status = CreateWindowW(L"STATIC", L"Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 340, 540, 60,
            hwnd, nullptr, nullptr, nullptr);
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        Button_SetCheck(g_ckTest, g_cfg.testPattern ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_ckAdv, g_cfg.advancedBlend ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_ckEnable, g_cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
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
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerAndCreateControlWindow(HINSTANCE hInst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ControlWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CTL_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_ctl = CreateWindowExW(WS_EX_TOPMOST, CTL_CLASS, L"Immersive Warp Native",
        WS_OVERLAPPEDWINDOW, 100, 100, 580, 460,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_ctl, SW_SHOW);
    UpdateWindow(g_ctl);
    refreshList();
    // global hotkeys so calibration keys work no matter which window has focus
    RegisterHotKey(nullptr, 1, 0, VK_F3);   // enter/exit calibration
    RegisterHotKey(nullptr, 2, 0, VK_F10);  // warp <-> blend mode
}

// GET_X_LPARAM / GET_Y_LPARAM come from windowsx.h (included above)
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
