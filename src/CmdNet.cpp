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

static std::atomic<bool> g_netRun{ false };
static std::thread g_netThread;
static SOCKET g_listenSock = INVALID_SOCKET;

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
