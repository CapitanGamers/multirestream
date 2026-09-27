#include "platform_ops.h"
#include "../server/http_win.h"
#include "../shared/json_mini.h"

namespace mr {

static const wchar_t* kTwitchWebClient = L"kimne78kx3ncx6brgo4mv6wki5h1ko";

static std::wstring H(const std::string& s) { return Utf8ToWide(s); }

static bool TwitchGql(const std::string& auth, const std::string& body,
                      std::string& resp, DWORD& st) {
    std::wstring hdr = L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n";
    hdr += L"Content-Type: application/json\r\n";
    if (!auth.empty())
        hdr += L"Authorization: OAuth " + H(auth) + L"\r\n";
    return HttpRequest(L"POST", L"gql.twitch.tv", 443, true, L"/gql", hdr, body, resp, st);
}

static PlatformAccount ProbeTwitch(const ChromeSession& ses) {
    PlatformAccount a;
    a.platform = Platform::Twitch;
    a.ingest_url = "rtmp://live.twitch.tv/app";
    std::string auth = ses.TwitchAuth();
    if (auth.empty()) {
        a.note = "کوکی auth-token نیست";
        return a;
    }
    a.logged_in = true;
    a.login = ses.TwitchLogin();

    std::string q = R"({"query":"query{currentUser{id login displayName streamKey}}"})";
    std::string body;
    DWORD st = 0;
    if (TwitchGql(auth, q, body, st)) {
        auto id = json::ExtractString(body, "id");
        auto login = json::ExtractString(body, "login");
        auto sk = json::ExtractString(body, "streamKey");
        if (!id.empty()) a.user_id = id;
        if (!login.empty()) a.login = login;
        if (!sk.empty()) a.stream_key = sk;
    }
    if (a.stream_key.empty()) {
        // Helix stream key — works only if the website token has the scope.
        std::wstring path = L"/helix/streams/key?broadcaster_id=" + H(a.user_id);
        std::wstring hdr = L"Client-Id: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n";
        hdr += L"Authorization: Bearer " + H(auth) + L"\r\n";
        std::string hb;
        DWORD hs = 0;
        if (!a.user_id.empty() && HttpGet(L"api.twitch.tv", 443, true, path, hdr, hb, hs)) {
            auto sk = json::ExtractString(hb, "stream_key");
            if (!sk.empty()) a.stream_key = sk;
        }
    }
    // Current title
    if (!a.user_id.empty()) {
        std::wstring path = L"/helix/channels?broadcaster_id=" + H(a.user_id);
        std::wstring hdr = L"Client-Id: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n";
        hdr += L"Authorization: Bearer " + H(auth) + L"\r\n";
        std::string hb; DWORD hs = 0;
        if (HttpGet(L"api.twitch.tv", 443, true, path, hdr, hb, hs))
            a.current_title = json::ExtractString(hb, "title");
    }
    if (a.stream_key.empty())
        a.note = "لاگین توییچ هست؛ کلید استریم از API نیامد — دستی پیست کنید";
    else
        a.note = "کلید توییچ از نشست کروم خوانده شد";
    return a;
}

static PlatformAccount ProbeKick(const ChromeSession& ses) {
    PlatformAccount a;
    a.platform = Platform::Kick;
    a.ingest_url = "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app";
    if (ses.kick.empty()) {
        a.note = "کوکی کیک نیست";
        return a;
    }
    std::wstring hdr = L"Accept: application/json\r\nCookie: " + H(ses.CookieHeader(ses.kick)) + L"\r\n";
    std::string body; DWORD st = 0;
    if (HttpGet(L"kick.com", 443, true, L"/api/v1/user", hdr, body, st) && st >= 200 && st < 300) {
        a.logged_in = true;
        a.login = json::ExtractString(body, "username");
        if (a.login.empty()) a.login = json::ExtractString(body, "slug");
        a.user_id = json::ExtractString(body, "id");
        a.current_title = json::ExtractString(body, "stream_title");
        if (a.current_title.empty())
            a.current_title = json::ExtractString(body, "title");
    }
    // Unofficial dashboard stream-key endpoints tried in order.
    const wchar_t* paths[] = {
        L"/api/v2/channels/stream-key",
        L"/api/v1/user/stream-key",
        L"/stream/streamkey"
    };
    for (auto p : paths) {
        std::string b; DWORD s = 0;
        if (!HttpGet(L"kick.com", 443, true, p, hdr, b, s)) continue;
        auto sk = json::ExtractString(b, "stream_key");
        if (sk.empty()) sk = json::ExtractString(b, "streamKey");
        if (sk.empty()) sk = json::ExtractString(b, "key");
        if (!sk.empty()) { a.stream_key = sk; break; }
    }
    a.note = a.logged_in
        ? (a.stream_key.empty() ? "لاگین کیک هست؛ کلید را از داشبورد پیست کنید" : "کلید کیک از نشست خوانده شد")
        : "نشست کیک معتبر نبود";
    return a;
}

static PlatformAccount ProbeYouTube(const ChromeSession& ses) {
    PlatformAccount a;
    a.platform = Platform::YouTube;
    a.ingest_url = "rtmp://a.rtmp.youtube.com/live2";
    a.logged_in = ses.youtube.count("SID") || ses.youtube.count("SAPISID") ||
                  ses.youtube.count("__Secure-1PSID");
    a.note = a.logged_in
        ? "یوتیوب در کروم لاگین است. کلید استریم یوتیوب از Studio باید پیست شود (API رسمی OAuth می‌خواهد)"
        : "کوکی یوتیوب نیست";
    return a;
}

std::vector<PlatformAccount> DiscoverAccounts(const ChromeSession& ses) {
    return { ProbeTwitch(ses), ProbeYouTube(ses), ProbeKick(ses) };
}

static TitleResult SetTwitchTitle(const ChromeSession& ses, const PlatformAccount& acc,
                                  const std::string& title) {
    TitleResult r;
    r.platform = Platform::Twitch;
    std::string auth = ses.TwitchAuth();
    if (auth.empty() || acc.user_id.empty()) {
        r.detail = "توییچ: نشست یا user id نیست";
        return r;
    }
    std::string payload = std::string("{\"title\":\"") + json::Escape(title) + "\"}";
    std::wstring path = L"/helix/channels?broadcaster_id=" + H(acc.user_id);
    std::wstring hdr = L"Client-Id: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n";
    hdr += L"Authorization: Bearer " + H(auth) + L"\r\n";
    hdr += L"Content-Type: application/json\r\n";
    std::string body; DWORD st = 0;
    HttpRequest(L"PATCH", L"api.twitch.tv", 443, true, path, hdr, payload, body, st);
    if (st == 204 || st == 200) {
        r.ok = true;
        r.detail = "توییچ: عنوان عوض شد";
        return r;
    }
    // GQL fallback (website session)
    std::string gql = std::string("{\"query\":\"mutation{broadcastSettingsUpdate(channelID:\\\"")
                    + json::Escape(acc.user_id) + "\\\",title:\\\"\"}";
    // safer JSON form:
    gql = std::string("{\"query\":\"mutation($id:ID!,$t:String){broadcastSettingsUpdate(channelID:$id,title:$t){title}}\",")
        + "\"variables\":{\"id\":\"" + json::Escape(acc.user_id) + "\",\"t\":\"" + json::Escape(title) + "\"}}";
    std::string gb; DWORD gs = 0;
    if (TwitchGql(auth, gql, gb, gs) && gb.find("errors") == std::string::npos) {
        r.ok = true;
        r.detail = "توییچ: عنوان از GQL عوض شد";
        return r;
    }
    r.detail = "توییچ: API عنوان رد کرد (توکن وب اسکوپ Helix ندارد). عنوان را در داشبورد چک کنید. HTTP "
             + std::to_string(st);
    return r;
}

static TitleResult SetKickTitle(const ChromeSession& ses, const std::string& title) {
    TitleResult r;
    r.platform = Platform::Kick;
    if (ses.kick.empty()) { r.detail = "کیک: نشست نیست"; return r; }
    std::string payload = std::string("{\"stream_title\":\"") + json::Escape(title) + "\"}";
    std::wstring hdr = L"Accept: application/json\r\nContent-Type: application/json\r\n";
    hdr += L"Cookie: " + H(ses.CookieHeader(ses.kick)) + L"\r\n";
    std::string body; DWORD st = 0;
    HttpRequest(L"PATCH", L"kick.com", 443, true, L"/api/v1/channels", hdr, payload, body, st);
    if (st == 204 || st == 200) {
        r.ok = true; r.detail = "کیک: عنوان عوض شد"; return r;
    }
    HttpRequest(L"PATCH", L"api.kick.com", 443, true, L"/public/v1/channels", hdr, payload, body, st);
    if (st == 204 || st == 200) {
        r.ok = true; r.detail = "کیک: عنوان از API رسمی عوض شد"; return r;
    }
    r.detail = "کیک: تغییر عنوان HTTP " + std::to_string(st);
    return r;
}

std::vector<TitleResult> ApplyTitle(const ChromeSession& ses,
                                    const std::string& title,
                                    const std::vector<PlatformAccount>& accounts) {
    std::vector<TitleResult> out;
    for (const auto& a : accounts) {
        if (!a.logged_in) continue;
        if (a.platform == Platform::Twitch) out.push_back(SetTwitchTitle(ses, a, title));
        if (a.platform == Platform::Kick)   out.push_back(SetKickTitle(ses, title));
        if (a.platform == Platform::YouTube) {
            TitleResult r;
            r.platform = Platform::YouTube;
            r.detail = "یوتیوب: عنوان از نشست کروم قابل‌نوشتن نیست — در Studio عوض کنید یا OAuth بدهید";
            out.push_back(r);
        }
    }
    return out;
}

std::string DestinationRtmp(const PlatformAccount& a) {
    if (a.stream_key.empty() || a.ingest_url.empty()) return {};
    if (a.ingest_url.back() == '/') return a.ingest_url + a.stream_key;
    return a.ingest_url + "/" + a.stream_key;
}

} // namespace mr
