// MQTT client: the original's v7.1.2 remote-control interface.
// Subscribes <prefix>/command (default prefix "display") and applies JSON
// command payloads through the CMDNET dispatch core; publishes
// <prefix>/status (retain) on connect and after every applied command.
//
// Minimal MQTT 3.1.1 over TCP, QoS 0, clean session, no TLS and no
// username/password - the official doc (doc.elise-ng.net/display/MQTT)
// documents the JSON payload contract but no auth, and the trust model
// matches CMDNET (localhost / trusted LAN broker). Zero dependencies:
// raw winsock2, hand-rolled packet codec and a tiny JSON value finder.
//
// Command map (original MQTT command -> our implementation):
//   setconfigset      {"value":1..8}     -> WM_MQCFGSET: preset by name order
//   setdesktopwarping {"enabled":bool}   -> WM_MQWARP: absolute output on/off
//   blackout          {"enabled":bool}   -> SET blackout=on|off
//   loadprocalib      {"file":"..."}     -> LOADFILE <path> (display key ignored:
//                                           our presets are whole-rig configs)
//   quit              {}                 -> WM_CLOSE
//   setheadpose                          -> logged, not supported (no 6-DOF
//                                           simulator integration)
// winsock2.h MUST come before windows.h (pulled in via Common.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include "Common.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// ---- endpoint configuration (socket/UI threads write, mqtt thread reads) ----
static std::mutex g_mqCfgMutex;
static std::string g_mqHost = "127.0.0.1";
static int g_mqPort = 1883;
static std::string g_mqPrefix = "display";

static std::atomic<bool> g_mqRun{ false };        // thread lifecycle
static std::atomic<bool> g_mqConnected{ false };  // broker session up
static std::atomic<bool> g_mqKick{ false };       // reconfigure: drop + reconnect
static std::thread g_mqThread;

// WM_MQWARP / WM_MQCFGSET are constexpr in Common.h (WM_APP+24/+25)

// ---------------- tiny JSON value finder ----------------
// Locates "key" and returns the position just past its ':' - sufficient for
// the documented flat payloads (no nested objects carry our keys).
static bool jsonFindValue(const std::string& js, const char* key, size_t& vpos) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    while (p != std::string::npos) {
        size_t c = js.find(':', p + pat.size());
        if (c != std::string::npos) { vpos = c + 1; return true; }
        p = js.find(pat, p + pat.size());
    }
    return false;
}
static void jsonSkipWs(const std::string& js, size_t& i) {
    while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\r' || js[i] == '\n')) i++;
}
static bool jsonFindString(const std::string& js, const char* key, std::string& out) {
    size_t i;
    if (!jsonFindValue(js, key, i)) return false;
    jsonSkipWs(js, i);
    if (i >= js.size() || js[i] != '"') return false;
    i++;
    out.clear();
    while (i < js.size()) {
        char c = js[i++];
        if (c == '"') return true;
        if (c == '\\' && i < js.size()) {
            char e = js[i++];
            if (e == '\\' || e == '"' || e == '/') out += e;
            else if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else out += e;   // unknown escape: keep verbatim
        } else out += c;
    }
    return false;   // unterminated string
}
static bool jsonFindNumber(const std::string& js, const char* key, double& out) {
    size_t i;
    if (!jsonFindValue(js, key, i)) return false;
    jsonSkipWs(js, i);
    char* end = nullptr;
    double v = strtod(js.c_str() + i, &end);
    if (end == js.c_str() + i) return false;
    out = v;
    return true;
}
static bool jsonFindBool(const std::string& js, const char* key, bool& out) {
    size_t i;
    if (!jsonFindValue(js, key, i)) return false;
    jsonSkipWs(js, i);
    if (js.compare(i, 4, "true") == 0) { out = true; return true; }
    if (js.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

// ---------------- status payload ----------------
static std::string statusJson() {
    bool warping = false;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        warping = g_cfg.enabled;
    }
    char buf[160];
    _snprintf_s(buf, _TRUNCATE,
                "{\"state\":\"running\",\"warping\":%d,\"blackout\":%d,\"outputs\":%d}",
                warping ? 1 : 0, g_blackout.load() ? 1 : 0, g_outCount.load());
    return buf;
}

// ---------------- command handling (mqtt thread) ----------------
// Runs on the mqtt thread: cmdnetExecute is the same entry the OSC thread
// uses (stateless dispatch, g_cfg writes happen under the outer mutex there),
// and UI-touching paths are marshalled with PostMessage like every other
// off-thread caller.
static void handleCommand(const std::string& payload) {
    std::string cmd;
    if (!jsonFindString(payload, "command", cmd) || cmd.empty()) {
        dbgLogLine("mqtt: payload without a command string (dropped)");
        return;
    }
    dbgLogLine("mqtt: cmd=" + cmd);
    if (cmd == "setconfigset") {
        double v;
        if (!jsonFindNumber(payload, "value", v) || v < 1 || v > 8 || v != (double)(int)v) {
            dbgLogLine("mqtt: setconfigset needs value 1..8");
            return;
        }
        PostMessage(consoleWindowHandle(), WM_MQCFGSET, (WPARAM)(int)v, 0);
    } else if (cmd == "setdesktopwarping") {
        bool on = false;
        if (!jsonFindBool(payload, "enabled", on)) {
            dbgLogLine("mqtt: setdesktopwarping needs enabled true|false");
            return;
        }
        HWND ctl = consoleWindowHandle();
        BOOL posted = PostMessage(ctl, WM_MQWARP, on ? 1 : 0, 0);
        dbgLogLine(std::string("mqtt: setdesktopwarping ") + (on ? "on" : "off") +
                   " ctl=" + std::to_string((unsigned long long)(size_t)ctl) +
                   " posted=" + std::to_string((int)posted));
    } else if (cmd == "blackout") {
        bool on = false;
        if (!jsonFindBool(payload, "enabled", on)) {
            dbgLogLine("mqtt: blackout needs enabled true|false");
            return;
        }
        cmdnetExecute(std::string("SET blackout=") + (on ? "ON" : "OFF"));
    } else if (cmd == "loadprocalib") {
        std::string file;
        if (!jsonFindString(payload, "file", file) || file.empty()) {
            dbgLogLine("mqtt: loadprocalib needs file");
            return;
        }
        // dispatch uppercases the line - NTFS paths are case-insensitive
        cmdnetExecute("LOADFILE " + file);
    } else if (cmd == "quit") {
        // WM_CLOSE only hides the console to the tray (the original's
        // behaviour) - the real exit is DestroyWindow, marshalled here
        PostMessage(consoleWindowHandle(), WM_MQQUIT, 0, 0);
    } else {
        // setheadpose and any future commands land here until supported
        dbgLogLine("mqtt: unsupported command " + cmd);
    }
}

// ---------------- MQTT 3.1.1 wire helpers ----------------
// Encodes the variable-length remaining-length field, returns bytes written.
static int encLen(int len, unsigned char out[4]) {
    int n = 0;
    do {
        unsigned char d = len % 128;
        len /= 128;
        if (len > 0) d |= 0x80;
        out[n++] = d;
    } while (len > 0 && n < 4);
    return n;
}
static bool sendAll(SOCKET s, const char* buf, int n) {
    int off = 0;
    while (off < n) {
        int w = send(s, buf + off, n - off, 0);
        if (w <= 0) return false;
        off += w;
    }
    return true;
}
static bool sendPacket(SOCKET s, unsigned char typeFlags, const std::string& body) {
    unsigned char hdr[5];
    hdr[0] = typeFlags;
    int ln = encLen((int)body.size(), hdr + 1);
    if (!sendAll(s, (const char*)hdr, 1 + ln)) return false;
    if (!body.empty()) return sendAll(s, body.data(), (int)body.size());
    return true;
}
// Extracts one complete MQTT packet from buf. Returns bytes consumed (0 =
// incomplete), fills type and body. Variable-length field is 1..4 bytes.
static int extractPacket(const std::string& buf, unsigned char& type, std::string& body) {
    if (buf.size() < 2) return 0;
    int len = 0, mul = 1;
    size_t i = 1;
    for (; i < buf.size() && i <= 4; i++) {
        unsigned char b = (unsigned char)buf[i];
        len += (b & 0x7F) * mul;
        mul *= 128;
        if (!(b & 0x80)) { i++; goto haveLen; }
    }
    return 0;
haveLen:
    if (buf.size() < i + (size_t)len) return 0;
    type = (unsigned char)(buf[0] >> 4);
    body = buf.substr(i, (size_t)len);
    return (int)(i + (size_t)len);
}
// CONNECT + SUBSCRIBE handshake; false on any protocol error.
static bool handshake(SOCKET s, const std::string& cmdTopic) {
    std::string body;
    char pidstr[32];
    sprintf_s(pidstr, "IWN-%u", (unsigned)GetCurrentProcessId());
    size_t idlen = strlen(pidstr);
    // payload: clientId
    body.push_back((char)0); body.push_back((char)idlen);
    body.append(pidstr, idlen);
    // variable header: "MQTT" level 4, clean session, keepalive 60s
    std::string pkt;
    pkt.push_back((char)0); pkt.push_back((char)4); pkt.append("MQTT", 4);
    pkt.push_back((char)4);            // level 4 = 3.1.1
    pkt.push_back((char)0x02);         // clean session
    pkt.push_back((char)0); pkt.push_back((char)60);
    std::string full = pkt + body;
    if (!sendPacket(s, 0x10, full)) return false;

    // CONNACK: 0x20 0x02 <session-present> <return-code>, rc must be 0
    unsigned char ack[4] = { 0, 0, 0, 0 };
    int got = 0;
    while (got < 4) {
        int r = recv(s, (char*)ack + got, 4 - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    if (ack[0] != 0x20 || ack[3] != 0) return false;

    // SUBSCRIBE: packet id 1, topic, QoS 0
    std::string sub;
    sub.push_back((char)0); sub.push_back((char)1);            // packet id
    sub.push_back((char)(cmdTopic.size() >> 8));
    sub.push_back((char)(cmdTopic.size() & 0xFF));
    sub.append(cmdTopic);
    sub.push_back((char)0);                                    // QoS 0
    if (!sendPacket(s, 0x82, sub)) return false;

    // SUBACK: 0x90, body = [id:2][qos-byte]; qos 0x80 means refused
    unsigned char sak[5] = { 0, 0, 0, 0, 0 };
    got = 0;
    while (got < 5) {
        int r = recv(s, (char*)sak + got, 5 - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return sak[0] == 0x90 && sak[4] != 0x80;
}
// PUBLISH (QoS 0, retain) of a UTF-8 payload.
static bool publishTopic(SOCKET s, const std::string& topic, const std::string& payload, bool retain) {
    std::string body;
    body.push_back((char)(topic.size() >> 8));
    body.push_back((char)(topic.size() & 0xFF));
    body.append(topic);
    body.append(payload);
    return sendPacket(s, retain ? 0x31 : 0x30, body);
}

// ---------------- thread main ----------------
static void mqttThreadFn() {
    dbgLogLine("mqtt: thread started");
    while (g_mqRun.load()) {
        std::string host, prefix;
        int port;
        {
            std::lock_guard<std::mutex> lk(g_mqCfgMutex);
            host = g_mqHost; port = g_mqPort; prefix = g_mqPrefix;
        }
        std::string cmdTopic = prefix + "/command";
        std::string statusTopic = prefix + "/status";

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(3000); continue; }
        // session timeouts keep a half-open broker connection from wedging the thread
        DWORD tmo = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)port);
        inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
        // hostname support: resolve non-numeric hosts before giving up
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                closesocket(s);
                dbgLogLine("mqtt: resolve failed " + host);
                Sleep(3000);
                continue;
            }
            sa.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
            closesocket(s);
            dbgLogLine("mqtt: connect failed " + host);
            Sleep(3000);
            continue;
        }
        if (!handshake(s, cmdTopic)) {
            closesocket(s);
            dbgLogLine("mqtt: handshake failed");
            Sleep(3000);
            continue;
        }
        g_mqConnected.store(true);
        dbgLogLine("mqtt: connected " + host);
        publishTopic(s, statusTopic, statusJson(), true);   // retained state on connect

        std::string rx;
        unsigned char type;
        std::string body;
        DWORD lastPing = GetTickCount();
        while (g_mqRun.load() && !g_mqKick.load()) {
            // select with a short timeout: stop/reconfigure latency stays low
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{ 0, 250000 };
            int rdy = select(0, &rd, nullptr, nullptr, &tv);
            if (rdy <= 0) {
                if (rdy < 0) break;
                // keepalive: PINGREQ every 30s (keepalive 60s), resp is a no-op
                if (GetTickCount() - lastPing > 30000) {
                    if (!sendPacket(s, 0xC0, std::string())) break;
                    lastPing = GetTickCount();
                }
                continue;
            }
            char chunk[4096];
            int r = recv(s, chunk, sizeof(chunk), 0);
            if (r <= 0) break;
            rx.append(chunk, (size_t)r);
            int used;
            while ((used = extractPacket(rx, type, body)) > 0) {
                rx.erase(0, (size_t)used);
                if (type == 3) {   // PUBLISH (QoS 0: no packet id in the body)
                    if (body.size() >= 2) {
                        int tlen = ((unsigned char)body[0] << 8) | (unsigned char)body[1];
                        if ((size_t)tlen + 2 <= body.size()) {
                            std::string payload = body.substr(2 + tlen);
                            handleCommand(payload);
                            publishTopic(s, statusTopic, statusJson(), true);
                        }
                    }
                }
                // PINGRESP / SUBACK re-delivery / anything else: ignore
            }
        }
        g_mqConnected.store(false);
        g_mqKick.store(false);   // consumed (or stop - the outer loop exits anyway)
        closesocket(s);
        if (g_mqRun.load()) Sleep(1000);   // brief backoff before reconnect
    }
    dbgLogLine("mqtt: thread exited");
}

// ---------------- public API (declared in Common.h) ----------------
void mqttStart() {
    if (g_mqRun.load()) return;
    // seed the endpoint from the current config. NOTE: the caller either
    // holds g_cfgMutex (SET mqtt=on inside the dispatch locked region) or
    // is single-threaded startup - locking again here would throw
    // (MSVC self-relock, #147).
    {
        std::lock_guard<std::mutex> lk(g_mqCfgMutex);
        g_mqHost = g_cfg.mqttHost;
        g_mqPort = g_cfg.mqttPort;
        g_mqPrefix = g_cfg.mqttPrefix;
    }
    g_mqKick.store(false);
    g_mqRun.store(true);
    try {
        g_mqThread = std::thread(mqttThreadFn);
    } catch (...) {
        g_mqRun.store(false);
        dbgLogLine("mqtt: thread spawn failed");
    }
}
void mqttStop() {
    if (!g_mqRun.load()) return;
    g_mqRun.store(false);
    // a command handler runs ON the mqtt thread (cmdnetExecute -> SET
    // mqtt=off) - joining self would deadlock, so only foreign threads join
    if (std::this_thread::get_id() != g_mqThread.get_id()) {
        if (g_mqThread.joinable()) g_mqThread.join();
    }
}
bool mqttRunning() { return g_mqRun.load(); }
bool mqttConnected() { return g_mqConnected.load(); }
void mqttReconfigure(const std::string& host, int port, const std::string& prefix) {
    {
        std::lock_guard<std::mutex> lk(g_mqCfgMutex);
        g_mqHost = host;
        g_mqPort = port;
        g_mqPrefix = prefix;
    }
    if (g_mqConnected.load()) g_mqKick.store(true);   // drop + reconnect with the new endpoint
}
