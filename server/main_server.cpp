#include "../shared/win_util.h"
#include "../shared/config.h"
#include "../shared/json_mini.h"
#include "../shared/logger.h"
#include "restream_engine.h"
#include "chat_hub.h"
#include "control_server.h"
#include "ui_ids.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <sstream>
#include <deque>
#include <memory>
#include <fstream>
#include <atomic>
#include <iostream>

using namespace mr;

static HWND g_wnd = nullptr;
static HWND g_ingest = nullptr;
static HWND g_log = nullptr;
static HWND g_dest = nullptr;
static HWND g_status = nullptr;
static HWND g_clients = nullptr;
static HFONT g_font = nullptr;
static HFONT g_font_sm = nullptr;

static AppConfig g_cfg;
static std::wstring g_cfg_path;
static std::unique_ptr<RestreamEngine> g_engine;
static std::unique_ptr<ChatHub> g_chat;
static std::unique_ptr<ControlServer> g_ctrl;

static std::mutex g_view_mu;
static std::vector<ViewerSnapshot> g_views;

struct DestRow {
    DestRuntime rt;
    ViewerSnapshot view;
    bool has_view = false;
};
static std::vector<DestRow> g_dest_rows;

static COLORREF C_BG         = RGB(24, 25, 28);
static COLORREF C_PANEL      = RGB(43, 45, 49);
static COLORREF C_TEXT       = RGB(242, 243, 245);
static COLORREF C_TEXT_DIM   = RGB(148, 155, 164);
static COLORREF C_BTN_PRM    = RGB(88, 101, 242);
static COLORREF C_BTN_PRM_DN = RGB(71, 82, 196);
static COLORREF C_BTN_SEC    = RGB(78, 80, 88);
static COLORREF C_BTN_SEC_DN = RGB(104, 109, 115);
static COLORREF C_BTN_DANGER = RGB(237, 66, 69);
static COLORREF C_BTN_DAN_DN = RGB(196, 52, 54);
static COLORREF C_LIVE       = RGB(88, 255, 120);
static COLORREF C_WARN       = RGB(230, 200, 90);
static COLORREF C_OFFLINE    = RGB(255, 140, 100);

static bool WantsConsole();

static void UiLog(const std::string& s) {
    Logger::I().Info("ui", s);
    if (g_wnd) {
        auto* heap = new std::wstring(Utf8ToWide(s));
        PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)heap);
    } else if (WantsConsole()) {
        std::cout << s << std::endl;
    }
}

static void AppendLog(const std::wstring& line) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[32]; swprintf(ts, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::wstring full = std::wstring(ts) + line + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    if (len > 20000) {
        SendMessageW(g_log, EM_SETSEL, 0, 8000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)full.c_str());
}

static std::string PublicHostGuess() {
    if (!g_cfg.public_host.empty() && g_cfg.public_host != "YOUR_VPS_IP") return g_cfg.public_host;
    char name[256]{}; if (gethostname(name, sizeof(name)) != 0) return "VPS_IP";
    addrinfo hints{}; hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) return name;
    char ip[64]{}; inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res); return ip[0] ? ip : name;
}

static void RefreshDestList() {
    if (!g_engine || !g_dest) return;
    auto dests = g_engine->Destinations();
    std::vector<ViewerSnapshot> views;
    { std::lock_guard<std::mutex> g(g_view_mu); views = g_views; }

    bool count_changed = (dests.size() != g_dest_rows.size());
    int prev_sel = static_cast<int>(SendMessageW(g_dest, LB_GETCURSEL, 0, 0));

    g_dest_rows.clear();
    if (count_changed) SendMessageW(g_dest, LB_RESETCONTENT, 0, 0);

    for (const auto& r : dests) {
        DestRow row;
        row.rt = r;
        for (const auto& v : views) {
            if (v.platform == r.dest.platform) { row.view = v; row.has_view = true; break; }
        }
        g_dest_rows.push_back(row);
        if (count_changed) {
            std::wstring fallback = Utf8ToWide(r.dest.title.empty() ? r.dest.id : r.dest.title);
            SendMessageW(g_dest, LB_ADDSTRING, 0, (LPARAM)fallback.c_str());
        }
    }
    if (count_changed && prev_sel >= 0 && prev_sel < static_cast<int>(g_dest_rows.size()))
        SendMessageW(g_dest, LB_SETCURSEL, prev_sel, 0);

    InvalidateRect(g_dest, nullptr, FALSE);
}

static void RefreshIngestBox() {
    SetWindowTextW(g_ingest, Utf8ToWide(g_cfg.IngestUrlHint(PublicHostGuess())).c_str());
}

static void BroadcastHello() {
    if (!g_ctrl) return;
    std::ostringstream o; o << "{\"type\":\"hello\",\"version\":\"" << kProtocolVersion << "\"}";
    g_ctrl->Broadcast(o.str());
}

static void BroadcastChat(const ChatMessage& m) {
    if (!g_ctrl) return;
    std::ostringstream o;
    o << "{\"type\":\"chat\",\"platform\":\"" << PlatformId(m.platform) << "\",\"user\":\"" << json::Escape(m.user)
      << "\",\"text\":\"" << json::Escape(m.text) << "\",\"ts\":" << m.ts_ms
      << ",\"mod\":" << (m.is_mod ? "true" : "false") << ",\"sub\":" << (m.is_sub ? "true" : "false") << "}";
    g_ctrl->Broadcast(o.str());
}

static void BroadcastView(const ViewerSnapshot& v) {
    {
        std::lock_guard<std::mutex> g(g_view_mu);
        bool found = false;
        for (auto& x : g_views) { if (x.platform == v.platform && x.name == v.name) { x = v; found = true; break; } }
        if (!found) g_views.push_back(v);
    }
    if (!g_ctrl) return;
    std::ostringstream o;
    o << "{\"type\":\"viewers\",\"platform\":\"" << PlatformId(v.platform) << "\",\"name\":\"" << json::Escape(v.name)
      << "\",\"viewers\":" << v.viewers << ",\"live\":" << (v.live ? "true" : "false")
      << ",\"connected\":" << (v.dest_connected ? "true" : "false") << ",\"status\":\"" << json::Escape(v.status) << "\"}";
    g_ctrl->Broadcast(o.str());
}

static void BroadcastEngine() {
    if (!g_ctrl || !g_engine) return;
    auto st = g_engine->Status();
    std::ostringstream o;
    o << "{\"type\":\"engine\",\"running\":" << (st.running ? "true" : "false")
      << ",\"publisher\":" << (st.publisher_connected ? "true" : "false")
      << ",\"kbps\":" << (long long)st.kbps << "}";
    g_ctrl->Broadcast(o.str());
}

static std::string HandleClientCmd(const std::string& line) {
    std::string type = json::ExtractString(line, "type");
    if (type == "ping") return "{\"type\":\"pong\"}";
    if (type == "get_ingest") {
        std::string host = PublicHostGuess();
        std::ostringstream o;
        o << "{\"type\":\"ingest\",\"host\":\"" << json::Escape(host) << "\",\"port\":" << g_cfg.rtmp_port
          << ",\"app\":\"" << json::Escape(g_cfg.ingest_app) << "\",\"key\":\"" << json::Escape(g_cfg.ingest_key)
          << "\",\"server\":\"rtmp://" << host << ":" << g_cfg.rtmp_port << "/" << g_cfg.ingest_app << "\""
          << ",\"url\":\"" << json::Escape(g_cfg.IngestUrlHint(host)) << "\"}";
        return o.str();
    }
    if (type == "set_title") { return "{\"type\":\"ok\"}"; }
    if (type == "set_destination") {
        Destination d; d.id = json::ExtractString(line, "id");
        d.platform = PlatformFromId(json::ExtractString(line, "platform"));
        d.title = json::ExtractString(line, "title"); d.rtmp_url = json::ExtractString(line, "rtmp_url");
        d.channel = json::ExtractString(line, "channel"); d.oauth_token = json::ExtractString(line, "oauth_token");
        d.enabled = json::ExtractBool(line, "enabled", true);
        if (d.id.empty()) d.id = PlatformId(d.platform);
        if (g_engine) g_engine->UpsertDestination(d);
        g_cfg = g_engine->GetConfig(); std::string err; SaveConfigToFile(WideToUtf8(g_cfg_path), g_cfg, err);
        PostMessageW(g_wnd, WM_COMMAND, IDC_RELOAD, 0); return "{\"type\":\"ok\"}";
    }
    if (type == "get_state") {
        EngineStatus est = g_engine ? g_engine->Status() : EngineStatus{};
        std::ostringstream o;
        o << "{\"type\":\"state\",\"running\":" << (est.running ? "true" : "false")
          << ",\"publisher\":" << (est.publisher_connected ? "true" : "false")
          << ",\"kbps\":" << (long long)est.kbps
          << ",\"ingest\":\"" << json::Escape(g_cfg.IngestUrlHint(PublicHostGuess())) << "\",\"dests\":[";
        auto dests = g_engine ? g_engine->Destinations() : std::vector<DestRuntime>{};
        for (size_t i = 0; i < dests.size(); ++i) {
            if (i) o << ",";
            o << "{\"id\":\"" << json::Escape(dests[i].dest.id) << "\",\"platform\":\"" << PlatformId(dests[i].dest.platform)
              << "\",\"title\":\"" << json::Escape(dests[i].dest.title) << "\",\"enabled\":" << (dests[i].dest.enabled ? "true" : "false") << "}";
        }
        o << "],\"viewers\":[";
        std::lock_guard<std::mutex> g(g_view_mu);
        for (size_t i = 0; i < g_views.size(); ++i) {
            if (i) o << ",";
            o << "{\"platform\":\"" << PlatformId(g_views[i].platform) << "\",\"name\":\"" << json::Escape(g_views[i].name)
              << "\",\"viewers\":" << g_views[i].viewers << ",\"live\":" << (g_views[i].live ? "true" : "false")
              << ",\"status\":\"" << json::Escape(g_views[i].status) << "\"}";
        }
        o << "]}"; return o.str();
    }
    if (type == "start" && g_engine) { g_engine->Start(); return "{\"type\":\"ok\"}"; }
    if (type == "stop"  && g_engine) { g_engine->Stop();  return "{\"type\":\"ok\"}"; }
    if (type == "toggle") {
        if (g_engine) g_engine->SetDestinationEnabled(json::ExtractString(line, "id"), json::ExtractBool(line, "enabled", true));
        PostMessageW(g_wnd, WM_COMMAND, IDC_RELOAD, 0); return "{\"type\":\"ok\"}";
    }
    return "{\"type\":\"error\",\"error\":\"unknown\"}";
}

static void LoadOrCreateConfig() {
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; dir.resize(dir.find_last_of(L"\\/"));
    g_cfg_path = dir + L"\\config.json";
    std::string err;
    if (!LoadConfigFromFile(WideToUtf8(g_cfg_path), g_cfg, err)) {
        std::ofstream out(WideToUtf8(g_cfg_path), std::ios::binary); out << DefaultConfigJson();
        LoadConfigFromFile(WideToUtf8(g_cfg_path), g_cfg, err);
    }
}

static void DrawModernButton(DRAWITEMSTRUCT* di) {
    bool is_start = (di->CtlID == IDC_START);
    bool is_stop = (di->CtlID == IDC_STOP);
    bool pushed = (di->itemState & ODS_SELECTED);
    
    COLORREF bg;
    if (is_start) bg = pushed ? C_BTN_PRM_DN : C_BTN_PRM;
    else if (is_stop) bg = pushed ? C_BTN_DAN_DN : C_BTN_DANGER;
    else bg = pushed ? C_BTN_SEC_DN : C_BTN_SEC;
                         
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(di->hDC, &di->rcItem, br);
    DeleteObject(br);
    
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, C_TEXT);
    SelectObject(di->hDC, g_font);
    
    wchar_t txt[128]; GetWindowTextW(di->hwndItem, txt, 128);
    DrawTextW(di->hDC, txt, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawDestRow(DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1 || di->itemID >= g_dest_rows.size()) return;
    const auto& row = g_dest_rows[di->itemID];
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH br = CreateSolidBrush(sel ? RGB(53, 55, 63) : C_PANEL);
    FillRect(dc, &r, br);
    DeleteObject(br);

    RECT dot{ r.left + 10, r.top + 5, r.left + 10 + 26, r.bottom - 5 };
    COLORREF pc = PlatformColor((int)row.rt.dest.platform);
    HBRUSH pb = CreateSolidBrush(row.rt.dest.enabled ? pc : RGB(90, 90, 95));
    HGDIOBJ ob = SelectObject(dc, pb);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, ob);
    DeleteObject(pb);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, g_font_sm);
    DrawTextW(dc, PlatformGlyph((int)row.rt.dest.platform), -1, &dot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT title{ dot.right + 10, r.top + 2, r.left + (r.right - r.left) * 55 / 100, r.bottom - 2 };
    SetTextColor(dc, C_TEXT);
    SelectObject(dc, g_font);
    std::wstring t = Utf8ToWide(row.rt.dest.title.empty() ? row.rt.dest.id : row.rt.dest.title);
    if (!row.rt.dest.enabled) t += L"  (خاموش)";
    DrawTextW(dc, t.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT stat{ title.right, r.top + 2, r.right - 12, r.bottom - 2 };
    std::wstring s;
    COLORREF sc = C_TEXT_DIM;
    if (!row.rt.dest.enabled) {
        s = L"غیرفعال";
    } else if (row.has_view && row.view.live) {
        s = L"زنده — " + FormatViewers(row.view.viewers) + L" بیننده";
        sc = C_LIVE;
    } else if (row.has_view) {
        s = Utf8ToWide(row.view.status.empty() ? "آفلاین" : row.view.status);
        sc = C_OFFLINE;
    } else if (row.rt.process_alive) {
        s = L"در حال ارسال…";
        sc = C_WARN;
    } else {
        s = L"—";
    }
    SetTextColor(dc, sc);
    DrawTextW(dc, s.c_str(), -1, &stat, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void Layout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    const int m = 16;
    int y = m;
    MoveWindow(GetDlgItem(h, 200), m, y, rc.right - 2 * m, 22, TRUE); y += 26;
    MoveWindow(g_ingest, m, y, rc.right - 2 * m - 100, 32, TRUE);
    MoveWindow(GetDlgItem(h, IDC_COPY), rc.right - m - 92, y, 92, 32, TRUE); y += 44;
    MoveWindow(GetDlgItem(h, IDC_START), m, y, 110, 34, TRUE);
    MoveWindow(GetDlgItem(h, IDC_STOP), m + 118, y, 110, 34, TRUE);
    MoveWindow(g_status, m + 240, y + 6, 220, 22, TRUE);
    MoveWindow(g_clients, rc.right - m - 140, y + 6, 140, 22, TRUE); y += 46;
    MoveWindow(GetDlgItem(h, 201), m, y, rc.right - 2 * m, 22, TRUE); y += 26;
    int dest_h = 120;
    MoveWindow(g_dest, m, y, rc.right - 2 * m, dest_h, TRUE); y += dest_h + 14;
    MoveWindow(GetDlgItem(h, 202), m, y, rc.right - 2 * m, 22, TRUE); y += 26;
    MoveWindow(g_log, m, y, rc.right - 2 * m, rc.bottom - y - m, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_font = MakeFont(14, false); g_font_sm = MakeFont(12, false);

        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int id) {
            HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto ed = [&](int id, DWORD exst = 0) {
            HWND c = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | exst,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowFont(c, g_font); return c;
        };
        auto btn = [&](const wchar_t* t, int id) {
            HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            return c;
        };

        mk(L"STATIC", L"آدرس اینجست OBS:", SS_RIGHT, 200);
        g_ingest = ed(IDC_INGEST, ES_READONLY | ES_AUTOHSCROLL);
        btn(L"کپی لینک", IDC_COPY);
        btn(L"شروع استریم", IDC_START); btn(L"توقف استریم", IDC_STOP);
        g_status = mk(L"STATIC", L"وضعیت: خاموش", 0, IDC_STATUS);
        g_clients = mk(L"STATIC", L"کلاینت: ۰", SS_RIGHT, IDC_CLIENTS);
        mk(L"STATIC", L"مقصدها (دوبار کلیک برای روشن/خاموش):", SS_RIGHT, 201);
        
        g_dest = CreateWindowExW(0, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                 0, 0, 10, 10, h, (HMENU)IDC_DEST_LIST, GetModuleHandleW(nullptr), nullptr);
        SetWindowFont(g_dest, g_font);
        
        mk(L"STATIC", L"گزارش سرور:", SS_RIGHT, 202);
        g_log = ed(IDC_LOG, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
        SetWindowFont(g_log, g_font_sm);
        SetTimer(h, 1, 1000, nullptr);
        return 0;
    }
    case WM_SIZE: Layout(h); return 0;
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlID == IDC_DEST_LIST) { mi->itemHeight = 34; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType == ODT_LISTBOX && di->CtlID == IDC_DEST_LIST) { DrawDestRow(di); return TRUE; }
        if (di->CtlType == ODT_BUTTON) { DrawModernButton(di); return TRUE; }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w; SetBkColor(dc, C_PANEL); SetTextColor(dc, C_TEXT);
        static HBRUSH br = CreateSolidBrush(C_PANEL); return (LRESULT)br;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w; SetBkColor(dc, C_BG); SetTextColor(dc, C_TEXT_DIM);
        static HBRUSH br = CreateSolidBrush(C_BG); return (LRESULT)br;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(C_BG); FillRect((HDC)w, &rc, br); DeleteObject(br); return 1;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == IDC_COPY) {
            int n = GetWindowTextLengthW(g_ingest);
            std::wstring t(n, 0); GetWindowTextW(g_ingest, t.data(), n + 1);
            if (OpenClipboard(h)) {
                EmptyClipboard();
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
                memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem); SetClipboardData(CF_UNICODETEXT, mem); CloseClipboard();
                UiLog("آدرس اینجست کپی شد");
            }
        }
        if (id == IDC_START && g_engine) { if (g_engine->Start()) SetWindowTextW(g_status, L"وضعیت: در انتظار OBS"); }
        if (id == IDC_STOP && g_engine) { g_engine->Stop(); SetWindowTextW(g_status, L"وضعیت: خاموش"); }
        if (id == IDC_DEST_LIST && code == LBN_DBLCLK && g_engine) {
            int sel = (int)SendMessageW(g_dest, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                auto dests = g_engine->Destinations();
                g_engine->SetDestinationEnabled(dests[sel].dest.id, !dests[sel].dest.enabled);
                RefreshDestList();
            }
        }
        if (id == IDC_RELOAD) RefreshDestList();
        return 0;
    }
    case WM_TIMER:
        if (g_engine) g_engine->Tick();
        RefreshDestList();
        BroadcastEngine();
        if (g_ctrl) { wchar_t b[64]; swprintf(b, 64, L"کلاینتهای داشبورد: %d", g_ctrl->ClientCount()); SetWindowTextW(g_clients, b); }
        if (g_engine) {
            auto st = g_engine->Status();
            wchar_t b2[160];
            if (!st.running) swprintf(b2, 160, L"وضعیت: خاموش");
            else if (!st.publisher_connected) swprintf(b2, 160, L"وضعیت: در انتظار اتصال OBS");
            else swprintf(b2, 160, L"وضعیت: در حال پخش زنده — %.1f Mbps", st.kbps / 1000.0);
            SetWindowTextW(g_status, b2);
        }
        return 0;
    case WM_APP_LOG: { auto* s = reinterpret_cast<std::wstring*>(l); if (s) { AppendLog(*s); delete s; } return 0; }
    case WM_DESTROY: KillTimer(h, 1); if (g_ctrl) g_ctrl->Stop(); if (g_chat) g_chat->Stop(); if (g_engine) g_engine->Stop(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void ServerHeartbeatTick() {
    if (g_engine) g_engine->Tick();
    BroadcastEngine();
}

static bool WantsConsole() {
    std::wstring cl = GetCommandLineW();
    return cl.find(L"--console") != std::wstring::npos || cl.find(L"--headless") != std::wstring::npos;
}

static void StartServices() {
    g_engine = std::make_unique<RestreamEngine>([](const std::string& s) { UiLog(s); });
    g_engine->SetConfig(g_cfg);
    g_chat = std::make_unique<ChatHub>([](const ChatMessage& m) { BroadcastChat(m); },
                                       [](const ViewerSnapshot& v) { BroadcastView(v); }, [](const std::string& s) { UiLog(s); });
    g_chat->SetConfig(g_cfg); g_chat->Start();
    g_ctrl = std::make_unique<ControlServer>(g_cfg.control_port, g_cfg.auth_token, HandleClientCmd, [](const std::string& s) { UiLog(s); });
    g_ctrl->Start(); BroadcastHello();
}
static std::atomic<bool> g_headless_run{true};
static BOOL WINAPI ConsoleCtrl(DWORD) { g_headless_run.store(false); return TRUE; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    Logger::I().Init(ExeDir() + L"\\logs", "server", LogLevel::Debug);
    InitWinsock(); LoadOrCreateConfig();

    if (WantsConsole()) {
        AllocConsole();
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleTitleW(L"MultiRestream Server (Headless)");
        SetConsoleCtrlHandler(ConsoleCtrl, TRUE);

        StartServices();
        if (g_engine) g_engine->Start();
        while (g_headless_run.load()) {
            ServerHeartbeatTick();
            Sleep(1000);
        }
        if (g_ctrl) g_ctrl->Stop();
        if (g_chat) g_chat->Stop();
        if (g_engine) g_engine->Stop();
        WSACleanup();
        return 0;
    }

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(C_BG); wc.lpszClassName = L"MRServer"; RegisterClassW(&wc);

    g_wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYOUTRTL | WS_EX_COMPOSITED, wc.lpszClassName,
                            L"هسته سرور مالتیاستریم", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, inst, nullptr);
    
    BOOL dark = TRUE; DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));
    DWORD corner = 2; DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));

    ShowWindow(g_wnd, show); StartServices(); RefreshIngestBox(); RefreshDestList();
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    WSACleanup(); return 0;
}
