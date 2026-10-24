#pragma once
// Immersive Warp Native - common types & configuration (P0)
#ifndef NOMINMAX
#define NOMINMAX // prevent windows.h min/max macros from breaking std::min/std::max
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// debug logging (%APPDATA%\ImmersiveWarpNative\debug.log)
void dbgLogLine(const std::string& line);
#include <atomic>
#include <cmath>
#include <cwctype>
#include <algorithm>

#define APP_NAME L"ImmersiveWarpNative"
#define MAX_MASKS 8
#define MAX_ADV 8

struct BlendSide {
    float fade = 0.0f;   // fraction of output dimension, 0..0.5
    float gamma = 1.0f;
    // parametric blend curve (matches the original's Gain/Slope per side):
    // f(x) = (gain+1)x / (slope*(x-1) + gain*x + 1). gain=0, slope=0 gives
    // the plain linear ramp f(x)=x that earlier builds used, so defaults
    // preserve existing configs pixel-for-pixel. gain 0..2, slope 0..1.
    float gain = 0.0f;
    float slope = 0.0f;
    // per-channel blend gamma (R/G/B): the weight becomes
    // pow(blendCurve(t), edgeGamma * chanC) per color channel, so 1/1/1
    // reproduces the legacy scalar weight exactly. Used to trim the overlap
    // zone's color balance (one projector's red stronger than the other's).
    // DXGI engine applies this in the shader; the MAG engine's layered
    // overlay has a single alpha per pixel and keeps the scalar weight.
    float chanR = 1.0f, chanG = 1.0f, chanB = 1.0f;
};

struct AdvPoint {
    float pos = 0.5f;    // coordinate along the edge 0..1
    float fade = 0.0f;
    float gamma = 1.0f;
};

struct ColorAdj {
    float gammaR = 1.0f, gammaG = 1.0f, gammaB = 1.0f;
    float bright = 1.0f, contrast = 1.0f;
};

struct MaskRect { float x = 0, y = 0, w = 0.10f, h = 0.10f; };   // default = 10%x10% at top-left

struct OutputCfg {
    std::wstring deviceName;          // DXGI output device name e.g. \\.\DISPLAY1
    std::wstring label;               // "1: DISPLAY1 [Primary]"
    std::wstring srcOverride;         // per-output capture source (SET src=).
                                      // empty = follow the global sourceDev pin
    bool enabled = false;
    int rows = 10, cols = 12;
    // dest positions of control points, row-major, 2*rows*cols floats, normalized [0..1]
    // src uv is a uniform grid; warping = moving these dest points
    std::vector<float> points;
    BlendSide blend[4];               // 0=left 1=right 2=top 3=bottom
    std::vector<AdvPoint> adv[4];     // advanced blending per side
    // which edges participate in blending: bit0=left bit1=right bit2=top bit3=bottom.
    // F12 cycles through the masks (all / single edges / none) like the original.
    int edges = 15;
    ColorAdj color;
    std::vector<MaskRect> masks;

    void initIdentity() {
        points.clear();
        points.reserve((size_t)rows * cols * 2);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                points.push_back((float)c / (cols - 1)); // dest x
                points.push_back((float)r / (rows - 1)); // dest y
            }
    }
};

struct AppConfig {
    std::vector<OutputCfg> outputs;
    bool testPattern = true;
    bool advancedBlend = false;
    bool enabled = false;             // Enable Desktop warping
    bool previewMode = false;         // windowed preview (single screen live warp)
    bool inplace = false;             // in-place overlay: fullscreen click-through output
    bool useImage = false;            // image source instead of pattern/capture
    std::wstring imagePath;
    int testImg = -1;                 // built-in test image library (0..3), -1 = none
    int testAnim = 0;                 // dynamic pattern scroll: 0 off, 1 H, 2 V, 3 diagonal
    int checker = 0;                  // 20x10 checkerboard overlay (screen space, blend-alignment aid)
    int subdivide = 8;                // mesh subdivision per control cell
    int curved = 0;                   // 1 = Catmull-Rom curved grid interp (needs rows>=4 && cols>=4), 0 = bilinear
    int cmdnet = 0;                   // CMDNET ASCII TCP show-control service on/off
    int cmdnetPort = 5252;            // CMDNET listen port (loopback only)
    int sampling = 0;                 // 0 trilinear, 1 aniso 4x, 2 aniso 8x, 3 bicubic
    int autostart = 0;                // start with Windows (HKCU Run mirror)
    // explicit capture source. empty = auto (first display that is not an
    // output). A non-empty value pins the desktop that gets warped, which is
    // what the original's Source page does on multi-screen rigs.
    std::wstring sourceDev;
    // NxN tiled layout of the enabled outputs: output k sits at
    // (row = k / layoutC, col = k % layoutC) after sorting by desktop
    // position, and shows the source window [col*(1-ov), col*(1-ov)+1] etc.
    // overlap ov is a percentage of one projector width/height (0..40).
    int layoutR = 1, layoutC = 1;
    int overlap = 0;
    // warp engine. 0 = DXGI Desktop Duplication (full warp/blend pipeline),
    // 1 = DWM Magnification host windows (1:1 desktop mirror, no mesh warp;
    // same mechanism the original uses as its DWM engine).
    int engine = 0;

    OutputCfg* findByDevice(const std::wstring& dev) {
        for (auto& o : outputs) if (o.deviceName == dev) return &o;
        return nullptr;
    }
};

// ---- global shared state ----
extern std::atomic<int> g_calibMode;   // 0 = geometrical correction, 1 = edge blending
extern std::atomic<bool> g_previewMode; // windowed preview (single-screen live warp)
// in-place overlay: output windows cover their monitor edge-to-edge with
// WS_EX_TRANSPARENT (mouse clicks fall through to the desktop) and are
// excluded from capture, so the warped result appears to be the desktop
// itself - the user-mode stand-in for the original's DWM desktop warping
extern std::atomic<bool> g_inplace;
// post WM_INPLACESET to the console so it restyles the output windows
// (implemented in Windows.cpp; safe from any thread)
void requestInplaceRestyle();
void requestMagBlendSync();    // any thread: MAG edge-blend overlays resync (UI thread)
void requestOutputRebuild();   // any thread: destroy+recreate output windows on the UI thread
void requestMagTestSync();     // any thread: test pattern overlays follow (engine=1)
void requestMagProbeSync();    // any thread: probe marker overlays follow (engine=1)
void requestSourceRebuild();   // any thread: per-output src override -> rebuild outputs
void requestOvrSrcSync();      // any thread: override combo follows SET src=
void requestBlackoutSync();    // any thread: blackout overlays + menu checkbox follow
void refreshEngineComboUI();   // socket thread safe: engine dropdown follows shared state
void refreshSampComboUI();     // socket thread safe: sampling dropdown follows shared state
// socket thread safe AND callable while holding g_cfgMutex: no internal
// locking, the caller passes the captured values (SET testanim runs inside
// the CMDNET locked region - an internal lock would self-deadlock)
void refreshAnimUI(int anim, bool paused);
void refreshCheckerUI(bool on);   // same family: checker overlay checkbox follows

// ---- DWM Magnification engine (src/MagEngine.cpp) --------------------
// engine=1: output windows are magnifier hosts mirroring the source
// desktop 1:1 (DWM-composited, no capture session, no render thread).
bool magEngineAvailable();                     // MagInitialize succeeded
bool magEngineInit();                          // idempotent process-wide init
void magEngineShutdown();                      // idempotent uninit
bool magAttachOutput(HWND host, const RECT& srcDesktop); // source rect + recursion filter
void magSetConsoleWindow(HWND ctl);            // console is excluded from the feed too
void magAddFilter(HWND w);                     // extra windows hidden from the feed
void magRemoveFilter(HWND w);
void magDetachAll();                           // clear sources, uninit
// set when WDA_EXCLUDEFROMCAPTURE is unavailable: capturing our own display
// would recursively film the output window, so capture is disabled instead
extern std::atomic<bool> g_previewNoCapture;
extern AppConfig g_cfg;
extern std::mutex g_cfgMutex;
extern std::atomic<int> g_rev;        // config revision, bumped on every edit
extern std::atomic<bool> g_calib;     // calibration mode active
// Register/UnregisterHotKey MUST run on the thread with the message loop (the
// UI thread) - registering from the socket thread would post WM_HOTKEY into a
// queue nobody pumps. CMDNET dispatches through this posted message.
constexpr UINT WM_CALIBSET = WM_APP + 12;   // wp=0, lp = desired g_calib value
void calibrationHotkeysSync(bool on);       // idempotent, UI thread only
void requestCalibrationSync();              // any thread: post WM_CALIBSET
extern std::atomic<bool> g_running;
// Ctrl+F3 snapshot mode: freeze the captured desktop into a still frame so
// alignment can be done on a stable image (matches the original's
// "Edit Warp and Blend with screenshot")
extern std::atomic<bool> g_snapshot;
// PROBE mode: bright white cross markers at every warp destination point so
// a calibration camera can detect where each grid point actually landed and
// feed the measured positions back through SET warp.load (auto-alignment)
extern std::atomic<bool> g_probe;
// show blackout: all outputs render black (View menu, CMDNET BLACKOUT).
// Session-only state - never persisted to config.txt.
extern std::atomic<bool> g_blackout;
extern std::atomic<bool> g_testPause;   // dynamic test pattern paused (session-only)
// WM_DISPLAYCHANGE counter (resolution change / monitor hotplug). The console
// window reposts itself into a deferred rebuild of the output windows; the
// counter is read back via GET dispchange for test/observability.
extern std::atomic<int> g_dispChange;
// live output-window count, maintained by the UI thread wherever g_outWins
// gains/loses entries. The status bar (also refreshed from socket threads)
// reads this instead of touching the g_outWins vector itself.
extern std::atomic<int> g_outCount;

struct DisplayInfo {
    std::wstring deviceName, label;
    RECT desktop;                     // desktop coordinates
    bool primary = false;
};
std::vector<DisplayInfo> enumerateDisplays();

// config persistence (simple text format)
bool saveConfig(const std::wstring& path);
bool loadConfig(const std::wstring& path);
std::wstring presetPath(const std::string& name);  // %APPDATA%\...\presets\<NAME>.cfg

// windows
void registerAndCreateControlWindow(HINSTANCE hInst);
// image source (WIC decode); empty path clears back to pattern/capture
bool engineLoadImage(const std::wstring& path);
bool engineHasImage();

// engine diagnostics for CMDNET "GET perf" (remote health monitoring)
double engineFps();                      // rolling 1s average, 0 before first window
unsigned long long engineFrames();       // frames rendered since start
unsigned long long engineUptimeMs();     // ms since process start
double engineCapMs();                    // capture stage, EMA ms (x100 fixed inside)
double engineDrawMs();                   // compose+draw+present stage, EMA ms
double engineFrameMs();                  // whole renderAll pass, EMA ms

// periodic perf snapshots into debug.log (config key perflog=, 1 line / 10 s)
extern std::atomic<bool> g_perfLog;
bool engineSetTestImage(int id);   // built-in test image library (0..3)
void createOutputWindows();
// render-thread bridge: the in-progress right-drag mask rectangle (true when
// a rubber band is being drawn on that output window)
bool getPendingMask(HWND hwnd, float m[4]);
void toggleCalibration();      // global hotkey F3: enter/exit calibration
void switchSubModeAll();       // global hotkey F10: warp <-> blend mode, all outputs
void cycleBlendEdges();        // global hotkey F12: cycle blended edges
void toggleSnapshot();         // global hotkey Ctrl+F3: freeze / resume capture
void toggleInplaceOverlay();   // global hotkey Ctrl+F9: in-place overlay safety switch
void toggleWindowWarping();    // global hotkey F4: start/stop outputs (Enabled flip)
void loadPresetByIndex(int i); // global hotkey Ctrl+Shift+1..8: Nth preset by name (0-based)
void openColorDialogEx();      // global hotkey D: color adjustment dialog
// per-projector "Configure display" dialog (Bezels/Align/Geometry/Blending/
// Color/Masking/Help tabs, edits the output selected in the console list)
void openConfigDialog();
// apply an R x C tiled layout with the given overlap percent (0..40):
// regenerates the warp grid of every enabled output and sets the blend-edge
// mask so only interior boundaries blend. Safe from any thread (marshals to UI).
void setLayout(int R, int C, int overlapPct);
void notifyOutputRaised();     // output window raised itself; keep control window above
void destroyOutputWindows();
void setCalibrationMode(bool on);
void refreshStatusBar();       // re-render the console status block
void setBlendEdgesMask(int mask); // explicit edge set (CMDNET), complements cycleBlendEdges
// CMDNET ASCII TCP show-control service: "<COMMAND> <PARAM>;\n", one request one reply
void cmdnetStart(int port);       // loopback only
void cmdnetStop();
// start with Windows: HKCU\...\Run value "ImmersiveWarpNative" (no admin needed)
void setAutostart(bool on);
bool queryAutostart();
// capture source ("Source:" combo / CMDNET SET source=). Empty string = auto.
// The duplication is bound when an output is attached, so a change rebuilds
// the output windows while running.
void setSourceDisplay(const std::wstring& dev);
std::wstring currentSourceDevice();   // "" when auto

// per-output command target ("SET out=DISPLAY2" / console "Target:" combo).
// Empty string = auto (masks/fade -> first enabled output, grid -> all).
// Shared storage, so the network command and the dropdown stay in sync.
void cmdSetTarget(const std::string& dev);   // "" = auto
std::string cmdGetTarget();
void refreshTargetComboUI();                 // resync the console dropdown

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// "\\.\DISPLAY2" and "DISPLAY2" denote the same output (CMDNET / config files
// may use either form, DXGI always reports the long one)
inline bool sameDisplayName(const std::wstring& a, const std::wstring& b) {
    auto norm = [](std::wstring s) {
        size_t p = s.find(L"\\\\.\\");
        if (p != std::wstring::npos) s = s.substr(p + 4);
        for (auto& c : s) c = (wchar_t)towupper(c);
        return s;
    };
    return !a.empty() && !b.empty() && norm(a) == norm(b);
}
