// CMDNET: ASCII TCP show-control service.
// Aligns with the original's protocol: plain-text "<COMMAND> <PARAM>;\n",
// one request -> one reply, meant for show-control / OSC-style integration.
// Binds to 127.0.0.1 only: remote control stays on this machine unless a
// future version explicitly opts into LAN exposure.
// winsock2.h MUST come before windows.h (pulled in via Common.h), otherwise
// the ancient winsock.h inside windows.h collides with it (sockaddr redefined)
#include <winsock2.h>
#include <ws2tcpip.h>
#include "Common.h"
#include "Engine.h"
#include <thread>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>

static std::atomic<bool> g_netRun{ false };
static std::thread g_netThread;
// OSC server globals (defined up here: the SET osc/oscport handlers in
// dispatch() reference g_oscPortLive, and definitions must precede use)
static std::atomic<bool> g_oscRun{ false };
static std::atomic<int>  g_oscPortLive{ 8000 };
static SOCKET g_oscSock = INVALID_SOCKET;
static std::thread g_oscThread;
static SOCKET g_listenSock = INVALID_SOCKET;

static std::string toNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// short form used on the wire: "\\.\DISPLAY2" -> "DISPLAY2"
static std::string shortDev(const std::wstring& w) {
    std::string s = toNarrow(w);
    size_t p = s.find("\\\\.\\");
    if (p != std::string::npos) s = s.substr(p + 4);
    return s;
}

static std::string edgesName(int m) {
    if (m == 15) return "ALL";
    if (m == 0)  return "NONE";
    std::string s;
    if (m & 1) s += "L";
    if (m & 2) s += (s.empty() ? "R" : "+R");
    if (m & 4) s += (s.empty() ? "T" : "+T");
    if (m & 8) s += (s.empty() ? "B" : "+B");
    return s;
}

static int edgesMask(const std::string& v) {
    if (v == "ALL")  return 15;
    if (v == "NONE") return 0;    // compound forms like "L+T" / "LEFT+RIGHT"
    int m = 0;
    size_t pos = 0;
    while (pos <= v.size()) {
        size_t plus = v.find('+', pos);
        std::string t = v.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        if      (t == "L" || t == "LEFT")  m |= 1;
        else if (t == "R" || t == "RIGHT") m |= 2;
        else if (t == "T" || t == "TOP")   m |= 4;
        else if (t == "B" || t == "BOTTOM") m |= 8;
        else return -1;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return m;
}

// GET presets: names of all saved presets (without the .cfg extension)
static std::string listPresets() {
    // derive the presets dir from presetPath() instead of calling
    // SHGetFolderPathW here (keeps the path logic in Config.cpp, avoids
    // pulling shlobj.h into the socket TU)
    std::wstring file = presetPath("x");
    std::wstring dir = file.substr(0, file.find_last_of(L'\\') + 1);
    std::string out = "OK presets=";
    bool first = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cfg").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string s = toNarrow(fd.cFileName);
            if (s.size() > 4 && s.rfind(".cfg") == s.size() - 4) s = s.substr(0, s.size() - 4);
            if (!first) out += ";";
            out += s;
            first = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (first) out += "none";
    return out + "\n";
}

// ---- per-output command target ------------------------------------------
// "SET out=DISPLAY2" points the following per-output commands (masks, blend
// sides, rows/cols, flip/rotate, edges) at that one output instead of the
// defaults. Empty target = legacy behavior (masks/fade read/write the first
// enabled output, grid ops apply to every enabled output).
static std::string g_cmdTarget;               // empty = auto
static std::mutex  g_cmdTargetMutex;

static std::string currentTarget() {
    std::lock_guard<std::mutex> lk(g_cmdTargetMutex);
    return g_cmdTarget;
}

// shared with the console "Target:" combo (Windows.cpp): one storage, both
// entry points stay in sync. Empty string = auto.
void cmdSetTarget(const std::string& dev) {
    std::lock_guard<std::mutex> lk(g_cmdTargetMutex);
    g_cmdTarget = dev;
}
std::string cmdGetTarget() {
    std::lock_guard<std::mutex> lk(g_cmdTargetMutex);
    return g_cmdTarget;
}

// resolve the target under g_cfgMutex (caller holds it). Matches the short
// device name ("DISPLAY2") or the label text, case-insensitive.
static OutputCfg* targetOutputLocked() {
    std::string t = currentTarget();
    for (auto& o : g_cfg.outputs) {
        if (!o.enabled) continue;
        if (t.empty()) return &o;   // legacy: first enabled
        if (_stricmp(shortDev(o.deviceName).c_str(), t.c_str()) == 0 ||
            _stricmp(toNarrow(o.label).c_str(), t.c_str()) == 0)
            return &o;
    }
    return nullptr;
}

static std::string dispatch(std::string line) {
    // trim + uppercase
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    for (auto& c : line) c = (char)toupper((unsigned char)c);
    size_t sp = line.find(' ');
    std::string cmd = line, arg;
    if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

    if (cmd == "PING") return "OK\n";

    // GET key: query a single value (integration-friendly counterpart of SET)
    if (cmd == "GET") {
        std::string k = arg;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "calib")    return std::string("OK calib=")    + (g_calib.load() ? "on" : "off") + "\n";
        if (k == "mode")     return std::string("OK mode=")     + (g_calibMode.load() == 1 ? "blend" : "warp") + "\n";
        if (k == "snapshot") return std::string("OK snapshot=") + (g_snapshot.load() ? "on" : "off") + "\n";
        if (k == "probe")    return std::string("OK probe=")    + (g_probe.load() ? "on" : "off") + "\n";
        if (k == "perflog")  return std::string("OK perflog=")  + (g_perfLog.load() ? "on" : "off") + "\n";
        if (k == "osc")      return std::string("OK osc=")      + (oscRunning() ? "on" : "off") + "\n";
        if (k == "oscport") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK oscport=" + std::to_string(g_cfg.oscPort) + "\n";
        }
        if (k == "inplace")  return std::string("OK inplace=")  + (g_inplace.load() ? "on" : "off") + "\n";
        // remote health monitoring: one line, key=value; pairs (STATUS style)
        if (k == "perf") {
            int nOut = 0;
            bool running;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                for (auto& o : g_cfg.outputs) if (o.enabled) nOut++;
                running = g_cfg.enabled || g_cfg.previewMode;
            }
            char buf[224];
            // fps/ms are fractional -> CRT snprintf (wsprintf has no %f)
            _snprintf_s(buf, _TRUNCATE,
                        "OK perf=uptime=%llu;frames=%llu;fps=%.1f;cap=%.2f;draw=%.2f;frame=%.2f;lat=%.2f;outputs=%d;running=%d\n",
                        (unsigned long long)(engineUptimeMs() / 1000),
                        engineFrames(), engineFps(),
                        engineCapMs(), engineDrawMs(), engineFrameMs(), engineLatMs(),
                        nOut, running ? 1 : 0);
            return buf;
        }
        if (k == "sampling") return "OK sampling=" + std::to_string(g_cfg.sampling) + "\n";
        if (k == "maxfps") return "OK maxfps=" + std::to_string(g_cfg.maxfps) + "\n";
        if (k == "hook") {
            char hex[16];
            snprintf(hex, sizeof(hex), "%X", hookIpcExcCode());
            return "OK hook=" + std::string(hookIpcEnabled() ? "on" : "off") +
                   ";pass=" + std::to_string(hookIpcPassCount()) +
                   ";exc=0x" + hex +
                   ";etid=" + std::to_string(hookIpcExcTid()) +
                   ";outs=" + std::to_string(hookIpcOutCount()) +
                   ";mesh=" + std::to_string(hookIpcMeshVerts()) +
                   ";rev=" + std::to_string(hookIpcMeshRev()) +
                   ";ovl=" + std::string(hookIpcOverlay() ? "on" : "off") + "\n";
        }
        if (k == "srcs") {   // per-output source overrides (empty "-" = global pin)
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            std::string s;
            int n = 0;
            for (auto& o : g_cfg.outputs) {
                if (!o.enabled) continue;
                s += ";src" + std::to_string(++n) + "=" + shortDev(o.deviceName) + ">" +
                     (o.srcOverride.empty() ? std::string("-") : shortDev(o.srcOverride));
            }
            return "OK srcs=" + std::to_string(n) + s + "\n";
        }
        if (k == "blackout") return std::string("OK blackout=") + (g_blackout.load() ? "on" : "off") + "\n";
        if (k == "testpause") return std::string("OK testpause=") + (g_testPause.load() ? "on" : "off") + "\n";
        if (k == "testanim") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK testanim=" + std::to_string(g_cfg.testAnim) + "\n";
        }
        if (k == "checker") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return std::string("OK checker=") + (g_cfg.checker ? "on" : "off") + "\n";
        }
        if (k == "subdivide") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK subdivide=" + std::to_string(g_cfg.subdivide) + "\n";
        }
        if (k == "snap") {
            std::lock_guard<std::mutex> lk(g_snapMutex);
            return std::string("OK snap=") + (g_lastSnap.empty() ? "none" : g_lastSnap) + "\n";
        }
        if (k == "move") {
            int m = g_moveMode.load();
            return std::string("OK move=") + (m == 1 ? "xy" : m == 2 ? "all" : "single") + "\n";
        }
        if (k == "roi") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            char b[64];
            sprintf_s(b, "OK roi=%d,%d,%d,%d\n",
                      g_cfg.roi[0], g_cfg.roi[1], g_cfg.roi[2], g_cfg.roi[3]);
            return b;
        }
        if (k == "roicircle") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return std::string("OK roicircle=") + (g_cfg.roiCircle ? "on" : "off") + "\n";
        }
        if (k == "roiborder") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK roiborder=" + std::to_string(g_cfg.roiBorder) + "\n";
        }
        if (k == "autostart")return std::string("OK autostart=") + (queryAutostart() ? "on" : "off") + "\n";
        if (k == "layout") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK layout=" + std::to_string(g_cfg.layoutC) + "x" +
                   std::to_string(g_cfg.layoutR) + "\n";
        }
        if (k == "overlap") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK overlap=" + std::to_string(g_cfg.overlap) + "\n";
        }
        if (k == "engine") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return "OK engine=" + std::string(g_cfg.engine == 1 ? "mag" : "desktopduplication") + "\n";
        }
        if (k == "dispchange") {
            return "OK dispchange=" + std::to_string(g_dispChange.load()) + "\n";
        }
        if (k == "curved") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return std::string("OK curved=") + (g_cfg.curved ? "on" : "off") + "\n";
        }
        if (k == "out") {
            std::string t = currentTarget();
            return "OK out=" + (t.empty() ? std::string("auto") : t) + "\n";
        }
        // full state dump in the same key=value; pairing as STATUS: global
        // keys first, then one outN.* block per output (masks use SPACES
        // between rects because ';' would end the pair). A show controller
        // can mirror the whole UI state in one round trip.
        if (k == "config") {
            char b[160];
            std::string s;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                s += "sampling=" + std::to_string(g_cfg.sampling) + ";";
                s += "maxfps=" + std::to_string(g_cfg.maxfps) + ";";
                s += std::string("hook=") + (hookIpcEnabled() ? "on" : "off") + ";";
                s += std::string("hookovl=") + (hookIpcOverlay() ? "on" : "off") + ";";
                s += std::string("testpattern=") + (g_cfg.testPattern ? "on" : "off") + ";";
                s += "layout=" + std::to_string(g_cfg.layoutC) + "x" + std::to_string(g_cfg.layoutR) + ";";
                s += "overlap=" + std::to_string(g_cfg.overlap) + ";";
                s += std::string("curved=") + (g_cfg.curved ? "on" : "off") + ";";
                s += std::string("engine=") + (g_cfg.engine == 1 ? "mag" : "desktopduplication") + ";";
                s += "osc=" + std::to_string(g_cfg.osc) + ";";
                s += "oscport=" + std::to_string(g_cfg.oscPort) + ";";
                int n = 0;
                for (auto& o : g_cfg.outputs) {
                    char p[8];
                    sprintf_s(p, "out%d.", n++);
                    s += std::string(p) + "dev=" + shortDev(o.deviceName) + ";";
                    s += std::string(p) + "enabled=" + (o.enabled ? "1" : "0") + ";";
                    s += std::string(p) + "rows=" + std::to_string(o.rows) + ";";
                    s += std::string(p) + "cols=" + std::to_string(o.cols) + ";";
                    s += std::string(p) + "edges=" + edgesName(o.edges) + ";";
                    static const char* sd[4] = { "left", "right", "top", "bottom" };
                    for (int q = 0; q < 4; q++) {
                        sprintf_s(b, "%s%s.fade=%.3f;%s%s.gamma=%.3f;%s%s.gain=%.3f;%s%s.slope=%.3f;%s%s.chan=%.3f,%.3f,%.3f;",
                                  p, sd[q], o.blend[q].fade, p, sd[q], o.blend[q].gamma,
                                  p, sd[q], o.blend[q].gain, p, sd[q], o.blend[q].slope,
                                  p, sd[q], o.blend[q].chanR, o.blend[q].chanG, o.blend[q].chanB);
                        s += b;
                    }
                    const ColorAdj& c = o.color;
                    sprintf_s(b, "%scolor=%.3f,%.3f,%.3f,%.3f,%.3f;",
                              p, c.gammaR, c.gammaG, c.gammaB, c.bright, c.contrast);
                    s += b;
                    sprintf_s(b, "%smasks=%d", p, (int)o.masks.size());
                    s += b;
                    for (auto& m : o.masks) {
                        sprintf_s(b, " %.3f,%.3f,%.3f,%.3f", m.x, m.y, m.w, m.h);
                        s += b;
                    }
                    s += ";";
                }
            }
            s += std::string("calib=") + (g_calib.load() ? "on" : "off") + ";";
            s += "mode=" + std::string(g_calibMode.load() == 1 ? "blend" : "warp") + ";";
            s += std::string("snapshot=") + (g_snapshot.load() ? "on" : "off") + ";";
            s += std::string("probe=") + (g_probe.load() ? "on" : "off") + ";";
            s += std::string("perflog=") + (g_perfLog.load() ? "on" : "off") + ";";
            s += std::string("inplace=") + (g_inplace.load() ? "on" : "off") + ";";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                s += "testanim=" + std::to_string(g_cfg.testAnim) + ";";
                s += std::string("checker=") + (g_cfg.checker ? "on" : "off") + ";";
                s += "subdivide=" + std::to_string(g_cfg.subdivide) + ";";
                {
                    int m = g_moveMode.load();
                    s += std::string("move=") + (m == 1 ? "xy" : m == 2 ? "all" : "single") + ";";
                }
                char rb[64];
                sprintf_s(rb, "roi=%d,%d,%d,%d;", g_cfg.roi[0], g_cfg.roi[1], g_cfg.roi[2], g_cfg.roi[3]);
                s += rb;
                s += std::string("roicircle=") + (g_cfg.roiCircle ? "on" : "off") + ";";
                s += "roiborder=" + std::to_string(g_cfg.roiBorder) + ";";
            }
            s += std::string("testpause=") + (g_testPause.load() ? "on" : "off") + ";";
            s += std::string("autostart=") + (queryAutostart() ? "on" : "off") + ";";
            std::wstring src = currentSourceDevice();   // locks g_cfgMutex itself
            s += "source=" + (src.empty() ? std::string("auto") : shortDev(src)) + ";";
            std::string t = currentTarget();
            s += "out=" + (t.empty() ? std::string("auto") : t) + ";";
            return "OK " + s + "\n";
        }
        // mask rectangles of the command-target output, normalized 0..1:
        // "OK masks=2;0.000,0.000,0.100,0.100;0.500,0.500,0.200,0.200;"
        if (k == "masks") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            OutputCfg* o = targetOutputLocked();
            if (!o) return "ERR no output (SET out=<name>?)\n";
            char b[40];
            sprintf_s(b, "OK masks=%d;", (int)(std::min)(o->masks.size(), (size_t)MAX_MASKS));
            std::string s = b;
            for (auto& m : o->masks) {
                sprintf_s(b, "%.3f,%.3f,%.3f,%.3f;", m.x, m.y, m.w, m.h);
                s += b;
            }
            return s + "\n";
        }
        if (k == "color") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            OutputCfg* o = targetOutputLocked();
            if (!o) return "ERR no output (SET out=<name>?)\n";
            char b[80];
            sprintf_s(b, "OK color=%.3f,%.3f,%.3f,%.3f,%.3f\n",
                      o->color.gammaR, o->color.gammaG, o->color.gammaB,
                      o->color.bright, o->color.contrast);
            return b;
        }
        if (k == "presets") return listPresets();
        // warp grid dump: "GET warp" lists every dest point as "x,y" pairs
        // (space separated, row-major), "GET warp=r,c" reads one point.
        if (k == "warp" || k.compare(0, 5, "warp=") == 0) {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            OutputCfg* o = targetOutputLocked();
            if (!o) return "ERR no output (SET out=<name>?)\n";
            char b[40];
            if (k.size() > 5) {
                int r, c;
                if (sscanf_s(k.c_str() + 5, "%d,%d", &r, &c) != 2)
                    return "ERR GET warp (want warp or warp=r,c)\n";
                if (r < 0 || r >= o->rows || c < 0 || c >= o->cols)
                    return "ERR GET warp (row %d col %d out of %dx%d grid)\n";
                size_t i = ((size_t)r * o->cols + c) * 2;
                sprintf_s(b, "OK point=%d,%d,%.4f,%.4f\n", r, c, o->points[i], o->points[i + 1]);
                return b;
            }
            sprintf_s(b, "OK warp=%d;%d;", o->rows, o->cols);
            std::string s = b;
            for (size_t i = 0; i < o->points.size(); i += 2) {
                sprintf_s(b, " %.4f,%.4f", o->points[i], o->points[i + 1]);
                s += b;
            }
            return s + "\n";
        }
        if (k == "rows" || k == "cols") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            OutputCfg* o = targetOutputLocked();
            if (!o) return "ERR no output (SET out=<name>?)\n";
            return "OK " + k + "=" + std::to_string(k == "rows" ? o->rows : o->cols) + "\n";
        }
        if (k == "source") {
            std::wstring cur = currentSourceDevice();
            return "OK source=" + (cur.empty() ? std::string("auto") : shortDev(cur)) + "\n";
        }
        if (k == "displays") {
            // topology listing so a show controller can pick a source:
            // OK displays=2;DISPLAY1=1920x1080*;DISPLAY2=1920x1080;
            auto ds = enumerateDisplays();
            std::string s = "OK displays=" + std::to_string(ds.size()) + ";";
            for (auto& d : ds) {
                s += shortDev(d.deviceName) + "=" +
                     std::to_string(d.desktop.right - d.desktop.left) + "x" +
                     std::to_string(d.desktop.bottom - d.desktop.top) +
                     (d.primary ? "*" : "") + ";";
            }
            return s + "\n";
        }
        if (k == "testpattern") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            return std::string("OK testpattern=") + (g_cfg.testPattern ? "on" : "off") + "\n";
        }
        if (k == "outputs") {
            // topology of the enabled outputs: the count first (old parsers
            // read just that), then each output's short device name:
            // OK outputs=2;DISPLAY1;DISPLAY2;
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            int n = 0;
            std::string s;
            for (auto& o : g_cfg.outputs) if (o.enabled) {
                n++;
                s += shortDev(o.deviceName) + ";";
            }
            return "OK outputs=" + std::to_string(n) + ";" + s + "\n";
        }
        if (k == "edges") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            OutputCfg* o = targetOutputLocked();
            if (!o) return "ERR no output (SET out=<name>?)\n";
            return "OK edges=" + edgesName(o->edges) + "\n";
        }
        static const char* sides[4] = { "left", "right", "top", "bottom" };
        for (int s = 0; s < 4; s++) {
            std::string pfx = std::string(sides[s]) + ".";
            if (k.compare(0, pfx.size(), pfx) == 0) {
                std::string field = k.substr(pfx.size());
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = targetOutputLocked();
                if (!o) return "ERR no output (SET out=<name>?)\n";
                char b[64];
                if (field == "fade")  { sprintf_s(b, "OK %s.fade=%.3f\n",  sides[s], o->blend[s].fade);  return b; }
                if (field == "gamma") { sprintf_s(b, "OK %s.gamma=%.3f\n", sides[s], o->blend[s].gamma); return b; }
                if (field == "gain")  { sprintf_s(b, "OK %s.gain=%.3f\n",  sides[s], o->blend[s].gain);  return b; }
                if (field == "slope") { sprintf_s(b, "OK %s.slope=%.3f\n", sides[s], o->blend[s].slope); return b; }
                if (field == "chanr") { sprintf_s(b, "OK %s.chanr=%.3f\n", sides[s], o->blend[s].chanR); return b; }
                if (field == "chang") { sprintf_s(b, "OK %s.chang=%.3f\n", sides[s], o->blend[s].chanG); return b; }
                if (field == "chanb") { sprintf_s(b, "OK %s.chanb=%.3f\n", sides[s], o->blend[s].chanB); return b; }
                return "ERR GET " + k + "\n";
            }
        }
        return "ERR GET " + arg + "\n";
    }

    if (cmd == "STATUS") {
        // one-shot summary for a show controller: the five original keys plus
        // layout/overlap/curved/masks/testpattern/source. Keys are appended at
        // the end, so older parsers reading "key=value;" pairs keep working.
        int edges = 15, outs = 0, masks = 0;
        int layoutC = 1, layoutR = 1, overlap = 0, eng = 0, testAnim = 0, checker = 0, subd = 8;
        int maxFps = 0;
        int roiL = 0, roiT = 0, roiR = 0, roiB = 0, roiCir = 0, roiBrd = 0;
        int oscCfg = 0, oscPortV = 8000;
        bool curved = false, tp = false;
        bool hookOn = hookIpcEnabled();
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            layoutC = g_cfg.layoutC; layoutR = g_cfg.layoutR; overlap = g_cfg.overlap;
            curved = g_cfg.curved != 0; tp = g_cfg.testPattern;
            eng = g_cfg.engine; testAnim = g_cfg.testAnim; checker = g_cfg.checker;
            subd = g_cfg.subdivide;
            maxFps = g_cfg.maxfps;
            roiL = g_cfg.roi[0]; roiT = g_cfg.roi[1]; roiR = g_cfg.roi[2]; roiB = g_cfg.roi[3];
            roiCir = g_cfg.roiCircle; roiBrd = g_cfg.roiBorder;
            oscCfg = g_cfg.osc; oscPortV = g_cfg.oscPort;
            for (auto& o : g_cfg.outputs) if (o.enabled) {
                outs++;
                if (outs == 1) { edges = o.edges; masks = (int)o.masks.size(); }
            }
        }
        std::wstring src = currentSourceDevice();   // locks g_cfgMutex itself
        std::string tgt = currentTarget();          // independent small mutex
        return "calib=" + std::string(g_calib.load() ? "on" : "off") +
               ";mode=" + std::string(g_calibMode.load() == 1 ? "blend" : "warp") +
               ";edges=" + edgesName(edges) +
               ";snapshot=" + std::string(g_snapshot.load() ? "on" : "off") +
               ";outputs=" + std::to_string(outs) +
               ";layout=" + std::to_string(layoutC) + "x" + std::to_string(layoutR) +
               ";overlap=" + std::to_string(overlap) +
               ";curved=" + std::string(curved ? "on" : "off") +
               ";masks=" + std::to_string(masks) +
               ";testpattern=" + std::string(tp ? "on" : "off") +
               ";source=" + (src.empty() ? std::string("auto") : shortDev(src)) +
               ";out=" + (tgt.empty() ? std::string("auto") : tgt) +
               ";probe=" + std::string(g_probe.load() ? "on" : "off") +
               ";perflog=" + std::string(g_perfLog.load() ? "on" : "off") +
               ";inplace=" + std::string(g_inplace.load() ? "on" : "off") +
               ";blackout=" + std::string(g_blackout.load() ? "on" : "off") +
               ";testanim=" + std::to_string(testAnim) +
               ";testpause=" + std::string(g_testPause.load() ? "on" : "off") +
               ";checker=" + std::string(checker ? "on" : "off") +
               ";subdivide=" + std::to_string(subd) +
               ";maxfps=" + std::to_string(maxFps) +
               std::string(";hook=") + (hookOn ? "on" : "off") +
               std::string(";hookovl=") + (hookIpcOverlay() ? "on" : "off") +
               ";move=" + std::string(g_moveMode.load() == 1 ? "xy" : g_moveMode.load() == 2 ? "all" : "single") +
               ";roi=" + std::to_string(roiL) + "," + std::to_string(roiT) + "," +
                          std::to_string(roiR) + "," + std::to_string(roiB) +
               ";roicircle=" + std::string(roiCir ? "on" : "off") +
               ";roiborder=" + std::to_string(roiBrd) +
               ";osc=" + std::string(oscRunning() || oscCfg ? "on" : "off") +
               ";oscport=" + std::to_string(oscPortV) +
               ";engine=" + std::string(eng == 1 ? "mag" : "desktopduplication") + "\n";
    }

    // in-place overlay: fullscreen click-through output over the desktop (the
    // user-mode stand-in for DWM desktop warping). Toggling rebuilds the
    // output windows on the UI thread (WM_INPLACESET).
    if (cmd == "INPLACE") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_inplace.load();
        g_inplace.store(want);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.inplace = want;
            g_rev++;
        }
        requestInplaceRestyle();
        return std::string("OK inplace=") + (g_inplace.load() ? "on" : "off") + "\n";
    }

    if (cmd == "CALIB") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_calib.load();
        g_calib.store(want);
        // hotkeys belong to the UI thread's message loop - never register
        // them here (WM_HOTKEY would land in the socket thread's queue)
        requestCalibrationSync();
        refreshStatusBar();
        return std::string("OK calib=") + (g_calib.load() ? "on" : "off") + "\n";
    }

    // show blackout: every output renders full black (View menu / lighting
    // desk convention). Toggle semantics; deliberately not persisted.
    if (cmd == "BLACKOUT") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_blackout.load();
        g_blackout.store(want);
        requestBlackoutSync();   // menu checkbox + MAG overlays follow now
        refreshStatusBar();
        return std::string("OK blackout=") + (g_blackout.load() ? "on" : "off") + "\n";
    }

    if (cmd == "TESTPAUSE") {
        // freeze the dynamic test pattern bands (original's Animation/Paused)
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_testPause.load();
        g_testPause.store(want);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);   // no lock held here
            refreshAnimUI(g_cfg.testAnim, want);   // Image-page checkbox follows
        }
        return std::string("OK testpause=") + (g_testPause.load() ? "on" : "off") + "\n";
    }

    if (cmd == "MODE") {
        if (arg != "WARP" && arg != "BLEND") return "ERR MODE WARP|BLEND\n";
        if (!g_calib.load()) {             // mode only exists inside calibration
            g_calib.store(true);
            requestCalibrationSync();
        }
        int want = (arg == "BLEND") ? 1 : 0;
        if (g_calibMode.load() != want) switchSubModeAll();
        return std::string("OK mode=") + (g_calibMode.load() == 1 ? "blend" : "warp") + "\n";
    }

    if (cmd == "EDGES") {
        int m = edgesMask(arg);
        if (m < 0) return "ERR EDGES ALL|L|R|T|B|NONE\n";
        setBlendEdgesMask(m);
        return "OK edges=" + edgesName(m) + "\n";
    }

    if (cmd == "SNAPSHOT") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_snapshot.load();
        if (g_snapshot.load() != want) toggleSnapshot();
        return std::string("OK snapshot=") + (g_snapshot.load() ? "on" : "off") + "\n";
    }

    // camera alignment probe markers (white crosses at the warp dest points).
    // PROBE (no arg) toggles; PROBE ON/OFF sets explicitly.
    if (cmd == "PROBE") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_probe.load();
        g_probe.store(want);
        requestMagProbeSync();   // engine=1: overlays follow on the UI thread
        return std::string("OK probe=") + (g_probe.load() ? "on" : "off") + "\n";
    }

    if (cmd == "SAVE") return saveConfig(L"") ? "OK saved\n" : "ERR SAVE\n";

    // periodic perf snapshots into debug.log (PERFLOG ON/OFF/toggle)
    if (cmd == "PERFLOG") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_perfLog.load();
        g_perfLog.store(want);
        return std::string("OK perflog=") + (g_perfLog.load() ? "on" : "off") + "\n";
    }

    // shared tail of LOAD / LOADAS: resync the state the UI's Load button also
    // refreshes. loadConfig has already released g_cfgMutex by the time this
    // runs, and refreshStatusBar locks it itself -> call it unlocked.
    // sigBefore = enabled-output signature captured BEFORE loadConfig: when
    // the preset retargets the projector rig the output windows must be
    // rebuilt live (Sets hot switching; the handler no-ops when nothing runs)
    auto finishLoad = [&](const std::string& what, const std::wstring& sigBefore) -> std::string {
        g_previewMode.store(g_cfg.previewMode);
        g_inplace.store(g_cfg.inplace);
        requestInplaceRestyle();   // restyle outputs
        requestMagTestSync();      // engine=1 test-pattern overlays follow
        requestMagProbeSync();     // engine=1 probe-marker overlays follow
        Engine::inst().setSampling(g_cfg.sampling);
        Engine::inst().setTestImage(g_cfg.testImg);   // <0 clears the overlay
        if (!g_cfg.imagePath.empty()) engineLoadImage(g_cfg.imagePath);
        refreshSampComboUI();   // engine/sampling dropdowns follow loaded config
        refreshMaxFpsUI();      // Settings max-fps combo follows loaded config
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            refreshAnimUI(g_cfg.testAnim, g_testPause.load());   // animation combo follows
            refreshCheckerUI(g_cfg.checker != 0);                // checker checkbox follows
        }
        refreshRoiUI();   // ROI edits follow the loaded config
        int outs = 0;
        refreshStatusBar();
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            for (auto& o : g_cfg.outputs) if (o.enabled) outs++;
        }
        if (enabledOutputsSignature() != sigBefore) requestOutputsRebuild();
        return "OK loaded " + what + ";outputs=" + std::to_string(outs) + "\n";
    };

    if (cmd == "LOAD") {
        std::wstring sigBefore = enabledOutputsSignature();
        if (!loadConfig(L"")) return "ERR LOAD (no config file)\n";
        return finishLoad("default", sigBefore);
    }
    if (cmd == "SAVEAS" || cmd == "LOADAS" || cmd == "DELPRESET") {
        // preset name: A-Z 0-9 '_' '-' only (dispatch uppercases the whole
        // line), max 32 chars -> no path traversal possible
        bool okName = !arg.empty() && arg.size() <= 32;
        for (char c : arg)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                okName = false;
        if (!okName) return "ERR " + cmd + " name (A-Z 0-9 _ -, max 32)\n";
        if (cmd == "SAVEAS") {
            if (!saveConfig(presetPath(arg))) return "ERR SAVEAS (write failed)\n";
            return "OK saved preset=" + arg + "\n";
        }
        if (cmd == "DELPRESET") {
            if (!DeleteFileW(presetPath(arg).c_str())) return "ERR DELPRESET (no such preset)\n";
            return "OK deleted preset=" + arg + "\n";
        }
        std::wstring sigBefore = enabledOutputsSignature();
        if (!loadConfig(presetPath(arg))) return "ERR LOADAS (no such preset)\n";
        return finishLoad("preset=" + arg, sigBefore);
    }

    if (cmd == "AUTOSTART") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !queryAutostart();
        setAutostart(want);
        return std::string("OK autostart=") + (queryAutostart() ? "on" : "off") + "\n";
    }

    // SET <side>.<field>=<value>  e.g. "SET left.fade=0.2" / "SET testpattern=off"
    // A value-less form ("SET mask.clear") is also accepted for actions that
    // take no argument; keys that need a value reject an empty val themselves.
    if (cmd == "SET") {
        size_t eq = arg.find('=');
        std::string key = (eq == std::string::npos) ? arg : arg.substr(0, eq);
        std::string val = (eq == std::string::npos) ? "" : arg.substr(eq + 1);
        if (key.empty()) return "ERR SET side.field=value\n";
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        // tolerate stray whitespace around '=' (OSC joins args with spaces,
        // and some TCP clients pad too): "SET perflog= ON" must equal ON
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        // Set the capture source: rebuilds the outputs, so it must run without
        // holding g_cfgMutex (setSourceDisplay locks internally).
        if (key == "source") {
            std::wstring dev;
            if (val != "AUTO" && !val.empty()) {
                dev.assign(val.begin(), val.end());
                if (dev.compare(0, 4, L"\\\\.\\") != 0) dev = L"\\\\.\\" + dev;
            }
            setSourceDisplay(dev);
            std::wstring cur = currentSourceDevice();
            return "OK source=" + (cur.empty() ? std::string("auto") : shortDev(cur)) + "\n";
        }
        // Per-output capture source override (OutputCfg.srcOverride). Targets
        // the SET out=<name> output, or every enabled output without one.
        // "auto"/"clear" removes the override (follows the global pin).
        // Rebuilds the outputs -> run without holding g_cfgMutex.
        if (key == "src") {
            std::wstring dev;
            if (val != "AUTO" && val != "CLEAR" && !val.empty()) {
                dev.assign(val.begin(), val.end());
                if (dev.compare(0, 4, L"\\\\.\\") != 0) dev = L"\\\\.\\" + dev;
                bool known = false;
                for (auto& d : enumerateDisplays())
                    if (sameDisplayName(d.deviceName, dev)) { known = true; break; }
                if (!known) return "ERR SET src (no such display: " + val + ")\n";
            }
            int n = 0;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                std::string t = currentTarget();
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (!t.empty() &&
                        _stricmp(shortDev(o.deviceName).c_str(), t.c_str()) != 0 &&
                        _stricmp(toNarrow(o.label).c_str(), t.c_str()) != 0)
                        continue;
                    o.srcOverride = dev;
                    n++;
                }
                if (n) g_rev++;
            }
            if (!n) return "ERR no output (SET out=<name>?)\n";
            requestSourceRebuild();
            requestOvrSrcSync();     // Displays-page override combo follows
            refreshStatusBar();
            return "OK src=" + (dev.empty() ? std::string("auto") : shortDev(dev)) +
                   " on " + std::to_string(n) + " output(s)\n";
        }
        // Point the following per-output commands at one output. AUTO (or an
        // empty value) restores the legacy defaults. The name is validated so
        // a typo fails here instead of on the next command.
        if (key == "out") {
            if (val == "AUTO" || val.empty()) {
                cmdSetTarget("");
                refreshTargetComboUI();   // keep the console dropdown in sync
                return "OK out=auto\n";
            }
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                bool found = false;
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (_stricmp(shortDev(o.deviceName).c_str(), val.c_str()) == 0 ||
                        _stricmp(toNarrow(o.label).c_str(), val.c_str()) == 0)
                        { found = true; break; }
                }
                if (!found) return "ERR SET out (no such output: " + val + ")\n";
            }
            cmdSetTarget(val);
            refreshTargetComboUI();
            return "OK out=" + val + "\n";
        }
        // NxN tiled layout, e.g. "SET layout=2x2" / "SET overlap=15"
        // (setLayout locks g_cfgMutex itself; the grid rebuild bumps g_rev so
        // the render thread rebuilds every mesh on the next frame).
        // NOTE: dispatch() uppercases the whole line -> val arrives as "2X2"
        if (key == "layout") {
            int C = 0, R = 0;
            if (sscanf_s(val.c_str(), "%dX%d", &C, &R) != 2 &&
                sscanf_s(val.c_str(), "%dx%d", &C, &R) != 2)
                return "ERR SET layout (want WxH)\n";
            if (C < 1 || R < 1 || C > 4 || R > 4)
                return "ERR SET layout (each 1..4)\n";
            setLayout(R, C, g_cfg.overlap);
            return "OK layout=" + std::to_string(C) + "x" + std::to_string(R) + "\n";
        }
        if (key == "overlap") {
            int ov = atoi(val.c_str());
            if (ov < 0 || ov > 40) return "ERR SET overlap (want 0..40)\n";
            setLayout(g_cfg.layoutR, g_cfg.layoutC, ov);
            return "OK overlap=" + std::to_string(ov) + "\n";
        }
        // image sampling quality: 0 trilinear, 1/2 anisotropic, 3 bi-cubic
        // (DXGI engine only - MAG composites on the DWM and has no shader)
        if (key == "sampling") {
            int q = atoi(val.c_str());
            if (val.empty() || q < 0 || q > 3)
                return "ERR SET sampling (0..3)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.sampling = q;
                g_rev++;
            }
            Engine::inst().setSampling(q);   // atomic publish, no rebuild needed
            refreshSampComboUI();            // keep the console dropdown in sync
            return "OK sampling=" + std::to_string(q) + "\n";
        }
        // output frame cap: 0 = uncapped, else the render loop holds each
        // frame until its slot is due (power/heat friendly on site). MAG has
        // no render loop - the cap is a DXGI-engine setting.
        if (key == "maxfps") {
            int v = atoi(val.c_str());
            // reject non-numeric values: atoi("ABC") is 0, which would
            // silently lift the cap instead of failing the command
            if (val.empty() || val.find_first_not_of("0123456789") != std::string::npos
                || v > 240)
                return "ERR SET maxfps (0..240)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.maxfps = v;
            }
            refreshMaxFpsUI();   // keep the Settings combo in sync
            return "OK maxfps=" + std::to_string(v) + "\n";
        }
        // scanout pipeline: warp params for the injected WarpHook.dll.
        // hook=on flips the enable flag; hooktest also installs a fixed
        // corner displacement so the in-place warp is visible on screen.
        if (key == "hook") {
            bool on;
            if (val == "1" || val == "ON" || val == "TRUE") on = true;
            else if (val == "0" || val == "OFF" || val == "FALSE") on = false;
            else return "ERR SET hook (on|off)\n";
            hookIpcSet(on, nullptr, 0);
            return std::string("OK hook=") + (on ? "on" : "off") + "\n";
        }
        if (key == "hookovl") {
            // DLL-side calibration overlay: draw the warp mesh as lines
            bool on;
            if (val == "1" || val == "ON" || val == "TRUE") on = true;
            else if (val == "0" || val == "OFF" || val == "FALSE") on = false;
            else return "ERR SET hookovl (on|off)\n";
            hookIpcSetOverlay(on);
            return std::string("OK hookovl=") + (on ? "on" : "off") + "\n";
        }
        if (key == "hooktest") {
            bool on;
            if (val == "1" || val == "ON" || val == "TRUE") on = true;
            else if (val == "0" || val == "OFF" || val == "FALSE") on = false;
            else return "ERR SET hooktest (on|off)\n";
            // fixed trapezoid: right edge pushed +0.25 NDC right, left edge
            // pulled -0.25 - clearly visible on any content, still on-screen
            static const float testCorners[4][2] = {
                { -0.25f, 0.0f }, { 0.25f, 0.0f },   // TL, TR
                {  0.0f,  0.0f }, { 0.0f,  0.0f },   // BL, BR
            };
            hookIpcSet(on, on ? testCorners : nullptr, 0);
            return std::string("OK hooktest=") + (on ? "on" : "off") + "\n";
        }
        if (key == "engine") {
            // MAG mode is a different kind of output window: the shared
            // state flips here, the UI thread rebuilds the windows (empty
            // selection means toggle-back to 0 - idempotent either way).
            int want = -1;
            if (val == "0" || val == "DXGI" || val == "DESKTOPDUPLICATION") want = 0;
            else if (val == "1" || val == "MAG" || val == "MAGNIFICATION" || val == "DWM") want = 1;
            if (want < 0) return "ERR SET engine (desktopduplication or mag)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.engine = want;
                g_rev++;
            }
            requestOutputRebuild();
            refreshEngineComboUI();
            return std::string("OK engine=") +
                   (want == 1 ? "mag" : "desktopduplication") + "\n";
        }
        // manual rebuild of the output windows - the same deferred path
        // WM_DISPLAYCHANGE takes (also handy after a dock/undock on-site)
        if (key == "rebuild") {
            if (val != "1" && val != "ON" && val != "TRUE")
                return "ERR SET rebuild (1 or ON)\n";
            requestOutputRebuild();
            return "OK rebuild\n";
        }
        // curved vs bilinear mesh interpolation (Catmull-Rom needs 4x4+ grids)
        if (key == "curved") {
            if (val != "ON" && val != "OFF" && val != "1" && val != "0")
                return "ERR SET curved (ON or OFF)\n";
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.curved = (val == "ON" || val == "1") ? 1 : 0;
            g_rev++;   // render thread rebuilds every mesh on the next frame
            return std::string("OK curved=") + (g_cfg.curved ? "on" : "off") + "\n";
        }
        // black-mask rectangles on the first enabled output, normalized 0..1:
        // "SET mask.add=x,y,w,h" / "SET mask.set=i,x,y,w,h" / "SET mask.del=i"
        // / "SET mask.clear". The UI edits the same vector, and the renderer
        // uploads masks in its per-frame constant buffer, so changes take
        // effect on the next frame with no explicit revision bump.
        if (key.rfind("mask.", 0) == 0) {
            std::string sub = key.substr(5);
            float x, y, w, h;
            int idx;
            if (sub == "clear") {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = targetOutputLocked();
                if (!o) return "ERR no output (SET out=<name>?)\n";
                o->masks.clear();
                return "OK masks=0\n";
            }
            if (sub == "add" || sub == "set") {
                bool ok = sub == "add"
                    ? sscanf_s(val.c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) == 4
                    : sscanf_s(val.c_str(), "%d,%f,%f,%f,%f", &idx, &x, &y, &w, &h) == 5;
                if (!ok)
                    return "ERR SET mask." + sub + " (want " +
                           (sub == "add" ? "x,y,w,h" : "i,x,y,w,h") + " in 0..1)\n";
                if (x < 0 || x > 1 || y < 0 || y > 1 || w < 0 || w > 1 || h < 0 || h > 1)
                    return "ERR SET mask." + sub + " (values 0..1)\n";
                std::string rep;
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    OutputCfg* o = targetOutputLocked();
                    if (!o) return "ERR no output (SET out=<name>?)\n";
                    // fit the rectangle so it stays on screen
                    w = (std::min)(w, 1.0f - x); h = (std::min)(h, 1.0f - y);
                    if (sub == "add") {
                        if (o->masks.size() >= MAX_MASKS)
                            return "ERR SET mask.add (max 8 masks)\n";
                        o->masks.push_back(MaskRect{ x, y, w, h });
                    } else {
                        if (idx < 0 || idx >= (int)o->masks.size())
                            return "ERR SET mask.set (bad index)\n";
                        o->masks[idx] = MaskRect{ x, y, w, h };
                    }
                    rep = "OK masks=" + std::to_string(o->masks.size()) + "\n";
                }
                refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
                return rep;
            }
            // batch import, replacing the whole list: rects are separated by
            // SPACES because ';' terminates the command on the wire
            if (sub == "load") {
                std::vector<MaskRect> add;
                size_t pos = 0;
                while (pos <= val.size()) {
                    size_t sp = val.find(' ', pos);
                    std::string t = val.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                    if (!t.empty()) {
                        float mx, my, mw, mh;
                        if (sscanf_s(t.c_str(), "%f,%f,%f,%f", &mx, &my, &mw, &mh) != 4 ||
                            mx < 0 || mx > 1 || my < 0 || my > 1 || mw < 0 || mw > 1 || mh < 0 || mh > 1)
                            return "ERR SET mask.load (rects x,y,w,h in 0..1, space separated)\n";
                        mw = (std::min)(mw, 1.0f - mx); mh = (std::min)(mh, 1.0f - my);
                        add.push_back(MaskRect{ mx, my, mw, mh });
                    }
                    if (sp == std::string::npos) break;
                    pos = sp + 1;
                }
                if (add.size() > MAX_MASKS)
                    return "ERR SET mask.load (max 8 masks)\n";
                std::string rep;
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    OutputCfg* o = targetOutputLocked();
                    if (!o) return "ERR no output (SET out=<name>?)\n";
                    o->masks = add;
                    rep = "OK masks=" + std::to_string(o->masks.size()) + "\n";
                }
                refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
                return rep;
            }
            if (sub == "del") {
                if (sscanf_s(val.c_str(), "%d", &idx) != 1)
                    return "ERR SET mask.del (want i)\n";
                std::string rep;
                {
                    std::lock_guard<std::mutex> lk(g_cfgMutex);
                    OutputCfg* o = targetOutputLocked();
                    if (!o) return "ERR no output (SET out=<name>?)\n";
                    if (idx < 0 || idx >= (int)o->masks.size())
                        return "ERR SET mask.del (bad index)\n";
                    o->masks.erase(o->masks.begin() + idx);
                    rep = "OK masks=" + std::to_string(o->masks.size()) + "\n";
                }
                refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
                return rep;
            }
            return "ERR SET mask (add|set|load|del|clear)\n";
        }
        // per-output color adjustment (gamma RGB + brightness/contrast).
        // Same clamps as the color dialog's sliders: bright/contrast 0.2..2.0,
        // gamma 0.2..3.0. Targets the selected output, else the first enabled.
        if (key == "color") {
            float gr, gg, gb, br, ct;
            if (sscanf_s(val.c_str(), "%f,%f,%f,%f,%f", &gr, &gg, &gb, &br, &ct) != 5)
                return "ERR SET color (gammaR,gammaG,gammaB,bright,contrast)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = targetOutputLocked();
                if (!o) return "ERR no output (SET out=<name>?)\n";
                o->color.gammaR  = clampf(gr, 0.2f, 3.0f);
                o->color.gammaG  = clampf(gg, 0.2f, 3.0f);
                o->color.gammaB  = clampf(gb, 0.2f, 3.0f);
                o->color.bright  = clampf(br, 0.2f, 2.0f);
                o->color.contrast = clampf(ct, 0.2f, 2.0f);
                g_rev++;
            }
            refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
            return "OK color=" + val + "\n";
        }
        // single warp control point: "SET point=r,c,x,y" moves the dest
        // position of one grid node (row-major, 0-based). Dest points may go
        // slightly outside [0..1] (keystone), clamped to -0.5..1.5 like the
        // drag handler's working range. Next-frame effect via g_rev++.
        if (key == "point") {
            int r, c;
            float x, y;
            if (sscanf_s(val.c_str(), "%d,%d,%f,%f", &r, &c, &x, &y) != 4)
                return "ERR SET point (want r,c,x,y)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = targetOutputLocked();
                if (!o) return "ERR no output (SET out=<name>?)\n";
                if (r < 0 || r >= o->rows || c < 0 || c >= o->cols)
                    return "ERR SET point (row %d col %d out of %dx%d grid)\n";
                size_t i = ((size_t)r * o->cols + c) * 2;
                o->points[i]     = clampf(x, -0.5f, 1.5f);
                o->points[i + 1] = clampf(y, -0.5f, 1.5f);
                g_rev++;
            }
            refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
            return "OK point=" + val + "\n";
        }
        // batch grid import, replacing the whole dest-point list: "x,y"
        // pairs separated by SPACES (';' terminates the wire command), count
        // must match rows*cols of the target output. Pairs with GET warp for
        // snapshot/restore and camera-feedback auto-alignment loops.
        if (key == "warp.load") {
            std::vector<float> pts;
            size_t pos = 0;
            bool bad = false;
            while (pos <= val.size()) {
                size_t sp = val.find(' ', pos);
                std::string t = val.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                if (!t.empty()) {
                    float px, py;
                    if (sscanf_s(t.c_str(), "%f,%f", &px, &py) != 2) { bad = true; break; }
                    pts.push_back(clampf(px, -0.5f, 1.5f));
                    pts.push_back(clampf(py, -0.5f, 1.5f));
                }
                if (sp == std::string::npos) break;
                pos = sp + 1;
            }
            if (bad) return "ERR SET warp.load (want x,y pairs, space separated)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                OutputCfg* o = targetOutputLocked();
                if (!o) return "ERR no output (SET out=<name>?)\n";
                if (pts.size() != o->points.size())
                    return "ERR SET warp.load (want " +
                           std::to_string(o->rows * o->cols) + " points, got " +
                           std::to_string(pts.size() / 2) + ")\n";
                o->points = pts;
                g_rev++;
            }
            refreshStatusBar();   // locks g_cfgMutex itself: call unlocked
            return "OK warp.load=" + std::to_string(pts.size() / 2) + "\n";
        }
        // grid density (REBUILDS each warp grid, dropping manual point edits,
        // same as the Configure dialog's Apply). Targets the selected output
        // when one is set, else every enabled output (legacy).
        if (key == "snap") {
            // frame snapshot to PNG: 1/on -> %APPDATA%\...\snapshots\<ts>.png,
            // or a custom path. Runs on the render thread (async); GET snap
            // reports the last written file. Named "snap" because "SNAPSHOT"
            // already means the calib freeze toggle. dispatch uppercased the
            // line, so a custom path arrives uppercase - Windows paths are
            // case-insensitive but the directory must already exist.
            std::wstring path;
            if (val == "1" || val == "ON" || val == "TRUE" || val.empty()) {
                path = snapshotDefaultPath();
            } else {
                path.assign(val.begin(), val.end());
                if (path.rfind(L".png") != path.size() - 4) path += L".png";
            }
            {
                std::lock_guard<std::mutex> lk(g_snapMutex);
                g_snapPath = path;
                g_lastSnap.clear();   // in-flight marker: GET snap polls for none -> path
            }
            if (g_cfg.engine == 1) {
                // MAG engine: no render thread to consume g_snapReq - the
                // snapshot is a GDI grab of the magnifier hosts on the UI thread
                requestMagSnapshot();
            } else {
                g_snapReq.store(1);
            }
            return "OK snap=requested\n";
        }
        if (key == "rows" || key == "cols") {
            int n = atoi(val.c_str());
            if (n < 3 || n > 24) return "ERR SET " + key + " (3..24)\n";
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                std::string t = currentTarget();
                bool found = false;
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (!t.empty() &&
                        _stricmp(shortDev(o.deviceName).c_str(), t.c_str()) != 0 &&
                        _stricmp(toNarrow(o.label).c_str(), t.c_str()) != 0)
                        continue;
                    found = true;
                    if (key == "rows") o.rows = n; else o.cols = n;
                    o.initIdentity();
                }
                if (!found) return "ERR no output (SET out=<name>?)\n";
                g_rev++;
            }
            refreshStatusBar();
            requestMagProbeSync();   // engine=1: probe grid follows the density
            return "OK " + key + "=" + val + "\n";
        }
        // grid transforms on every enabled output (same math as the dialog)
        if (key == "flip") {
            if (val != "H" && val != "V") return "ERR SET flip (H or V)\n";
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                std::string t = currentTarget();
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (!t.empty() &&
                        _stricmp(shortDev(o.deviceName).c_str(), t.c_str()) != 0 &&
                        _stricmp(toNarrow(o.label).c_str(), t.c_str()) != 0)
                        continue;
                    found = true;
                    for (size_t i = 0; i < o.points.size(); i += 2) {
                        if (val == "H") o.points[i] = 1.0f - o.points[i];
                        else            o.points[i + 1] = 1.0f - o.points[i + 1];
                    }
                }
                if (found) g_rev++;
            }
            if (!found) return "ERR no output (SET out=<name>?)\n";
            refreshStatusBar();
            return "OK flip=" + val + "\n";
        }
        if (key == "rotate") {
            if (val != "90" && val != "-90" && val != "270")
                return "ERR SET rotate (90, -90 or 270)\n";
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                std::string t = currentTarget();
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (!t.empty() &&
                        _stricmp(shortDev(o.deviceName).c_str(), t.c_str()) != 0 &&
                        _stricmp(toNarrow(o.label).c_str(), t.c_str()) != 0)
                        continue;
                    found = true;
                    for (size_t i = 0; i < o.points.size(); i += 2) {
                        float x = o.points[i], y = o.points[i + 1];
                        if (val == "90")  { o.points[i] = 1.0f - y; o.points[i + 1] = x; }
                        else              { o.points[i] = y; o.points[i + 1] = 1.0f - x; }
                    }
                }
                if (found) g_rev++;
            }
            if (!found) return "ERR no output (SET out=<name>?)\n";
            refreshStatusBar();
            return "OK rotate=" + val + "\n";
        }
        static const char* sides[4] = { "left", "right", "top", "bottom" };
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            bool applied = false;
            std::string t = currentTarget();
            for (int s = 0; s < 4 && !applied; s++) {
                std::string pfx = std::string(sides[s]) + ".";
                if (key.compare(0, pfx.size(), pfx) != 0) continue;
                std::string field = key.substr(pfx.size());
                float v = (float)atof(val.c_str());
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    // targeted: only the selected output; auto: every output
                    if (!t.empty() &&
                        _stricmp(shortDev(o.deviceName).c_str(), t.c_str()) != 0 &&
                        _stricmp(toNarrow(o.label).c_str(), t.c_str()) != 0)
                        continue;
                    if (field == "fade")  { o.blend[s].fade  = clampf(v, 0.0f, 0.5f); applied = true; }
                    if (field == "gamma") { o.blend[s].gamma = clampf(v, 0.1f, 5.0f); applied = true; }
                    if (field == "gain")  { o.blend[s].gain  = clampf(v, 0.0f, 2.0f); applied = true; }
                    if (field == "slope") { o.blend[s].slope = clampf(v, 0.0f, 1.0f); applied = true; }
                    if (field == "chanr") { o.blend[s].chanR = clampf(v, 0.1f, 5.0f); applied = true; }
                    if (field == "chang") { o.blend[s].chanG = clampf(v, 0.1f, 5.0f); applied = true; }
                    if (field == "chanb") { o.blend[s].chanB = clampf(v, 0.1f, 5.0f); applied = true; }
                }
            }
            if (!applied && key == "testpattern") {
                g_cfg.testPattern = (val == "ON" || val == "1" || val == "true");
                applied = true;
                requestMagTestSync();   // engine=1: GDI overlays follow
            }
            if (!applied && key == "testanim") {
                int v = atoi(val.c_str());
                if (v < 0 || v > 3) return "ERR SET testanim (want 0..3)\n";
                g_cfg.testAnim = v;
                applied = true;
                refreshAnimUI(v, g_testPause.load());   // Image-page combo follows
            }
            if (!applied && key == "testpause") {
                bool want = (val == "ON" || val == "1" || val == "true");
                g_testPause.store(want);
                applied = true;
                refreshAnimUI(g_cfg.testAnim, want);   // Image-page checkbox follows
            }
            if (!applied && key == "checker") {
                bool want = (val == "ON" || val == "1" || val == "true");
                g_cfg.checker = want;
                applied = true;
                refreshCheckerUI(want);   // Image-page checkbox follows
            }
            if (!applied && key == "subdivide") {
                int n = atoi(val.c_str());
                if (n < 1 || n > 64) return "ERR SET subdivide (1..64)\n";
                g_cfg.subdivide = n;   // outer lock already held; g_rev++ below retriggers buildMesh
                applied = true;
            }
            if (!applied && key == "move") {
                // session-only editor mode; dispatch uppercased the line so
                // accept both cases via explicit comparisons
                if (val == "SINGLE" || val == "1") g_moveMode.store(0);
                else if (val == "XY" || val == "2") g_moveMode.store(1);
                else if (val == "ALL" || val == "3") g_moveMode.store(2);
                else return "ERR SET move (single|xy|all)\n";
                applied = true;
            }
            if (!applied && key == "roi") {
                // L,T,R,B inset px, comma separated (missing fields = 0)
                int v[4] = { 0, 0, 0, 0 };
                if (sscanf_s(val.c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) < 1)
                    return "ERR SET roi=l,t,r,b\n";
                for (int i = 0; i < 4; i++)
                    g_cfg.roi[i] = (std::max)(0, (std::min)(v[i], 2048));
                applied = true;
                refreshRoiUI();   // Image-page edits follow (marshalled)
            }
            if (!applied && key == "roicircle") {
                g_cfg.roiCircle = (val == "ON" || val == "1" || val == "true");
                applied = true;
                refreshRoiUI();
            }
            if (!applied && key == "roiborder") {
                g_cfg.roiBorder = (std::max)(0, (std::min)(atoi(val.c_str()), 64));
                applied = true;
                refreshRoiUI();
            }
            if (!applied && key == "probe") {
                g_probe.store(val == "ON" || val == "1" || val == "true");
                applied = true;
                requestMagProbeSync();   // engine=1: overlays follow
            }
            if (!applied && key == "blackout") {
                g_blackout.store(val == "ON" || val == "1" || val == "true");
                applied = true;
                requestBlackoutSync();   // menu checkbox + MAG overlays follow
            }
            if (!applied && key == "perflog") {
                g_perfLog.store(val == "ON" || val == "1" || val == "true");
                applied = true;
            }
            if (!applied && key == "inplace") {
                g_inplace.store(val == "ON" || val == "1" || val == "true");
                // mirror into the persisted config so the style survives a restart
                g_cfg.inplace = g_inplace.load();
                requestInplaceRestyle();
                applied = true;
            }
            if (!applied && key == "osc") {
                // NOTE: this whole chain runs INSIDE the g_cfgMutex scope
                // opened above - touching g_cfg directly needs no lock here,
                // and locking again throws (MSVC detects the self-relock).
                if (val != "ON" && val != "1" && val != "OFF" && val != "0")
                    return "ERR SET osc=on|off\n";
                bool want = (val == "ON" || val == "1");
                g_cfg.osc = want ? 1 : 0;
                try {
                    if (want && !oscRunning()) oscStart(g_oscPortLive.load());
                    else if (!want) oscStop();
                } catch (const std::exception& e) {
                    dbgLogLine(std::string("osc: SET threw: ") + e.what());
                    return std::string("ERR SET osc (") + e.what() + ")\n";
                } catch (...) {
                    dbgLogLine("osc: SET threw (non-std)");
                    return "ERR SET osc (unknown)\n";
                }
                applied = true;
            }
            if (!applied && key == "oscport") {
                int p = atoi(val.c_str());
                if (p < 1024 || p > 65535) return "ERR SET oscport (1024..65535)\n";
                g_cfg.oscPort = p;   // under the outer mutex already
                g_oscPortLive.store(p);
                try {
                    if (oscRunning()) { oscStop(); oscStart(p); }
                } catch (const std::exception& e) {
                    dbgLogLine(std::string("osc: rebind threw: ") + e.what());
                    return std::string("ERR SET oscport (") + e.what() + ")\n";
                } catch (...) {
                    dbgLogLine("osc: rebind threw (non-std)");
                    return "ERR SET oscport (unknown)\n";
                }
                applied = true;
            }
            if (!applied) return "ERR SET " + key + "\n";
            g_rev++;
        }
        if (key.size() > 5) {
            // blend parameter writes reshape the MAG fade-bar overlays
            std::string field = key.substr(key.find('.') == std::string::npos ? key.size() : key.find('.') + 1);
            if (field == "fade") requestMagBlendSync();
        }
        refreshStatusBar();
        return "OK " + key + "=" + val + "\n";
    }

    return "ERR UNKNOWN\n";
}

static void handleClient(SOCKET s) {
    dbgLogLine("cmdnet client tid=" + std::to_string((long)GetCurrentThreadId()));
    std::string buf;
    char tmp[4096];
    while (g_netRun.load()) {
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == ';' || c == '\n' || c == '\r') {
                if (!buf.empty()) {
                    std::string rep = dispatch(buf);
                    send(s, rep.c_str(), (int)rep.size(), 0);
                    buf.clear();
                }
            } else if (buf.size() < 16384) {
                // generous line budget: GET warp dumps ~120 points (~1.7KB),
                // and SET warp.load feeds them right back
                buf += c;
            }
        }
    }
    closesocket(s);
}

void cmdnetStart(int port) {
    if (g_netRun.load()) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        closesocket(s); WSACleanup(); return;
    }
    g_listenSock = s;
    g_netRun.store(true);
    // one thread per client: a show controller may open a second connection
    // for monitoring while the first one keeps commanding (the serial loop
    // used to park the second accept until the first client disconnected,
    // which looked like random connection resets to fast clients)
    g_netThread = std::thread([s]() {
        while (g_netRun.load()) {
            sockaddr_in ca{}; int cl = sizeof(ca);
            SOCKET c = accept(s, (sockaddr*)&ca, &cl);
            if (c == INVALID_SOCKET) break;
            std::thread([c]() { handleClient(c); }).detach();
        }
    });
    g_netThread.detach();
}

void cmdnetStop() {
    if (!g_netRun.exchange(false)) return;
    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
    }
    WSACleanup();
}

std::string cmdnetExecute(const std::string& line) { return dispatch(line); }

// ---------- OSC over UDP (loopback only) ----------
// The original's remote-control stack is CMDNET + OSC. Mapping contract:
//   address segment after '/' = command word, OSC args join with spaces into
//   one CMDNET line ("set" + s"engine=1" -> SET ENGINE=1, "get" + s"perf" ->
//   GET PERF), and the textual response is sent back to the sender. Bundles
//   and exotic typetags are ignored. Loopback bind keeps the control surface
//   off the network (widening it is a deliberate later decision).

bool oscRunning() { return g_oscRun.load(); }

// read one null-terminated, 4-byte-aligned OSC blob; p advances past padding
static bool oscBlob(const char*& p, const char* end, const char** out, int* outLen) {
    const char* q = p;
    while (q < end && *q) q++;
    if (q >= end) return false;
    *out = p; *outLen = (int)(q - p);
    q++;                                        // skip NUL
    int pad = (int)(4 - ((q - p) & 3)) & 3;
    if (q + pad > end) return false;
    p = q + pad;
    return true;
}

static void oscHandle(const char* d, int len, sockaddr_in* from) {
    if (len >= 8 && memcmp(d, "#bundle", 8) == 0) return;   // bundles unsupported
    const char* p = d; const char* end = d + len;
    const char* addr; int alen;
    if (!oscBlob(p, end, &addr, &alen)) return;
    std::string a(addr, alen);
    if (a.empty() || a[0] != '/') return;
    std::string line = a.substr(1);
    for (auto& c : line) c = (char)toupper((unsigned char)c);
    if (p < end && *p == ',') {
        const char* tags; int tlen;
        if (!oscBlob(p, end, &tags, &tlen)) return;
        std::string args;
        for (int i = 1; i < tlen; i++) {
            char t = tags[i];
            char tmp[64];
            if (t == 'i' && p + 4 <= end) {
                unsigned v = ((unsigned)(unsigned char)p[0] << 24) |
                             ((unsigned)(unsigned char)p[1] << 16) |
                             ((unsigned)(unsigned char)p[2] << 8) |
                             (unsigned)(unsigned char)p[3];
                p += 4;
                sprintf_s(tmp, sizeof(tmp), "%u", v);
                args += tmp;
            } else if (t == 'f' && p + 4 <= end) {
                // OSC numbers are big-endian: reverse into LE float memory
                unsigned char b4[4] = { (unsigned char)p[3], (unsigned char)p[2],
                                        (unsigned char)p[1], (unsigned char)p[0] };
                p += 4;
                float f; memcpy(&f, b4, 4);
                sprintf_s(tmp, sizeof(tmp), "%g", f);
                args += tmp;
            } else if (t == 's' || t == 'S') {
                const char* sv; int sl;
                if (!oscBlob(p, end, &sv, &sl)) return;
                args.append(sv, sl);
            } else if (t == 'T') {
                args += "on";
            } else if (t == 'F') {
                args += "off";
            } else {
                return;   // unknown typetag (b/h/d/timestamps): ignore packet
            }
            args += ' ';
        }
        while (!args.empty() && args.back() == ' ') args.pop_back();
        if (!args.empty()) { line += ' '; line += args; }
    }
    if (line.empty()) return;
    std::string rep = cmdnetExecute(line);
    sendto(g_oscSock, rep.c_str(), (int)rep.size(), 0, (sockaddr*)from, sizeof(*from));
}

static void oscStartInner(int port) {
    if (g_oscRun.load()) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;   // ref-counted: own pair
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s); WSACleanup(); return;
    }
    g_oscSock = s;
    g_oscPortLive.store(port);
    g_oscRun.store(true);
    dbgLogLine("osc: listening on udp/127.0.0.1:" + std::to_string(port));
    try {
        g_oscThread = std::thread([s]() {
            char buf[2048];
            while (g_oscRun.load()) {
                sockaddr_in from{}; int fl = sizeof(from);
                int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
                if (n <= 0) break;
                oscHandle(buf, n, &from);
            }
        });
    } catch (const std::exception& e) {
        dbgLogLine(std::string("osc: thread threw: ") + e.what());
        g_oscRun.store(false);
        closesocket(s); WSACleanup(); return;
    }
    g_oscThread.detach();
}

void oscStart(int port) {
    try {
        oscStartInner(port);
    } catch (const std::exception& e) {
        dbgLogLine(std::string("osc: start threw: ") + e.what());
    } catch (...) {
        dbgLogLine("osc: start threw (non-std)");
    }
}

void oscStop() {
    if (!g_oscRun.exchange(false)) return;
    if (g_oscSock != INVALID_SOCKET) {
        closesocket(g_oscSock);
        g_oscSock = INVALID_SOCKET;
    }
    WSACleanup();
}
