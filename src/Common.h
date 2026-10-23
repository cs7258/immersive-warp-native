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

struct MaskRect { float x = 0, y = 0, w = 0.2f, h = 0.2f; };

struct OutputCfg {
    std::wstring deviceName;          // DXGI output device name e.g. \\.\DISPLAY1
    std::wstring label;               // "1: DISPLAY1 [Primary]"
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
    bool useImage = false;            // image source instead of pattern/capture
    std::wstring imagePath;
    int testImg = -1;                 // built-in test image library (0..3), -1 = none
    int subdivide = 8;                // mesh subdivision per control cell
    int cmdnet = 0;                   // CMDNET ASCII TCP show-control service on/off
    int cmdnetPort = 5252;            // CMDNET listen port (loopback only)
    int sampling = 0;                 // 0 trilinear, 1 aniso 4x, 2 aniso 8x
    int autostart = 0;                // start with Windows (HKCU Run mirror)
    // explicit capture source. empty = auto (first display that is not an
    // output). A non-empty value pins the desktop that gets warped, which is
    // what the original's Source page does on multi-screen rigs.
    std::wstring sourceDev;

    OutputCfg* findByDevice(const std::wstring& dev) {
        for (auto& o : outputs) if (o.deviceName == dev) return &o;
        return nullptr;
    }
};

// ---- global shared state ----
extern std::atomic<int> g_calibMode;   // 0 = geometrical correction, 1 = edge blending
extern std::atomic<bool> g_previewMode; // windowed preview (single-screen live warp)
// set when WDA_EXCLUDEFROMCAPTURE is unavailable: capturing our own display
// would recursively film the output window, so capture is disabled instead
extern std::atomic<bool> g_previewNoCapture;
extern AppConfig g_cfg;
extern std::mutex g_cfgMutex;
extern std::atomic<int> g_rev;        // config revision, bumped on every edit
extern std::atomic<bool> g_calib;     // calibration mode active
extern std::atomic<bool> g_running;
// Ctrl+F3 snapshot mode: freeze the captured desktop into a still frame so
// alignment can be done on a stable image (matches the original's
// "Edit Warp and Blend with screenshot")
extern std::atomic<bool> g_snapshot;

struct DisplayInfo {
    std::wstring deviceName, label;
    RECT desktop;                     // desktop coordinates
    bool primary = false;
};
std::vector<DisplayInfo> enumerateDisplays();

// config persistence (simple text format)
bool saveConfig(const std::wstring& path);
bool loadConfig(const std::wstring& path);

// windows
void registerAndCreateControlWindow(HINSTANCE hInst);
// image source (WIC decode); empty path clears back to pattern/capture
bool engineLoadImage(const std::wstring& path);
bool engineHasImage();
bool engineSetTestImage(int id);   // built-in test image library (0..3)
void createOutputWindows();
void toggleCalibration();      // global hotkey F3: enter/exit calibration
void switchSubModeAll();       // global hotkey F10: warp <-> blend mode, all outputs
void cycleBlendEdges();        // global hotkey F12: cycle blended edges
void toggleSnapshot();         // global hotkey Ctrl+F3: freeze / resume capture
void openColorDialogEx();      // global hotkey D: color adjustment dialog
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
