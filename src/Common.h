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
#include <algorithm>

#define APP_NAME L"ImmersiveWarpNative"
#define MAX_MASKS 8
#define MAX_ADV 8

struct BlendSide {
    float fade = 0.0f;   // fraction of output dimension, 0..0.5
    float gamma = 1.0f;
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
    int subdivide = 8;                // mesh subdivision per control cell

    OutputCfg* findByDevice(const std::wstring& dev) {
        for (auto& o : outputs) if (o.deviceName == dev) return &o;
        return nullptr;
    }
};

// ---- global shared state ----
extern std::atomic<int> g_calibMode;   // 0 = geometrical correction, 1 = edge blending
extern AppConfig g_cfg;
extern std::mutex g_cfgMutex;
extern std::atomic<int> g_rev;        // config revision, bumped on every edit
extern std::atomic<bool> g_calib;     // calibration mode active
extern std::atomic<bool> g_running;

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
void createOutputWindows();
void toggleCalibration();      // global hotkey F3: enter/exit calibration
void switchSubModeAll();       // global hotkey F10: warp <-> blend mode, all outputs
void notifyOutputRaised();     // output window raised itself; keep control window above
void destroyOutputWindows();
void setCalibrationMode(bool on);

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
