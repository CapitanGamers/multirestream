#include "chrome_bridge.h"
#include "ws_tiny.h"
#include "../shared/json_mini.h"
#include "../shared/logger.h"
#include "../server/http_win.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <vector>

namespace mr {

static std::wstring DebugUserDataDir() {
    wchar_t local[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    std::wstring dir = std::wstring(local) + L"\\MultiRestream\\ChromeDebug";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

static std::string TcpHttpGet(const char* host, const char* port, const char* path) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, port, &hints, &res) != 0) return {};
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return {}; }
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(s); freeaddrinfo(res); return {};
    }
    freeaddrinfo(res);
    std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: " + host +
                      ":" + port + "\r\nConnection: close\r\n\r\n";
    send(s, req.data(), (int)req.size(), 0);
    std::string acc;
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, n);
        if (acc.size() > 1 << 20) break;
    }
    closesocket(s);
    auto body = acc.find("\r\n\r\n");
    if (body == std::string::npos) return acc;
    return acc.substr(body + 4);
}

static void IndexCookies(ChromeSession& s) {
    for (const auto& c : s.cookies) {
        std::string d = c.domain;
        if (!d.empty() && d[0] == '.') d.erase(0, 1);
        auto has = [&](const char* needle) {
            return d.find(needle) != std::string::npos;
        };
        if (has("twitch.tv")) s.twitch[c.name] = c.value;
        if (has("kick.com"))  s.kick[c.name] = c.value;
        if (has("youtube.com") || has("google.com") || has("youtube.com"))
            s.youtube[c.name] = c.value;
    }
}

static void ParseCookieBlob(const std::string& reply, ChromeSession& out) {
    std::size_t pos = 0;
    for (;;) {
        auto npos = reply.find("\"name\"", pos);
        if (npos == std::string::npos) break;
        auto chunk = reply.substr(npos, 500);
        ChromeCookie c;
        c.name = json::ExtractString(chunk, "name");
        c.value = json::ExtractString(chunk, "value");
        c.domain = json::ExtractString(chunk, "domain");
        if (!c.name.empty()) out.cookies.push_back(c);
        pos = npos + 6;
        if (out.cookies.size() > 8000) break;
    }
}

static bool CdpCookies(TinyWs& ws, int id, const char* method, std::string& reply) {
    std::string cmd = std::string("{\"id\":") + std::to_string(id) +
                      ",\"method\":\"" + method + "\"}";
    if (!ws.SendText(cmd)) return false;
    reply.clear();
    for (int i = 0; i < 25; ++i) {
        std::string frame;
        if (!ws.RecvText(frame)) break;
        reply += frame;
        std::string key = "\"id\":" + std::to_string(id);
        if (reply.find(key) != std::string::npos) return true;
    }
    return reply.find("cookies") != std::string::npos;
}

ChromeSession ReadChromeSession(int debug_port) {
    ChromeSession out;
    std::string port = std::to_string(debug_port);
    Logger::I().Info("chrome", "probing 127.0.0.1:" + port + "/json/version");
    std::string ver = TcpHttpGet("127.0.0.1", port.c_str(), "/json/version");
    if (ver.empty())
        ver = TcpHttpGet("127.0.0.1", port.c_str(), "/json/list");
    if (ver.empty()) {
        out.error = "Chrome debug port 9333 is closed. Chrome 136+ blocks the default profile. Run start-chrome-debug.bat, then log in to Twitch/Kick inside THAT window.";
        Logger::I().Warn("chrome", "no debugger on port " + port);
        return out;
    }
    Logger::I().Debug("chrome", "version bytes=" + std::to_string(ver.size()));

    std::string wsurl = json::ExtractString(ver, "webSocketDebuggerUrl");
    if (wsurl.empty()) {
        std::string list = TcpHttpGet("127.0.0.1", port.c_str(), "/json/list");
        if (list.empty()) list = TcpHttpGet("127.0.0.1", port.c_str(), "/json");
        wsurl = json::ExtractString(list, "webSocketDebuggerUrl");
    }
    if (wsurl.rfind("ws://", 0) != 0) {
        out.error = "Chrome answered but has no websocket URL. Close every Chrome window and run start-chrome-debug.bat again.";
        Logger::I().Warn("chrome", "no ws url in: " + ver.substr(0, 180));
        return out;
    }

    std::string rest = wsurl.substr(5);
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = hostport, pstr = port;
    auto col = hostport.find(':');
    if (col != std::string::npos) {
        host = hostport.substr(0, col);
        pstr = hostport.substr(col + 1);
    }
    if (host == "localhost") host = "127.0.0.1";

    TinyWs ws;
    if (!ws.Connect(host, pstr, path)) {
        out.error = "CDP websocket handshake failed";
        Logger::I().Error("chrome", "ws connect failed " + host + ":" + pstr + path);
        return out;
    }

    std::string reply;
    bool got = CdpCookies(ws, 1, "Storage.getCookies", reply);
    if (!got || reply.find("cookies") == std::string::npos) {
        Logger::I().Info("chrome", "Storage.getCookies missed, trying Network.getAllCookies");
        CdpCookies(ws, 2, "Network.getAllCookies", reply);
    }
    Logger::I().Debug("chrome", "cdp reply bytes=" + std::to_string(reply.size()));
    ParseCookieBlob(reply, out);
    IndexCookies(out);
    Logger::I().Info("chrome", "cookies=" + std::to_string(out.cookies.size()) +
                               " twitch=" + std::to_string(out.twitch.size()) +
                               " kick=" + std::to_string(out.kick.size()) +
                               " yt=" + std::to_string(out.youtube.size()));

    out.ok = !out.twitch.empty() || !out.kick.empty() || !out.youtube.empty();
    if (!out.ok)
        out.error = "Debug Chrome is open but Twitch/Kick/YouTube cookies are missing. In the Chrome window opened by the script, log in to those sites, then click Read Chrome again.";
    else
        out.error.clear();
    return out;
}

static std::wstring ChromeExe() {
    wchar_t path[MAX_PATH]{};
    DWORD n = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe",
                     nullptr, RRF_RT_REG_SZ, nullptr, path, &n) == ERROR_SUCCESS)
        return path;
    return L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
}

bool LaunchChromeDebug(int debug_port) {
    std::wstring exe = ChromeExe();
    std::wstring user = DebugUserDataDir();
    std::wstring args = L"\"" + exe + L"\" --remote-debugging-port=" +
                        std::to_wstring(debug_port) +
                        L" --remote-allow-origins=*" +
                        L" --user-data-dir=\"" + user + L"\"" +
                        L" --profile-directory=Default";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(args.begin(), args.end());
    buf.push_back(0);
    Logger::I().Info("chrome", "launch debug profile");
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace mr
