#include "Common.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

AppConfig g_cfg;
std::mutex g_cfgMutex;
std::atomic<int> g_rev{ 1 };
std::atomic<bool> g_calib{ false };
std::atomic<int> g_calibMode{ 0 };     // 0 = warp, 1 = blend
std::atomic<bool> g_previewMode{ false };
std::atomic<bool> g_previewNoCapture{ false };
std::atomic<bool> g_running{ true };
std::atomic<bool> g_snapshot{ false };

std::vector<DisplayInfo> enumerateDisplays() {
    std::vector<DisplayInfo> result;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return result;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc))) {
                DisplayInfo di;
                di.deviceName = desc.DeviceName;
                di.desktop = desc.DesktopCoordinates;
                di.primary = (desc.AttachedToDesktop != 0) &&
                             di.desktop.left == 0 && di.desktop.top == 0;
                wchar_t buf[128];
                wsprintfW(buf, L"%u: %s", (unsigned)(result.size() + 1), desc.DeviceName);
                di.label = buf;
                result.push_back(di);
            }
            output->Release();
        }
        adapter->Release();
        if (a > 8) break; // single adapter for P0
    }
    factory->Release();
    return result;
}

// ---------- config persistence: line-based key=value (UTF-8) ----------
static std::wstring cfgPath() {
    wchar_t path[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path) + L"\\ImmersiveWarpNative";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.txt";
}

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void writeSide(std::ostream& os, const char* name, const BlendSide& s, const std::vector<AdvPoint>& adv) {
    os << name << ".fade=" << s.fade << "\n" << name << ".gamma=" << s.gamma << "\n";
    os << name << ".advN=" << adv.size() << "\n";
    for (auto& p : adv)
        os << name << ".adv=" << p.pos << "," << p.fade << "," << p.gamma << "\n";
}

bool saveConfig(const std::wstring& pathIn) {
    std::wstring path = pathIn.empty() ? cfgPath() : pathIn;
    std::ofstream os(path);
    if (!os) return false;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    os << "testPattern=" << (g_cfg.testPattern ? 1 : 0) << "\n";
    os << "advancedBlend=" << (g_cfg.advancedBlend ? 1 : 0) << "\n";
    os << "enabled=" << (g_cfg.enabled ? 1 : 0) << "\n";
    os << "preview=" << (g_cfg.previewMode ? 1 : 0) << "\n";
    os << "useImage=" << (g_cfg.useImage ? 1 : 0) << "\n";
    os << "image=" << toUtf8(g_cfg.imagePath) << "\n";
    os << "testImg=" << g_cfg.testImg << "\n";
    os << "cmdnet=" << g_cfg.cmdnet << "\n";
    os << "cmdnetPort=" << g_cfg.cmdnetPort << "\n";
    os << "sampling=" << g_cfg.sampling << "\n";
    os << "autostart=" << g_cfg.autostart << "\n";
    os << "subdivide=" << g_cfg.subdivide << "\n";
    os << "outputs=" << g_cfg.outputs.size() << "\n";
    for (auto& o : g_cfg.outputs) {
        os << "dev=" << toUtf8(o.deviceName) << "\n";
        os << "label=" << toUtf8(o.label) << "\n";
        os << "enabled=" << (o.enabled ? 1 : 0) << "\n";
        os << "grid=" << o.rows << " " << o.cols << "\n";
        os << "edges=" << o.edges << "\n";
        for (size_t i = 0; i + 1 < o.points.size(); i += 2)
            os << "pt=" << o.points[i] << " " << o.points[i + 1] << "\n";
        writeSide(os, "left", o.blend[0], o.adv[0]);
        writeSide(os, "right", o.blend[1], o.adv[1]);
        writeSide(os, "top", o.blend[2], o.adv[2]);
        writeSide(os, "bottom", o.blend[3], o.adv[3]);
        os << "color=" << o.color.gammaR << "," << o.color.gammaG << "," << o.color.gammaB
           << "," << o.color.bright << "," << o.color.contrast << "\n";
        os << "masksN=" << o.masks.size() << "\n";
        for (auto& m : o.masks)
            os << "mask=" << m.x << "," << m.y << "," << m.w << "," << m.h << "\n";
    }
    return true;
}

static void parseSideVal(const std::string& key, const std::string& val, OutputCfg& o, int side) {
    auto& b = o.blend[side];
    auto& adv = o.adv[side];
    if (key == "fade") b.fade = (float)atof(val.c_str());
    else if (key == "gamma") b.gamma = (float)atof(val.c_str());
    else if (key == "advN") adv.clear();
    else if (key == "adv") {
        AdvPoint p; sscanf_s(val.c_str(), "%f,%f,%f", &p.pos, &p.fade, &p.gamma);
        adv.push_back(p);
    }
}

bool loadConfig(const std::wstring& pathIn) {
    std::wstring path = pathIn.empty() ? cfgPath() : pathIn;
    std::ifstream is(path);
    if (!is) return false;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    AppConfig c;
    std::string line;
    OutputCfg* cur = nullptr;
    int advSide = -1;
    while (std::getline(is, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "testPattern") c.testPattern = val == "1";
        else if (key == "advancedBlend") c.advancedBlend = val == "1";
        else if (key == "preview") c.previewMode = val == "1";
        else if (key == "useImage") c.useImage = val == "1";
        else if (key == "image") c.imagePath = fromUtf8(val);
        else if (key == "testImg") c.testImg = atoi(val.c_str());
        else if (key == "cmdnet") c.cmdnet = atoi(val.c_str());
        else if (key == "cmdnetPort") c.cmdnetPort = atoi(val.c_str());
        else if (key == "sampling") c.sampling = atoi(val.c_str());
        else if (key == "autostart") c.autostart = atoi(val.c_str());
        else if (key == "subdivide") c.subdivide = atoi(val.c_str());
        else if (key == "outputs") c.outputs.resize((size_t)atoi(val.c_str()));
        else if (key == "dev") {
            cur = nullptr;
            for (auto& o : c.outputs) if (o.deviceName.empty()) { cur = &o; break; }
            if (cur) cur->deviceName = fromUtf8(val);
        }
        else if (!cur) { if (key == "enabled") c.enabled = val == "1"; }  // global keys
        else if (key == "enabled") cur->enabled = val == "1";             // per-output
        else if (key == "label") cur->label = fromUtf8(val);
        else if (key == "grid") sscanf_s(val.c_str(), "%d %d", &cur->rows, &cur->cols);
        else if (key == "edges") cur->edges = atoi(val.c_str()) & 15;
        else if (key == "pt") {
            float x = 0, y = 0;
            sscanf_s(val.c_str(), "%f %f", &x, &y);
            cur->points.push_back(x); cur->points.push_back(y);
        }
        else if (key == "left") { advSide = 0; cur->adv[0].clear(); }
        else if (key == "right") { advSide = 1; cur->adv[1].clear(); }
        else if (key == "top") { advSide = 2; cur->adv[2].clear(); }
        else if (key == "bottom") { advSide = 3; cur->adv[3].clear(); }
        else if (key == "color") sscanf_s(val.c_str(), "%f,%f,%f,%f,%f",
            &cur->color.gammaR, &cur->color.gammaG, &cur->color.gammaB, &cur->color.bright, &cur->color.contrast);
        else if (key == "masksN") cur->masks.clear();
        else if (key == "mask") {
            MaskRect m; sscanf_s(val.c_str(), "%f,%f,%f,%f", &m.x, &m.y, &m.w, &m.h);
            cur->masks.push_back(m);
        }
        else {
            // side.* keys
            static const char* sideNames[4] = { "left", "right", "top", "bottom" };
            bool matched = false;
            for (int s = 0; s < 4; s++) {
                std::string pre = std::string(sideNames[s]) + ".";
                if (key.rfind(pre, 0) == 0) { parseSideVal(key.substr(pre.size()), val, *cur, s); matched = true; break; }
            }
            (void)matched; (void)advSide;
        }
    }
    // repair: a config without a full control-point grid (older file, manual
    // edit, interrupted save) would leave OutputCfg::points empty and every
    // grid draw would read out of bounds -> rebuild the identity grid
    for (auto& o : c.outputs) {
        if (o.rows < 2) o.rows = 2;
        if (o.cols < 2) o.cols = 2;
        if (o.points.size() != (size_t)o.rows * o.cols * 2) o.initIdentity();
    }
    g_cfg = c;
    g_rev++;
    {
        // startup diagnostics: proves what the parser actually produced
        int en = 0;
        for (auto& o : g_cfg.outputs) if (o.enabled) en++;
        char b[256];
        sprintf_s(b, "loadConfig: outputs=%zu enabledOut=%d globalEnabled=%d preview=%d useImage=%d img=%s",
            g_cfg.outputs.size(), en, (int)g_cfg.enabled, (int)g_cfg.previewMode,
            (int)g_cfg.useImage, g_cfg.imagePath.empty() ? "(none)" : "(set)");
        dbgLogLine(b);
        for (auto& o : g_cfg.outputs) {
            sprintf_s(b, "loadConfig: dev=%ls enabled=%d pts=%zu grid=%dx%d",
                o.deviceName.c_str(), (int)o.enabled, o.points.size(), o.rows, o.cols);
            dbgLogLine(b);
        }
    }
    return true;
}
