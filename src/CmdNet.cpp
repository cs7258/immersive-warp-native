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
#include <thread>
#include <cstdlib>
#include <cctype>
#include <cstdio>

static std::atomic<bool> g_netRun{ false };
static std::thread g_netThread;
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
    if (v == "NONE") return 0;
    // compound forms like "L+T" / "LEFT+RIGHT"
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
        if (k == "sampling") return "OK sampling=" + std::to_string(g_cfg.sampling) + "\n";
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
            return "OK engine=" + std::string(g_cfg.engine == 0 ? "desktopduplication" : "unknown") + "\n";
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
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            int n = 0; for (auto& o : g_cfg.outputs) if (o.enabled) n++;
            return "OK outputs=" + std::to_string(n) + "\n";
        }
        if (k == "edges") {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            for (auto& o : g_cfg.outputs) if (o.enabled) return "OK edges=" + edgesName(o.edges) + "\n";
            return "ERR no output\n";
        }
        static const char* sides[4] = { "left", "right", "top", "bottom" };
        for (int s = 0; s < 4; s++) {
            std::string pfx = std::string(sides[s]) + ".";
            if (k.compare(0, pfx.size(), pfx) == 0) {
                std::string field = k.substr(pfx.size());
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    char b[64];
                    if (field == "fade")  { sprintf_s(b, "OK %s.fade=%.3f\n",  sides[s], o.blend[s].fade);  return b; }
                    if (field == "gamma") { sprintf_s(b, "OK %s.gamma=%.3f\n", sides[s], o.blend[s].gamma); return b; }
                    if (field == "gain")  { sprintf_s(b, "OK %s.gain=%.3f\n",  sides[s], o.blend[s].gain);  return b; }
                    if (field == "slope") { sprintf_s(b, "OK %s.slope=%.3f\n", sides[s], o.blend[s].slope); return b; }
                }
                return "ERR GET " + k + "\n";
            }
        }
        return "ERR GET " + arg + "\n";
    }

    if (cmd == "STATUS") {
        int edges = 15, outs = 0;
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            for (auto& o : g_cfg.outputs) if (o.enabled) { outs++; if (outs == 1) edges = o.edges; }
        }
        return "calib=" + std::string(g_calib.load() ? "on" : "off") +
               ";mode=" + std::string(g_calibMode.load() == 1 ? "blend" : "warp") +
               ";edges=" + edgesName(edges) +
               ";snapshot=" + std::string(g_snapshot.load() ? "on" : "off") +
               ";outputs=" + std::to_string(outs) + "\n";
    }

    if (cmd == "CALIB") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !g_calib.load();
        setCalibrationMode(want);
        return std::string("OK calib=") + (g_calib.load() ? "on" : "off") + "\n";
    }

    if (cmd == "MODE") {
        if (arg != "WARP" && arg != "BLEND") return "ERR MODE WARP|BLEND\n";
        if (!g_calib.load()) setCalibrationMode(true);   // mode only exists inside calibration
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

    if (cmd == "SAVE") return saveConfig(L"") ? "OK saved\n" : "ERR SAVE\n";

    if (cmd == "AUTOSTART") {
        bool want = (arg == "ON") ? true : (arg == "OFF") ? false : !queryAutostart();
        setAutostart(want);
        return std::string("OK autostart=") + (queryAutostart() ? "on" : "off") + "\n";
    }

    // SET <side>.<field>=<value>  e.g. "SET left.fade=0.2" / "SET testpattern=off"
    if (cmd == "SET") {
        size_t eq = arg.find('=');
        if (eq == std::string::npos) return "ERR SET side.field=value\n";
        std::string key = arg.substr(0, eq), val = arg.substr(eq + 1);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
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
        if (key == "engine") {
            if (val == "0" || val == "DXGI" || val == "DESKTOPDUPLICATION") {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg.engine = 0;
                return "OK engine=desktopduplication\n";
            }
            return "ERR SET engine (only desktopduplication in this build)\n";
        }
        static const char* sides[4] = { "left", "right", "top", "bottom" };
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            bool applied = false;
            for (int s = 0; s < 4 && !applied; s++) {
                std::string pfx = std::string(sides[s]) + ".";
                if (key.compare(0, pfx.size(), pfx) != 0) continue;
                std::string field = key.substr(pfx.size());
                float v = (float)atof(val.c_str());
                for (auto& o : g_cfg.outputs) {
                    if (!o.enabled) continue;
                    if (field == "fade")  { o.blend[s].fade  = clampf(v, 0.0f, 0.5f); applied = true; }
                    if (field == "gamma") { o.blend[s].gamma = clampf(v, 0.1f, 5.0f); applied = true; }
                    if (field == "gain")  { o.blend[s].gain  = clampf(v, 0.0f, 2.0f); applied = true; }
                    if (field == "slope") { o.blend[s].slope = clampf(v, 0.0f, 1.0f); applied = true; }
                }
            }
            if (!applied && key == "testpattern") {
                g_cfg.testPattern = (val == "ON" || val == "1" || val == "true");
                applied = true;
            }
            if (!applied) return "ERR SET " + key + "\n";
            g_rev++;
        }
        refreshStatusBar();
        return "OK " + key + "=" + val + "\n";
    }

    return "ERR UNKNOWN\n";
}

static void handleClient(SOCKET s) {
    std::string buf;
    char tmp[256];
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
            } else if (buf.size() < 255) {
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
    g_netThread = std::thread([s]() {
        while (g_netRun.load()) {
            sockaddr_in ca{}; int cl = sizeof(ca);
            SOCKET c = accept(s, (sockaddr*)&ca, &cl);
            if (c == INVALID_SOCKET) break;
            handleClient(c);
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
