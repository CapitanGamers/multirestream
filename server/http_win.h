#pragma once
// Minimal WinHTTP client (GET/POST/PATCH/PUT).

#include "../shared/win_util.h"
#include <winhttp.h>
#include <string>

namespace mr {

inline bool HttpRequest(const std::wstring& method,
                        const std::wstring& host,
                        INTERNET_PORT port,
                        bool https,
                        const std::wstring& path,
                        const std::wstring& extra_headers,
                        const std::string& body_in,
                        std::string& body_out,
                        DWORD& status) {
    body_out.clear();
    status = 0;
    HINTERNET ses = WinHttpOpen(L"MultiRestream/1.1",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
    if (!con) { WinHttpCloseHandle(ses); return false; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, method.c_str(), path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false;
    }
    BOOL ok = WinHttpSendRequest(req,
                                 extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str(),
                                 extra_headers.empty() ? 0 : (DWORD)-1,
                                 body_in.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body_in.data(),
                                 (DWORD)body_in.size(), (DWORD)body_in.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            body_out += chunk;
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok == TRUE;
}

inline bool HttpGet(const std::wstring& host, INTERNET_PORT port, bool https,
                    const std::wstring& path, const std::wstring& extra_headers,
                    std::string& body, DWORD& status) {
    return HttpRequest(L"GET", host, port, https, path, extra_headers, {}, body, status);
}

} // namespace mr
