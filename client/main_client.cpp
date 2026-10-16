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
#define IDC_FULLSCREEN  330

#define WM_APP_LINE     (WM_APP + 10)
#define WM_APP_CONN     (WM_APP + 11)

static const int kReconnectTimerId = 2;

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
static HACCEL g_accel = nullptr;

static SOCKET g_sock = INVALID_SOCKET;
static std::atomic<bool> g_run{false};
static std::thread g_rx;

static bool g_want_connected = false;
static int g_reconnect_delay_ms = 3000;

static bool g_high_contrast = false;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_prev_placement = { sizeof(WINDOWPLACEMENT) };

static bool g_srv_running = false;
static bool g_srv_publisher = false;
static double g_srv_kbps = 0.0;

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

static void RecalcAllChatHeights() {
    if (!g_chat) return;
    RECT rc{}; GetClientRect(g_chat, &rc);
    int text_w = (std::max)(180L, (rc.right - rc.left) - (g_pt + 68));
    HDC dc = GetDC(g_chat);
    HGDIOBJ old_f = SelectObject(dc, g_font_chat);
    std::lock_guard<std::mutex> g(g_mu);
    int count = static_cast<int>(g_msgs.size());
    for (int i = 0; i < count; ++i) {
        const auto& m = g_msgs[i];
        RECT calc{ 0, 0, text_w, 0 };
        std::wstring full = Utf8ToWide(m.user + ":   " + m.text);
        DrawTextW(dc, full.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK);
        int h = (std::max)(g_pt + 26, static_cast<int>(calc.bottom - calc.top) + 16);
        SendMessageW(g_chat, LB_SETITEMHEIGHT, static_cast<WPARAM>(i), static_cast<LPARAM>(h));
    }
    SelectObject(dc, old_f);
    ReleaseDC(g_chat, dc);
    InvalidateRect(g_chat, nullptr, TRUE);
}

static void RecreateFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_chat) DeleteObject(g_font_chat);
    g_font = MakeFont(g_pt, false);
    g_font_big = MakeFont(g_pt + 8, true);
    g_font_chat = MakeFont(g_pt + 2, false);
    if (g_chat) SetWindowFont(g_chat, g_font_chat);
    RecalcAllChatHeights();
    if (g_wnd) InvalidateRect(g_wnd, nullptr, TRUE);
}

static void ToggleContrast() {
    g_high_contrast = !g_high_contrast;
    if (g_high_contrast) {
        C_BG = RGB(0, 0, 0);
        C_PANEL = RGB(18, 18, 18);
        C_TEXT = RGB(255, 255, 255);
        C_TEXT_DIM = RGB(225, 225, 225);
    } else {
        C_BG = RGB(24, 25, 28);
        C_PANEL = RGB(43, 45, 49);
        C_TEXT = RGB(242, 243, 245);
        C_TEXT_DIM = RGB(148, 155, 164);
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, TRUE);
}

static void ToggleFullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{ sizeof(mi) };
        if (GetWindowPlacement(h, &g_prev_placement) &&
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(h, GWL_STYLE, style & ~(LONG)WS_OVERLAPPEDWINDOW);
            SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_prev_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

static void SetStatus(const wchar_t* s) {
    Logger::I().Info("ui", WideToUtf8(s ? s : L""));
    if (g_status) SetWindowTextW(g_status, s);
}

static void UpdateTitleBadge() {
    if (!g_wnd) return;
    std::wstring t = L"داشبورد مدیریت لایو استریم";
    if (g_srv_running) {
        if (g_srv_publisher) {
            wchar_t b[64];
            swprintf(b, 64, L" — پخش زنده (%.1f Mbps)", g_srv_kbps / 1000.0);
            t += b;
        } else {
            t += L" — در انتظار OBS";
        }
    }
    SetWindowTextW(g_wnd, t.c_str());
}

static void ScheduleReconnect() {
    if (!g_want_connected || !g_wnd) return;
    SetStatus(L"ارتباط قطع شد — تلاش مجدد خودکار...");
    SetTimer(g_wnd, kReconnectTimerId, g_reconnect_delay_ms, nullptr);
    g_reconnect_delay_ms = (std::min)(30000, g_reconnect_delay_ms * 2);
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
        g_srv_running = json::ExtractBool(line, "running", false);
        g_srv_publisher = json::ExtractBool(line, "publisher", false);
        g_srv_kbps = (double)json::ExtractInt(line, "kbps", 0);
        UpdateTitleBadge();
        SetStatus(L"متصل — وضعیت همگام شد");
    }
    if (type == "engine") {
        g_srv_running = json::ExtractBool(line, "running", false);
        g_srv_publisher = json::ExtractBool(line, "publisher", false);
        g_srv_kbps = (double)json::ExtractInt(line, "kbps", 0);
        UpdateTitleBadge();
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

static void Disconnect(bool user_initiated = true) {
    g_run.store(false);
    if (g_sock != INVALID_SOCKET) {
        shutdown(g_sock, SD_BOTH);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    if (g_rx.joinable() && g_rx.get_id() != std::this_thread::get_id())
        g_rx.join();
    if (user_initiated) {
        g_want_connected = false;
        if (g_wnd) KillTimer(g_wnd, kReconnectTimerId);
        SetStatus(L"قطع شد");
    }
}

static void Connect() {
    Disconnect(false);
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
        ScheduleReconnect();
        return;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        SetStatus(L"اتصال برقرار نشد — فایروال سرور را چک کنید");
        closesocket(s); freeaddrinfo(res);
        ScheduleReconnect();
        return;
    }
    freeaddrinfo(res);
    g_reconnect_delay_ms = 3000;
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

static int DrawPillBadge(HDC dc, int x, int y, int h, const wchar_t* label, COLORREF bg_col, COLORREF txt_col) {
    SelectObject(dc, g_font);
    RECT calc{ 0, 0, 200, h };
    DrawTextW(dc, label, -1, &calc, DT_CALCRECT | DT_SINGLELINE);
    int w = (calc.right - calc.left) + 12;
    RECT pill{ x, y + 2, x + w, y + h - 2 };
    HBRUSH br = CreateSolidBrush(bg_col);
    HPEN pen = CreatePen(PS_SOLID, 1, bg_col);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, 6, 6);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt_col);
    DrawTextW(dc, label, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w + 6;
}

static void DrawChatItem(DRAWITEMSTRUCT* dis) {
    if (dis->itemID == static_cast<UINT>(-1)) return;
    ChatMessage m;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (dis->itemID >= g_msgs.size()) return;
        m = g_msgs[dis->itemID];
    }
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool sel = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF row_bg = sel ? RGB(42, 48, 66) : ((dis->itemID % 2 == 0) ? C_PANEL : RGB(36, 38, 44));
    HBRUSH br = CreateSolidBrush(row_bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    int badge_sz = g_pt + 12;
    RECT badge{ r.left + 10, r.top + 6, r.left + 10 + badge_sz, r.top + 6 + badge_sz };
    DrawLogoBadge(dc, badge, m.platform);

    int cur_x = badge.right + 10;
    int line_h = g_pt + 10;
    if (m.is_mod) cur_x += DrawPillBadge(dc, cur_x, r.top + 6, line_h, L"MOD", RGB(16, 185, 129), RGB(10, 25, 20));
    if (m.is_sub) cur_x += DrawPillBadge(dc, cur_x, r.top + 6, line_h, L"SUB", RGB(139, 92, 246), RGB(255, 255, 255));

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_chat);
    std::wstring user_str = Utf8ToWide(m.user) + L": ";
    SetTextColor(dc, PlatformColor(static_cast<int>(m.platform)));
    RECT u_calc{ cur_x, r.top + 6, r.right - 12, r.top + 6 + line_h };
    DrawTextW(dc, user_str.c_str(), -1, &u_calc, DT_CALCRECT | DT_SINGLELINE);
    DrawTextW(dc, user_str.c_str(), -1, &u_calc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    std::wstring msg_str = Utf8ToWide(m.text);
    SetTextColor(dc, C_TEXT);
    RECT m_rc{ u_calc.right + 4, r.top + 6, r.right - 12, r.bottom - 6 };
    if (m_rc.right - m_rc.left < 120) {
        m_rc.left = badge.right + 10;
        m_rc.top += line_h + 2;
    }
    DrawTextW(dc, msg_str.c_str(), -1, &m_rc, DT_LEFT | DT_TOP | DT_WORDBREAK);
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
    const int pad = 16;
    const int side_w = 400;
    int side_x = rc.right - side_w - pad;
    int main_w = side_x - pad * 2;

    // top bar
    int top_y = 8;
    MoveWindow(GetDlgItem(h, IDC_FULLSCREEN), pad, top_y, 34, 26, TRUE);
    MoveWindow(GetDlgItem(h, IDC_CONTRAST), pad + 38, top_y, 34, 26, TRUE);
    MoveWindow(GetDlgItem(h, IDC_FONTMINUS), pad + 76, top_y, 34, 26, TRUE);
    MoveWindow(GetDlgItem(h, IDC_FONTPLUS), pad + 114, top_y, 34, 26, TRUE);
    MoveWindow(g_status, pad + 156, top_y + 2, main_w - 156, 22, TRUE);

    // main canvas
    int main_y = 42;
    int cards_h = 110;
    MoveWindow(g_view, pad, main_y, main_w, cards_h, TRUE);
    main_y += cards_h + 8;
    MoveWindow(GetDlgItem(h, 403), pad, main_y, main_w, 20, TRUE);
    main_y += 24;
    MoveWindow(g_chat, pad, main_y, main_w, rc.bottom - main_y - pad, TRUE);

    // sidebar
    int sx = side_x + 10;
    int sw = side_w - 20;
    int sy = 42;

    // server + port row
    MoveWindow(GetDlgItem(h, 400), sx, sy, 40, 20, TRUE);
    MoveWindow(g_host, sx + 44, sy, sw - 130, 28, TRUE);
    MoveWindow(GetDlgItem(h, 401), sx + sw - 78, sy, 32, 20, TRUE);
    MoveWindow(g_port, sx + sw - 42, sy, 42, 28, TRUE);
    sy += 34;

    // token
    MoveWindow(GetDlgItem(h, 402), sx, sy, 40, 20, TRUE);
    MoveWindow(g_token, sx + 44, sy, sw - 44, 28, TRUE);
    sy += 36;

    // connect buttons
    int half = (sw - 6) / 2;
    MoveWindow(GetDlgItem(h, IDC_CONNECT), sx, sy, half, 30, TRUE);
    MoveWindow(GetDlgItem(h, IDC_DISCONNECT), sx + half + 6, sy, half, 30, TRUE);
    sy += 42;

    // OBS link
    MoveWindow(GetDlgItem(h, 410), sx, sy, sw, 18, TRUE);
    sy += 20;
    MoveWindow(g_ingest, sx, sy, sw - 86, 28, TRUE);
    MoveWindow(GetDlgItem(h, IDC_COPY_INGEST), sx + sw - 80, sy, 80, 28, TRUE);
    sy += 34;

    // OBS pass
    MoveWindow(GetDlgItem(h, 411), sx, sy, 50, 20, TRUE);
    MoveWindow(g_obs_pass, sx + 54, sy, sw - 150, 28, TRUE);
    MoveWindow(GetDlgItem(h, IDC_OBS_PUSH), sx + sw - 90, sy, 90, 28, TRUE);
    sy += 40;

    // title
    MoveWindow(GetDlgItem(h, 412), sx, sy, sw, 18, TRUE);
    sy += 20;
    MoveWindow(g_title, sx, sy, sw - 100, 28, TRUE);
    MoveWindow(GetDlgItem(h, IDC_APPLY_TITLE), sx + sw - 94, sy, 94, 28, TRUE);
    sy += 36;

    // chrome buttons
    int third = (sw - 10) / 3;
    MoveWindow(GetDlgItem(h, IDC_LAUNCH_CH), sx, sy, third, 30, TRUE);
    MoveWindow(GetDlgItem(h, IDC_CHROME), sx + third + 5, sy, third, 30, TRUE);
    MoveWindow(GetDlgItem(h, IDC_PUSH_KEYS), sx + 2 * (third + 5), sy, third, 30, TRUE);
    sy += 36;

    MoveWindow(g_accounts, sx, sy, sw, (std::max)(60L, rc.bottom - sy - pad - 6), TRUE);
    RecalcAllChatHeights();
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
            HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto ed = [&](int id, const wchar_t* v, DWORD extra = 0) {
            HWND c = CreateWindowExW(0, L"EDIT", v,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto btn = [&](const wchar_t* t, int id) {
            HWND c = CreateWindowExW(0, L"BUTTON", t,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            return c;
        };

        lab(L"سرور", 400); g_host = ed(IDC_HOST, L"");
        lab(L"پورت", 401); g_port = ed(IDC_PORT, L"7788");
        lab(L"توکن", 402); g_token = ed(IDC_TOKEN, L"", ES_PASSWORD);
        
        btn(L"اتصال", IDC_CONNECT); btn(L"قطع", IDC_DISCONNECT);
        g_status = CreateWindowExW(0, L"STATIC", L"آماده...", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 10, 10, h, (HMENU)IDC_STATUS, GetModuleHandleW(nullptr), nullptr);
        SetWindowFont(g_status, g_font);

        btn(L"A-", IDC_FONTMINUS);
        btn(L"A+", IDC_FONTPLUS);
        btn(L"◐", IDC_CONTRAST);
        btn(L"⛶", IDC_FULLSCREEN);

        lab(L"لینک OBS", 410); g_ingest = ed(IDC_INGEST_BOX, L""); SendMessageW(g_ingest, EM_SETREADONLY, TRUE, 0);
        btn(L"کپی لینک", IDC_COPY_INGEST);
        lab(L"رمز OBS", 411); g_obs_pass = ed(IDC_OBS_PASS, L"", ES_PASSWORD);
        btn(L"تنظیم OBS", IDC_OBS_PUSH);

        lab(L"عنوان لایو", 412); g_title = ed(IDC_TITLE, L"");
        btn(L"تغییر عنوان", IDC_APPLY_TITLE);
        btn(L"کروم", IDC_LAUNCH_CH); btn(L"خواندن", IDC_CHROME); btn(L"ارسال", IDC_PUSH_KEYS);

        g_accounts = CreateWindowExW(0, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                     0, 0, 10, 10, h, (HMENU)IDC_ACCOUNTS, GetModuleHandleW(nullptr), nullptr);
        SetWindowFont(g_accounts, g_font);

        WNDCLASSW vc{}; vc.lpfnWndProc = ViewProc; vc.hInstance = GetModuleHandleW(nullptr);
        vc.lpszClassName = L"MRViewPanel"; vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&vc);
        g_view = CreateWindowExW(0, L"MRViewPanel", L"", WS_CHILD | WS_VISIBLE,
                                 0, 0, 10, 10, h, (HMENU)IDC_VIEWPANEL, GetModuleHandleW(nullptr), nullptr);

        lab(L"چت زنده تماشاگران", 403);
        g_chat = CreateWindowExW(0, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                 LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                 0, 0, 10, 10, h, (HMENU)IDC_CHAT, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_chat, LB_SETITEMHEIGHT, 0, g_pt + 22);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlID == IDC_CHAT && g_chat) {
            std::lock_guard<std::mutex> g(g_mu);
            if (mi->itemID < g_msgs.size()) {
                const auto& m = g_msgs[mi->itemID];
                RECT rcClient{}; GetClientRect(g_chat, &rcClient);
                int text_w = (std::max)(200L, (rcClient.right - rcClient.left) - (g_pt + 60));
                HDC dc = GetDC(g_chat);
                HGDIOBJ of = SelectObject(dc, g_font_chat);
                RECT calc{ 0, 0, text_w, 0 };
                std::wstring full = Utf8ToWide(m.user + ": " + m.text);
                DrawTextW(dc, full.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK);
                SelectObject(dc, of);
                ReleaseDC(g_chat, dc);
                mi->itemHeight = (std::max)(g_pt + 26, (int)(calc.bottom - calc.top) + 16);
                return TRUE;
            }
            mi->itemHeight = g_pt + 26;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType == ODT_LISTBOX && di->CtlID == IDC_CHAT) { DrawChatItem(di); return TRUE; }
        if (di->CtlType == ODT_BUTTON) { DrawModernButton(di); return TRUE; }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE: Layout(h); InvalidateRect(g_view, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: { ((MINMAXINFO*)l)->ptMinTrackSize = { 1100, 720 }; return 0; }
    
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_PANEL);
        SetTextColor(dc, C_TEXT);
        static HBRUSH br = nullptr;
        static COLORREF last = (COLORREF)-1;
        if (!br || last != C_PANEL) { if (br) DeleteObject(br); br = CreateSolidBrush(C_PANEL); last = C_PANEL; }
        return (LRESULT)br;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkColor(dc, C_BG);
        SetTextColor(dc, C_TEXT_DIM);
        static HBRUSH br = nullptr;
        static COLORREF last = (COLORREF)-1;
        if (!br || last != C_BG) { if (br) DeleteObject(br); br = CreateSolidBrush(C_BG); last = C_BG; }
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
        if (id == IDC_CONNECT) { g_want_connected = true; g_reconnect_delay_ms = 3000; Connect(); }
        if (id == IDC_DISCONNECT) Disconnect(true);
        if (id == IDC_CHROME) DoChrome();
        if (id == IDC_LAUNCH_CH) LaunchChromeDebug(9333);
        if (id == IDC_PUSH_KEYS) DoPushKeys();
        if (id == IDC_APPLY_TITLE) DoApplyTitle();
        if (id == IDC_OBS_PUSH) DoObs();
        if (id == IDC_FONTPLUS) { g_pt = (std::min)(28, g_pt + 2); RecreateFonts(); Layout(h); }
        if (id == IDC_FONTMINUS) { g_pt = (std::max)(12, g_pt - 2); RecreateFonts(); Layout(h); }
        if (id == IDC_CONTRAST) ToggleContrast();
        if (id == IDC_FULLSCREEN) ToggleFullscreen(h);
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
    case WM_TIMER:
        if (w == (WPARAM)kReconnectTimerId) {
            KillTimer(h, kReconnectTimerId);
            if (g_want_connected) Connect();
        }
        return 0;
    case WM_APP_LINE: { auto* s = reinterpret_cast<std::string*>(l); if (s) { ApplyLine(*s); delete s; } return 0; }
    case WM_APP_CONN:
        SetStatus(L"ارتباط با سرور قطع شد");
        g_srv_running = false; g_srv_publisher = false; UpdateTitleBadge();
        ScheduleReconnect();
        return 0;
    case WM_DESTROY: KillTimer(h, kReconnectTimerId); Disconnect(true); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    Logger::I().Init(ExeDir() + L"\\logs", "client", LogLevel::Debug);
    InitWinsock();
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = CreateSolidBrush(C_BG);
    wc.lpszClassName = L"MRClient"; RegisterClassW(&wc);

    g_wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_COMPOSITED, wc.lpszClassName,
                            L"داشبورد مدیریت لایو استریم", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780, nullptr, nullptr, inst, nullptr);
    
    BOOL dark = TRUE; DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    DWORD corner = 2; DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS,  IDC_FONTPLUS },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDC_FONTMINUS },
        { FVIRTKEY,            VK_F11,       IDC_FULLSCREEN },
    };
    g_accel = CreateAcceleratorTableW(accels, 3);

    ShowWindow(g_wnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!g_accel || !TranslateAcceleratorW(g_wnd, g_accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_accel) DestroyAcceleratorTable(g_accel);
    WSACleanup(); return 0;
}
