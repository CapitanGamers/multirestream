#include "control_server.h"
#include "../shared/win_util.h"
#include "../shared/json_mini.h"
#include "../shared/logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>

namespace mr {

ControlServer::ControlServer(int port, const std::string& auth, CmdFn on_cmd, LogFn log)
    : port_(port), auth_(auth), on_cmd_(std::move(on_cmd)), log_(std::move(log)) {}

ControlServer::~ControlServer() { Stop(); }

int ControlServer::ClientCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return (int)clients_.size();
}

bool ControlServer::Start() {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port_);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0) {
        if (log_) log_("bind :" + std::to_string(port_) + " failed");
        closesocket(ls);
        return false;
    }
    listen(ls, 8);
    listen_sock_ = (unsigned long long)ls;
    run_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    if (log_) log_("Control API listening on " + std::to_string(port_));
    return true;
}

void ControlServer::Stop() {
    run_.store(false);
    if (listen_sock_ != ~0ull) {
        closesocket((SOCKET)listen_sock_);
        listen_sock_ = ~0ull;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto s : clients_) closesocket((SOCKET)s);
        clients_.clear();
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void ControlServer::Broadcast(const std::string& json_line) {
    std::string line = json_line;
    if (line.empty() || line.back() != '\n') line += '\n';
    std::lock_guard<std::mutex> g(mu_);
    std::vector<unsigned long long> dead;
    for (auto s : clients_) {
        int r = send((SOCKET)s, line.data(), (int)line.size(), 0);
        if (r <= 0) dead.push_back(s);
    }
    for (auto s : dead) {
        closesocket((SOCKET)s);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
    }
}

void ControlServer::AcceptLoop() {
    while (run_.load()) {
        sockaddr_in caddr{};
        int clen = sizeof(caddr);
        SOCKET c = accept((SOCKET)listen_sock_, (sockaddr*)&caddr, &clen);
        if (c == INVALID_SOCKET) {
            if (!run_.load()) break;
            Logger::I().Warn("ctrl", "accept failed", WSAGetLastError());
            continue;
        }
        
        int flag = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

        char ip[64]{};
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        Logger::I().Info("ctrl", std::string("client connected from ") + ip +
                                 ":" + std::to_string(ntohs(caddr.sin_port)));
        {
            std::lock_guard<std::mutex> g(mu_);
            clients_.push_back((unsigned long long)c);
        }
        std::thread([this, c] { ClientLoop((unsigned long long)c); }).detach();
    }
}

void ControlServer::ClientLoop(unsigned long long sock) {
    SOCKET s = (SOCKET)sock;
    std::string acc;
    char buf[2048];
    bool authed = auth_.empty();
    while (run_.load()) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, n);
        std::size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string type = json::ExtractString(line, "type");
            if (type == "auth") {
                authed = json::ExtractString(line, "token") == auth_ || auth_.empty();
                Logger::I().Info("ctrl", authed ? "auth ok" : "auth rejected (bad token)");
                std::string r = std::string("{\"type\":\"auth_ok\",\"ok\":") + (authed ? "true" : "false") + "}\n";
                send(s, r.data(), (int)r.size(), 0);
                continue;
            }
            if (!authed) {
                Logger::I().Warn("ctrl", "command before auth, type=" + type);
                const char* r = "{\"type\":\"error\",\"error\":\"auth\"}\n";
                send(s, r, (int)strlen(r), 0);
                continue;
            }
            if (on_cmd_) {
                std::string resp = on_cmd_(line);
                if (!resp.empty()) {
                    if (resp.back() != '\n') resp += '\n';
                    send(s, resp.data(), (int)resp.size(), 0);
                }
            }
        }
    }
    Logger::I().Info("ctrl", "client disconnected");
    std::lock_guard<std::mutex> g(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), sock), clients_.end());
    closesocket(s);
}

} // namespace mr
