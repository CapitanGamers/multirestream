#include "../shared/win_util.h"
#include "../shared/protocol.h"
#include "../shared/json_mini.h"
#include "chrome_bridge.h"
#include "platform_ops.h"
#include "obs_bridge.h"
#include "../shared/logger.h"

#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>

using namespace mr;

#define IDC_HOST        301
#define IDC_PORT        302
#define IDC_TOKEN       303
#define IDC_CONNECT     304
#define IDC_DISCONNECT  305
#define IDC_FONTPLUS    306
#define IDC_FONTMINUS   307
#define IDC_CHAT        308
#define IDC_STATUS      309
#define IDC_CONTRAST    310
#define IDC_VIEWPANEL   311
#define IDC_CHROME      320
#define IDC_TITLE       321
#define IDC_APPLY_TITLE 322
#define IDC_INGEST_BOX  323
#define IDC_COPY_INGEST 324
#define IDC_OBS_PASS    325
#define IDC_OBS_PUSH    326
#define IDC_ACCOUNTS    327
#define IDC_PUSH_KEYS   328
#define IDC_LAUNCH_CH   329

#define WM_APP_LINE     (WM_APP + 10)
#define WM_APP_CONN     (WM_APP + 11)

static HWND g_wnd = nullptr;
static HWND g_chat = nullptr;
static HWND g_status = nullptr;
static HWND g_host = nullptr;
static HWND g_port = nullptr;
static HWND g_token = nullptr;
static HWND g_view = nullptr;
static HWND g_title = nullptr;
static HWND g_ingest = nullptr;
static HWND g_obs_pass = nullptr;
static HWND g_accounts = nullptr;
static HFONT g_font = nullptr;
static HFONT g_font_big = nullptr;
static HFONT g_font_chat = nullptr;
static int g_pt = 14;

static SOCKET g_sock = INVALID_SOCKET;
static std::atomic<bool> g_run{false};
static std::thread g_rx;

struct ViewCard {
    Platform platform = Platform::Unknown;
    std::string name;
    int viewers = -1;
    bool live = false;
    std::string status;
};

static std::mutex g_mu;
static std::vector<ViewCard> g_cards;
static std::deque<ChatMessage> g_msgs;
static const size_t kMaxChat = 400;

static ChromeSession g_chrome;
static std::vector<PlatformAccount> g_accounts_data;
static std::string g_ingest_server;
static std::string g_ingest_key;
static std::string g_ingest_url;

static COLORREF C_BG         = RGB(24, 25, 28);
static COLORREF C_PANEL      = RGB(43, 45, 49);
static COLORREF C_TEXT       = RGB(242, 243, 245);
static COLORREF C_TEXT_DIM   = RGB(148, 155, 164);
static COLORREF C_BTN_PRM    = RGB(88, 101, 242);
static COLORREF C_BTN_PRM_DN = RGB(71, 82, 196);
static COLORREF C_BTN_SEC    = RGB(78, 80, 88);
static COLORREF C_BTN_SEC_DN = RGB(104, 109, 115);

static void RecreateFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_chat) DeleteObject(g_font_chat);
    g_font = MakeFont(g_pt, false);
    g_font_big = MakeFont(g_pt + 8, true);
    g_font_chat = MakeFont(g_pt + 2, false);
    if (g_chat) {
        SetWindowFont(g_chat, g_font_chat);
        SendMessageW(g_chat, LB_SETITEMHEIGHT, 0, g_pt + 22);
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, TRUE);
}

static void SetStatus(const wchar_t* s) {
    Logger::I().Info("ui", WideToUtf8(s ? s : L""));
    if (g_status) SetWindowTextW(g_status, s);
}

static void SendLine(const std::string& json) {
    if (g_sock == INVALID_SOCKET) return;
    std::string line = json;
    if (line.empty() || line.back() != '\n') line += '\n';
    send(g_sock, line.data(), (int)line.size(), 0);
}

static void RefreshAccountList() {
    if (!g_accounts) return;
    SendMessageW(g_accounts, LB_RESETCONTENT, 0, 0);
    for (const auto& a : g_accounts_data) {
        std::wstring row = Utf8ToWide(PlatformTitleFa(a.platform));
        row += a.logged_in ? L"  ✓  " : L"  ✗  ";
        row += Utf8ToWide(a.login);
        row += L"  |  ";
        row += a.stream_key.empty() ? L"بدون کلید" : L"کلید آماده";
        row += L"  |  ";
        row += Utf8ToWide(a.note);
        SendMessageW(g_accounts, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
}

static void ApplyLine(const std::string& line) {
    std::string type = json::ExtractString(line, "type");
    if (type == "chat") {
        ChatMessage m;
        m.platform = PlatformFromId(json::ExtractString(line, "platform"));
        m.user = json::ExtractString(line, "user");
        m.text = json::ExtractString(line, "text");
        m.ts_ms = json::ExtractInt(line, "ts", NowMs());
        m.is_mod = json::ExtractBool(line, "mod", false);
        m.is_sub = json::ExtractBool(line, "sub", false);
        {
            std::lock_guard<std::mutex> g(g_mu);
            g_msgs.push_back(m);
        }
        std::wstring acc = Utf8ToWide(std::string(PlatformTitleFa(m.platform))) + L" " + Utf8ToWide(m.user) + L": " + Utf8ToWide(m.text);
        SendMessageW(g_chat, LB_ADDSTRING, 0, (LPARAM)acc.c_str());
        int count = (int)SendMessageW(g_chat, LB_GETCOUNT, 0, 0);
        {
            std::lock_guard<std::mutex> g(g_mu);
            while ((int)g_msgs.size() > (int)kMaxChat) g_msgs.pop_front();
        }
        while (count > (int)kMaxChat) {
            SendMessageW(g_chat, LB_DELETESTRING, 0, 0);
            --count;
        }
        SendMessageW(g_chat, LB_SETTOPINDEX, count - 1, 0);
        return;
    }
    if (type == "viewers") {
        ViewCard c;
        c.platform = PlatformFromId(json::ExtractString(line, "platform"));
        c.name = json::ExtractString(line, "name");
        c.viewers = (int)json::ExtractInt(line, "viewers", -1);
        c.live = json::ExtractBool(line, "live", false);
        c.status = json::ExtractString(line, "status");
        {
            std::lock_guard<std::mutex> g(g_mu);
            bool found = false;
            for (auto& x : g_cards) {
                if (x.platform == c.platform && x.name == c.name) { x = c; found = true; break; }
            }
            if (!found) g_cards.push_back(c);
        }
        if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    if (type == "state") {
        SetStatus(L"متصل — وضعیت همگام شد");
    }
    if (type == "auth_ok") {
        bool ok = json::ExtractBool(line, "ok", false);
        SetStatus(ok ? L"احراز هویت موفق" : L"توکن نادرست");
        if (ok && g_sock != INVALID_SOCKET) {
            SendLine("{\"type\":\"get_state\"}");
            SendLine("{\"type\":\"get_ingest\"}");
        }
    }
    if (type == "ingest") {
        g_ingest_server = json::ExtractString(line, "server");
        g_ingest_key = json::ExtractString(line, "key");
        g_ingest_url = json::ExtractString(line, "url");
        if (g_ingest) SetWindowTextW(g_ingest, Utf8ToWide(g_ingest_url).c_str());
        SetStatus(L"کلید اینجست از سرور گرفته شد — بگذار در OBS");
    }
}

static void RxLoop() {
    std::string acc;
    char buf[4096];
    while (g_run.load() && g_sock != INVALID_SOCKET) {
        int n = recv(g_sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, n);
        std::size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto* heap = new std::string(std::move(line));
            PostMessageW(g_wnd, WM_APP_LINE, 0, (LPARAM)heap);
        }
    }
    PostMessageW(g_wnd, WM_APP_CONN, 0, 0);
}

static void Disconnect() {
    g_run.store(false);
    if (g_sock != INVALID_SOCKET) {
        shutdown(g_sock, SD_BOTH);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    if (g_rx.joinable() && g_rx.get_id() != std::this_thread::get_id())
        g_rx.join();
    SetStatus(L"قطع شد");
}

static void Connect() {
    Disconnect();
    wchar_t hostw[256], portw[32], tokw[256];
    GetWindowTextW(g_host, hostw, 256);
    GetWindowTextW(g_port, portw, 32);
    GetWindowTextW(g_token, tokw, 256);
    std::string host = WideToUtf8(hostw);
    std::string port = WideToUtf8(portw);
    std::string tok  = WideToUtf8(tokw);
    if (host.empty()) { SetStatus(L"آیپی سرور خالی است"); return; }
    if (port.empty()) port = "7788";

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        SetStatus(L"DNS / آدرس نامعتبر");
        return;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        SetStatus(L"اتصال برقرار نشد — فایروال سرور را چک کنید");
        closesocket(s); freeaddrinfo(res); return;
    }
    freeaddrinfo(res);
    g_sock = s;
    g_run.store(true);
    g_rx = std::thread(RxLoop);
    std::string auth = std::string("{\"type\":\"auth\",\"token\":\"") + json::Escape(tok) + "\"}\n";
    send(s, auth.data(), (int)auth.size(), 0);
    SetStatus(L"در حال اتصال…");
}

static void DrawLogoBadge(HDC dc, RECT r, Platform p) {
    COLORREF col = PlatformColor((int)p);
    HBRUSH br = CreateSolidBrush(col);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ obr = SelectObject(dc, br);
    HGDIOBJ open = SelectObject(dc, pen);
    int side = (std::min)(r.right - r.left, r.bottom - r.top);
    int x = r.left + ((r.right - r.left) - side) / 2;
    int y = r.top  + ((r.bottom - r.top) - side) / 2;
    Ellipse(dc, x, y, x + side, y + side);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p == Platform::Kick ? RGB(10, 20, 10) : RGB(255, 255, 255));
    HFONT badge = MakeFont((std::max)(8, g_pt - 4), true);
    HGDIOBJ of = SelectObject(dc, badge);
    DrawTextW(dc, PlatformGlyph((int)p), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(badge);
    SelectObject(dc, obr);
    SelectObject(dc, open);
    DeleteObject(br);
    DeleteObject(pen);
}

static void DrawViewCards(HDC dc, RECT area) {
    std::vector<ViewCard> cards;
    { std::lock_guard<std::mutex> g(g_mu); cards = g_cards; }
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &area, bg);
    DeleteObject(bg);

    if (cards.empty()) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT_DIM);
        SelectObject(dc, g_font);
        DrawTextW(dc, L"آماده دریافت اطلاعات لایو از سرور...", -1, &area, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    const int n = (int)cards.size();
    const int gap = 16;
    int card_w = (area.right - area.left - gap * (n + 1)) / (std::max)(1, n);
    if (card_w < 160) card_w = 160;
    int x = area.left + gap;
    for (const auto& c : cards) {
        RECT cr{ x, area.top + gap, x + card_w, area.bottom - gap };
        HBRUSH pb = CreateSolidBrush(C_PANEL);
        HPEN pn = CreatePen(PS_SOLID, 4, PlatformColor((int)c.platform));
        HGDIOBJ ob = SelectObject(dc, pb);
        HGDIOBJ op = SelectObject(dc, pn);
        RoundRect(dc, cr.left, cr.top, cr.right, cr.bottom, 12, 12);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(pb); DeleteObject(pn);

        RECT badge{ cr.left + 16, cr.top + 16, cr.left + 52, cr.top + 52 };
        DrawLogoBadge(dc, badge, c.platform);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        RECT title{ badge.right + 12, cr.top + 18, cr.right - 12, cr.top + 46 };
        SelectObject(dc, g_font);
        std::wstring t = Utf8ToWide(c.name.empty() ? PlatformTitleFa(c.platform) : c.name);
        DrawTextW(dc, t.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT num{ cr.left + 16, cr.top + 64, cr.right - 16, cr.bottom - 36 };
        SelectObject(dc, g_font_big);
        SetTextColor(dc, c.live ? RGB(88, 255, 120) : C_TEXT_DIM);
        std::wstring vs = FormatViewers(c.viewers);
        DrawTextW(dc, vs.c_str(), -1, &num, DT_LEFT | DT_TOP | DT_SINGLELINE);

        RECT st{ cr.left + 16, cr.bottom - 34, cr.right - 16, cr.bottom - 12 };
        SelectObject(dc, g_font);
        SetTextColor(dc, c.live ? RGB(88, 255, 120) : RGB(255, 100, 100));
        std::wstring sl = c.live ? L"زنده" : Utf8ToWide(c.status.empty() ? "آفلاین" : c.status);
        DrawTextW(dc, sl.c_str(), -1, &st, DT_LEFT | DT_SINGLELINE);
        x += card_w + gap;
    }
}

static void DrawChatItem(DRAWITEMSTRUCT* dis) {
    if (dis->itemID == (UINT)-1) return;
    ChatMessage m;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (dis->itemID >= g_msgs.size()) return;
        m = g_msgs[dis->itemID];
    }
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool sel = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH br = CreateSolidBrush(sel ? RGB(53, 55, 63) : C_PANEL);
    FillRect(dc, &r, br);
    DeleteObject(br);

    RECT badge{ r.left + 12, r.top + 8, r.left + 12 + (g_pt + 12), r.bottom - 8 };
    DrawLogoBadge(dc, badge, m.platform);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_chat);

    RECT pr{ badge.right + 12, r.top + 2, r.right - 12, r.bottom - 2 };
    std::wstring user = Utf8ToWide(m.user);
    std::wstring text = Utf8ToWide(m.text);
    std::wstring line = user;
    if (m.is_mod) line += L" [M]";
    if (m.is_sub) line += L" [S]";
    line += L"   ";
    line += text;

    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, line.c_str(), -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void DrawModernButton(DRAWITEMSTRUCT* di) {
    bool is_primary = (di->CtlID == IDC_CONNECT || di->CtlID == IDC_APPLY_TITLE || di->CtlID == IDC_OBS_PUSH);
    bool pushed = (di->itemState & ODS_SELECTED);
    COLORREF bg = pushed ? (is_primary ? C_BTN_PRM_DN : C_BTN_SEC_DN) 
                         : (is_primary ? C_BTN_PRM : C_BTN_SEC);
                         
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(di->hDC, &di->rcItem, br);
    DeleteObject(br);
    
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, C_TEXT);
    SelectObject(di->hDC, g_font);
    
    wchar_t txt[128];
    GetWindowTextW(di->hwndItem, txt, 128);
    DrawTextW(di->hDC, txt, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DoChrome() {
    SetStatus(L"در حال خواندن کروم…");
    g_chrome = ReadChromeSession(9333);
    if (!g_chrome.ok) { SetStatus(Utf8ToWide(g_chrome.error).c_str()); return; }
    g_accounts_data = DiscoverAccounts(g_chrome);
    RefreshAccountList();
    SetStatus(L"نشست خوانده شد — کوکیها امن ماندند");
}

static void DoPushKeys() {
    if (g_sock == INVALID_SOCKET) { SetStatus(L"به سرور وصل نیستید"); return; }
    int n = 0;
    for (const auto& a : g_accounts_data) {
        auto url = DestinationRtmp(a);
        if (url.empty()) continue;
        std::ostringstream o;
        o << "{\"type\":\"set_destination\",\"id\":\"" << PlatformId(a.platform)
          << "\",\"platform\":\"" << PlatformId(a.platform) << "\",\"title\":\"" << json::Escape(a.login)
          << "\",\"channel\":\"" << json::Escape(a.login) << "\",\"rtmp_url\":\"" << json::Escape(url)
          << "\",\"enabled\":true";
        if (a.platform == Platform::Twitch && !g_chrome.TwitchAuth().empty())
            o << ",\"oauth_token\":\"" << json::Escape(g_chrome.TwitchAuth()) << "\"";
        o << "}";
        SendLine(o.str());
        ++n;
    }
    wchar_t b[128]; swprintf(b, 128, L"%d مقصد فرستاده شد", n); SetStatus(b);
}

static void DoApplyTitle() {
    int n = GetWindowTextLengthW(g_title);
    std::wstring wt(n, 0); GetWindowTextW(g_title, wt.data(), n + 1);
    std::string title = WideToUtf8(wt);
    if (title.empty()) return;
    SendLine(std::string("{\"type\":\"set_title\",\"title\":\"") + json::Escape(title) + "\"}");
    auto results = ApplyTitle(g_chrome, title, g_accounts_data);
    std::wstring msg;
    for (const auto& r : results) {
        if (!msg.empty()) msg += L"  |  ";
        msg += Utf8ToWide(r.detail);
    }
    SetStatus(msg.empty() ? L"عنوان فرستاده شد" : msg.c_str());
}

static void DoObs() {
    if (g_ingest_server.empty()) return;
    int n = GetWindowTextLengthW(g_obs_pass);
    std::wstring wp(n, 0); GetWindowTextW(g_obs_pass, wp.data(), n + 1);
    auto r = ObsConfigureAndStart("127.0.0.1", 4455, WideToUtf8(wp), g_ingest_server, g_ingest_key, true);
    SetStatus(Utf8ToWide(r.detail).c_str());
}

static void Layout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    const int m = 20;
    int y = m;
    
    MoveWindow(GetDlgItem(h, 400), m, y + 8, 50, 24, TRUE);
    MoveWindow(g_host, m + 54, y, 200, 36, TRUE);
    MoveWindow(GetDlgItem(h, 401), m + 270, y + 8, 40, 24, TRUE);
    MoveWindow(g_port, m + 314, y, 70, 36, TRUE);
    MoveWindow(GetDlgItem(h, 402), m + 400, y + 8, 40, 24, TRUE);
    MoveWindow(g_token, m + 444, y, 200, 36, TRUE);
    
    MoveWindow(GetDlgItem(h, IDC_CONNECT), rc.right - m - 230, y, 110, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_DISCONNECT), rc.right - m - 110, y, 110, 36, TRUE);
    y += 50;
    
    MoveWindow(g_status, m, y, rc.right - 2 * m, 24, TRUE);
    y += 36;
    
    MoveWindow(GetDlgItem(h, 410), m, y + 8, 80, 24, TRUE);
    MoveWindow(g_ingest, m + 84, y, rc.right - m - 460, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_COPY_INGEST), rc.right - m - 360, y, 90, 36, TRUE);
    
    MoveWindow(GetDlgItem(h, 411), rc.right - m - 250, y + 8, 40, 24, TRUE);
    MoveWindow(g_obs_pass, rc.right - m - 200, y, 90, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_OBS_PUSH), rc.right - m - 100, y, 100, 36, TRUE);
    y += 50;
    
    MoveWindow(GetDlgItem(h, 412), m, y + 8, 80, 24, TRUE);
    MoveWindow(g_title, m + 84, y, rc.right - m - 550, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_APPLY_TITLE), rc.right - m - 450, y, 120, 36, TRUE);
    
    MoveWindow(GetDlgItem(h, IDC_LAUNCH_CH), rc.right - m - 320, y, 100, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_CHROME), rc.right - m - 210, y, 100, 36, TRUE);
    MoveWindow(GetDlgItem(h, IDC_PUSH_KEYS), rc.right - m - 100, y, 100, 36, TRUE);
    y += 50;
    
    MoveWindow(g_accounts, m, y, rc.right - 2 * m, 100, TRUE);
    y += 116;
    
    int cards_h = 140;
    MoveWindow(g_view, m, y, rc.right - 2 * m, cards_h, TRUE);
    y += cards_h + 16;
    
    MoveWindow(GetDlgItem(h, 403), m, y, 300, 24, TRUE);
    y += 30;
    MoveWindow(g_chat, m, y, rc.right - 2 * m, rc.bottom - y - m, TRUE);
}

static LRESULT CALLBACK ViewProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        DrawViewCards(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
        RecreateFonts();

        auto lab = [&](const wchar_t* t, int id) {
            HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto ed = [&](int id, const wchar_t* v, DWORD extra = 0) {
            HWND c = CreateWindowExW(0, L"EDIT", v,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER | extra,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto btn = [&](const wchar_t* t, int id) {
            HWND c = CreateWindowExW(0, L"BUTTON", t,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            return c;
        };

        lab(L"سرور", 400); g_host = ed(IDC_HOST, L"91.216.104.42");
        lab(L"پورت", 401); g_port = ed(IDC_PORT, L"7788");
        lab(L"توکن", 402); g_token = ed(IDC_TOKEN, L"change-this-dashboard-token", ES_PASSWORD);
        
        btn(L"اتصال به سرور", IDC_CONNECT); btn(L"قطع ارتباط", IDC_DISCONNECT);
        g_status = lab(L"آماده...", IDC_STATUS);

        lab(L"لینک OBS", 410); g_ingest = ed(IDC_INGEST_BOX, L""); SendMessageW(g_ingest, EM_SETREADONLY, TRUE, 0);
        btn(L"کپی لینک", IDC_COPY_INGEST);
        lab(L"رمز OBS", 411); g_obs_pass = ed(IDC_OBS_PASS, L"", ES_PASSWORD);
        btn(L"تنظیم OBS", IDC_OBS_PUSH);

        lab(L"عنوان لایو", 412); g_title = ed(IDC_TITLE, L"");
        btn(L"تغییر عنوان", IDC_APPLY_TITLE);
        btn(L"اجرای مرورگر", IDC_LAUNCH_CH); btn(L"خواندن اطلاعات", IDC_CHROME); btn(L"ارسال به سرور", IDC_PUSH_KEYS);

        g_accounts = CreateWindowExW(0, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER,
                                     0, 0, 10, 10, h, (HMENU)IDC_ACCOUNTS, GetModuleHandleW(nullptr), nullptr);
        SetWindowFont(g_accounts, g_font);

        WNDCLASSW vc{}; vc.lpfnWndProc = ViewProc; vc.hInstance = GetModuleHandleW(nullptr);
        vc.lpszClassName = L"MRViewPanel"; vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&vc);
        g_view = CreateWindowExW(0, L"MRViewPanel", L"", WS_CHILD | WS_VISIBLE,
                                 0, 0, 10, 10, h, (HMENU)IDC_VIEWPANEL, GetModuleHandleW(nullptr), nullptr);

        lab(L"پیامهای زنده تماشاگران", 403);
        g_chat = CreateWindowExW(0, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | WS_BORDER |
                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                 0, 0, 10, 10, h, (HMENU)IDC_CHAT, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_chat, LB_SETITEMHEIGHT, 0, g_pt + 22);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlID == IDC_CHAT) { mi->itemHeight = g_pt + 22; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType == ODT_LISTBOX && di->CtlID == IDC_CHAT) { DrawChatItem(di); return TRUE; }
        if (di->CtlType == ODT_BUTTON) { DrawModernButton(di); return TRUE; }
        break;
    }
    case WM_SIZE: Layout(h); InvalidateRect(g_view, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: { ((MINMAXINFO*)l)->ptMinTrackSize = { 1100, 720 }; return 0; }
    
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_PANEL);
        SetTextColor(dc, C_TEXT);
        static HBRUSH br = CreateSolidBrush(C_PANEL);
        return (LRESULT)br;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_BG);
        SetTextColor(dc, C_TEXT_DIM);
        static HBRUSH br = CreateSolidBrush(C_BG);
        return (LRESULT)br;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(C_BG);
        FillRect((HDC)w, &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_CONNECT) Connect();
        if (id == IDC_DISCONNECT) Disconnect();
        if (id == IDC_CHROME) DoChrome();
        if (id == IDC_LAUNCH_CH) LaunchChromeDebug(9333);
        if (id == IDC_PUSH_KEYS) DoPushKeys();
        if (id == IDC_APPLY_TITLE) DoApplyTitle();
        if (id == IDC_OBS_PUSH) DoObs();
        if (id == IDC_COPY_INGEST && g_ingest) {
            int n = GetWindowTextLengthW(g_ingest);
            std::wstring t(n, 0); GetWindowTextW(g_ingest, t.data(), n + 1);
            if (OpenClipboard(h)) {
                EmptyClipboard();
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
                memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem); SetClipboardData(CF_UNICODETEXT, mem); CloseClipboard();
                SetStatus(L"آدرس کپی شد");
            }
        }
        return 0;
    }
    case WM_APP_LINE: { auto* s = reinterpret_cast<std::string*>(l); if (s) { ApplyLine(*s); delete s; } return 0; }
    case WM_APP_CONN: SetStatus(L"ارتباط با سرور قطع شد"); return 0;
    case WM_DESTROY: Disconnect(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    Logger::I().Init(ExeDir() + L"\\logs", "client", LogLevel::Debug);
    InitWinsock();
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = CreateSolidBrush(C_BG);
    wc.lpszClassName = L"MRClient"; RegisterClassW(&wc);

    g_wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYOUTRTL | WS_EX_COMPOSITED, wc.lpszClassName,
                            L"داشبورد مدیریت لایو استریم", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780, nullptr, nullptr, inst, nullptr);
    
    BOOL dark = TRUE; DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    DWORD corner = 2; DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));

    ShowWindow(g_wnd, show);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    WSACleanup(); return 0;
}
