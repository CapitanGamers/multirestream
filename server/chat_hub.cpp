#include "chat_hub.h"
#include "http_win.h"
#include "../shared/json_mini.h"
#include "../shared/win_util.h"
#include "../shared/logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>

namespace mr {

ChatHub::ChatHub(ChatFn on_chat, ViewFn on_view, LogFn log)
    : on_chat_(std::move(on_chat)), on_view_(std::move(on_view)), log_(std::move(log)) {}

ChatHub::~ChatHub() { Stop(); }

void ChatHub::SetConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> g(mu_);
    cfg_ = cfg;
}

void ChatHub::Start() {
    Stop();
    run_.store(true);
    worker_ = std::thread([this] { Worker(); });
    irc_thread_ = std::thread([this] {
        while (run_.load()) {
            AppConfig snap;
            { std::lock_guard<std::mutex> g(mu_); snap = cfg_; }
            bool did = false;
            for (const auto& d : snap.destinations) {
                if (d.enabled && d.platform == Platform::Twitch && !d.channel.empty()) {
                    did = true;
                    TwitchIrcOnce(d, run_);
                }
            }
            if (!did) Sleep(2000);
            else Sleep(1500);
        }
    });
}

void ChatHub::Stop() {
    run_.store(false);
    if (worker_.joinable()) worker_.join();
    if (irc_thread_.joinable()) irc_thread_.join();
}

void ChatHub::Worker() {
    while (run_.load()) {
        AppConfig snap;
        { std::lock_guard<std::mutex> g(mu_); snap = cfg_; }
        for (const auto& d : snap.destinations) {
            if (!d.enabled) continue;
            try {
                if (d.platform == Platform::Twitch)  PollTwitch(d);
                if (d.platform == Platform::YouTube) PollYouTube(d);
                if (d.platform == Platform::Kick)    PollKick(d);
            } catch (...) {
                if (log_) log_("poll exception on " + d.id);
            }
        }
        for (int i = 0; i < 20 && run_.load(); ++i) Sleep(250);
    }
}

void ChatHub::PollTwitch(const Destination& d) {
    if (d.channel.empty()) return;
    std::wstring path = L"/helix/streams?user_login=" + Utf8ToWide(d.channel);
    std::wstring hdr = L"Client-Id: " + Utf8ToWide(d.client_id) + L"\r\n";
    if (!d.oauth_token.empty()) {
        std::string tok = d.oauth_token;
        if (tok.rfind("oauth:", 0) == 0) tok = tok.substr(6);
        hdr += L"Authorization: Bearer " + Utf8ToWide(tok) + L"\r\n";
    }
    std::string body; DWORD st = 0;
    if (!HttpGet(L"api.twitch.tv", 443, true, path, hdr, body, st)) {
        if (on_view_) on_view_({Platform::Twitch, d.title, -1, false, false, "http error"});
        return;
    }
    ViewerSnapshot vs;
    vs.platform = Platform::Twitch;
    vs.name = d.title;
    vs.dest_connected = true;
    vs.live = body.find("\"type\":\"live\"") != std::string::npos || body.find("\"type\": \"live\"") != std::string::npos;
    vs.viewers = vs.live ? static_cast<int>(json::ExtractInt(body, "viewer_count", 0)) : 0;
    vs.status = vs.live ? "live" : "offline";
    if (st == 401 || st == 403) vs.status = "auth error";
    if (on_view_) on_view_(vs);
}

void ChatHub::PollYouTube(const Destination& d) {
    if (d.channel.empty() || d.api_key.empty()) return;
    std::wstring path = L"/youtube/v3/videos?part=liveStreamingDetails,statistics&id="
                      + Utf8ToWide(d.channel) + L"&key=" + Utf8ToWide(d.api_key);
    std::string body; DWORD st = 0;
    if (!HttpGet(L"www.googleapis.com", 443, true, path, L"", body, st)) return;
    ViewerSnapshot vs;
    vs.platform = Platform::YouTube;
    vs.name = d.title;
    vs.dest_connected = true;
    int cc = static_cast<int>(json::ExtractInt(body, "concurrentViewers", -1));
    vs.viewers = cc;
    vs.live = cc >= 0;
    vs.status = vs.live ? "live" : "offline";
    if (on_view_) on_view_(vs);

    std::string chat_id = json::ExtractString(body, "activeLiveChatId");
    if (chat_id.empty()) return;
    std::wstring cpath = L"/youtube/v3/liveChat/messages?part=snippet,authorDetails&liveChatId="
                       + Utf8ToWide(chat_id) + L"&maxResults=20&key=" + Utf8ToWide(d.api_key);
    std::string cbody; DWORD cst = 0;
    if (!HttpGet(L"www.googleapis.com", 443, true, cpath, L"", cbody, cst)) return;
    std::size_t p = 0;
    for (int n = 0; n < 8; ++n) {
        auto dn = cbody.find("\"displayName\"", p);
        auto dm = cbody.find("\"displayMessage\"", p);
        if (dn == std::string::npos || dm == std::string::npos) break;
        ChatMessage m;
        m.platform = Platform::YouTube;
        m.user = json::ExtractString(cbody.substr(dn, 180), "displayName");
        m.text = json::ExtractString(cbody.substr(dm, 400), "displayMessage");
        m.ts_ms = NowMs();
        m.logo_text = "YT";
        m.color_hex = "#FF0000"; // رنگ قرمز یوتیوب
        if (!m.text.empty() && on_chat_) on_chat_(m);
        p = (std::max)(dn, dm) + 16;
    }
}

void ChatHub::PollKick(const Destination& d) {
    if (d.channel.empty()) return;
    std::wstring path = L"/public/v1/channels?slug=" + Utf8ToWide(d.channel);
    std::wstring hdr = d.oauth_token.empty() ? L"Accept: application/json\r\n" : L"Authorization: Bearer " + Utf8ToWide(d.oauth_token) + L"\r\nAccept: application/json\r\n";
    std::string body; DWORD st = 0;
    if (!HttpGet(L"api.kick.com", 443, true, path, hdr, body, st)) return;
    ViewerSnapshot vs;
    vs.platform = Platform::Kick;
    vs.name = d.title;
    vs.dest_connected = true;
    vs.viewers = static_cast<int>(json::ExtractInt(body, "viewer_count", -1));
    vs.live = vs.viewers >= 0 && body.find("\"livestream\"") != std::string::npos;
    vs.status = vs.live ? "live" : "offline";
    if (on_view_) on_view_(vs);
}

static bool SockSendAll(SOCKET s, const char* p, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, p + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

void ChatHub::TwitchIrcOnce(const Destination& d, std::atomic<bool>& run) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &res) != 0) return;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return; }
    DWORD timeout = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closesocket(s); freeaddrinfo(res); return; }
    freeaddrinfo(res);

    std::string nick = "justinfan" + std::to_string(10000 + (GetTickCount() % 80000));
    std::string pass = "PASS SCHMOOPIIE\r\n";
    if (!d.oauth_token.empty()) {
        std::string tok = d.oauth_token;
        if (tok.rfind("oauth:", 0) != 0) tok = "oauth:" + tok;
        pass = "PASS " + tok + "\r\n";
        if (!d.channel.empty()) nick = d.channel;
    }
    std::string hello = pass + "NICK " + nick + "\r\nCAP REQ :twitch.tv/tags twitch.tv/commands\r\nJOIN #" + d.channel + "\r\n";
    if (!SockSendAll(s, hello.data(), (int)hello.size())) { closesocket(s); return; }

    std::string acc; char buf[2048];
    while (run.load()) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, n);
        std::size_t nl;
        while ((nl = acc.find("\r\n")) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 2);
            if (line.rfind("PING", 0) == 0) {
                std::string pong = "PONG" + line.substr(4) + "\r\n";
                SockSendAll(s, pong.data(), (int)pong.size());
                continue;
            }
            auto priv = line.find(" PRIVMSG #");
            if (priv == std::string::npos) continue;
            std::string user;
            auto bang = line.find('!');
            auto prefix = line.find(':');
            if (line[0] == '@') {
                auto dn = line.find("display-name=");
                if (dn != std::string::npos) {
                    dn += 13;
                    auto end = line.find(';', dn);
                    if (end == std::string::npos) end = line.find(' ', dn);
                    user = line.substr(dn, end - dn);
                }
            }
            if (user.empty() && bang != std::string::npos && prefix != std::string::npos && bang > prefix) {
                user = line.substr(prefix + 1, bang - prefix - 1);
            }
            auto textpos = line.find(" :", priv);
            if (textpos == std::string::npos) continue;
            
            ChatMessage m;
            m.platform = Platform::Twitch;
            m.user = user.empty() ? "user" : user;
            m.text = line.substr(textpos + 2);
            m.ts_ms = NowMs();
            m.is_mod = line.find("mod=1") != std::string::npos;
            m.is_sub = line.find("subscriber=1") != std::string::npos;
            
            // تولید رنگ و لوگو در سرور!
            m.logo_text = "TW";
            m.color_hex = "#9146FF"; // رنگ بنفش توییچ

            if (on_chat_) on_chat_(m);
        }
    }
    closesocket(s);
}

} // namespace mr
