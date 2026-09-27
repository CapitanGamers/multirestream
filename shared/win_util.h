#pragma once
// Windows helpers shared by server and client. UNICODE / UTF-16 throughout.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace mr {

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::int64_t NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart / 10000ULL - 11644473600000ULL);
}

inline std::wstring FormatViewers(int v) {
    if (v < 0) return L"—";
    wchar_t buf[32];
    if (v >= 1000000) {
        swprintf(buf, 32, L"%.1fM", v / 1000000.0);
    } else if (v >= 10000) {
        swprintf(buf, 32, L"%.1fK", v / 1000.0);
    } else {
        swprintf(buf, 32, L"%d", v);
    }
    return buf;
}

inline HFONT MakeFont(int pt, bool bold = false) {
    HDC hdc = GetDC(nullptr);
    int px = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    return CreateFontW(px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

inline void SetWindowFont(HWND h, HFONT f) {
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

inline bool InitWinsock() {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
}

inline COLORREF PlatformColor(int platform) {
    switch (platform) {
        case 1: return RGB(145, 70, 255);   // Twitch
        case 2: return RGB(255,   0,   0);  // YouTube
        case 3: return RGB( 83, 255,  26);  // Kick
        case 4: return RGB( 24, 119, 242);  // Facebook
        case 5: return RGB( 16, 16, 16);    // TikTok
        default: return RGB(160, 160, 160);
    }
}

inline const wchar_t* PlatformGlyph(int platform) {
    // Short badge text drawn inside the colored circle — works without image files.
    switch (platform) {
        case 1: return L"TW";
        case 2: return L"YT";
        case 3: return L"KK";
        case 4: return L"FB";
        case 5: return L"TT";
        default: return L"?";
    }
}

} // namespace mr
