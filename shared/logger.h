#pragma once
// Thread-safe file + debug logger.
// Files: <log_dir>/<name>-YYYYMMDD.log
// Line:  2026-09-19 19:20:01.123 [ERROR] [ffmpeg] message  (win=2 The system cannot find the file)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdarg>
#include <vector>
#include <cstdio>

namespace mr {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

inline const char* LogLevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

inline std::string WinErrText(DWORD code) {
    if (code == 0) return {};
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::string s;
    if (n && buf) {
        while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) --n;
        int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        s.resize(bytes);
        WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, s.data(), bytes, nullptr, nullptr);
        LocalFree(buf);
    }
    return s;
}

inline std::string RedactSecrets(std::string s) {
    // Hide typical stream-key / token tails after last slash or oauth values.
    const char* keys[] = {"oauth_token", "auth_token", "stream_key", "ingest_key", "PASS "};
    for (auto k : keys) {
        auto p = s.find(k);
        if (p == std::string::npos) continue;
        auto start = s.find_first_of("=: ", p + 1);
        if (start == std::string::npos) continue;
        ++start;
        while (start < s.size() && (s[start] == ' ' || s[start] == '"' || s[start] == ':')) ++start;
        auto end = start;
        while (end < s.size() && s[end] != '"' && s[end] != ',' && s[end] != ' ' && s[end] != '\n') ++end;
        if (end > start + 4)
            s.replace(start, end - start, std::string(4, s[start]) + "…REDACTED");
    }
    // rtmp://host/app/KEY  → keep host/app, hide last path segment if long
    auto r = s.find("rtmp");
    while (r != std::string::npos) {
        auto last = s.find_first_of(" |\"", r);
        if (last == std::string::npos) last = s.size();
        auto slash = s.rfind('/', last);
        auto prev = slash == std::string::npos ? std::string::npos : s.rfind('/', slash - 1);
        if (slash != std::string::npos && slash + 8 < last) {
            s.replace(slash + 1, last - slash - 1, "…REDACTED");
            last = slash + 11;
        }
        r = s.find("rtmp", last);
    }
    return s;
}

class Logger {
public:
    static Logger& I() {
        static Logger x;
        return x;
    }

    void Init(const std::wstring& dir, const std::string& name, LogLevel min_level = LogLevel::Debug) {
        std::lock_guard<std::mutex> g(mu_);
        dir_ = dir;
        name_ = name;
        min_ = min_level;
        CreateDirectoryW(dir.c_str(), nullptr);
        OpenLocked();
        WriteLocked(LogLevel::Info, "logger",
                    "file logger started, level=" + std::string(LogLevelName(min_)), 0);
    }

    void SetLevel(LogLevel l) {
        std::lock_guard<std::mutex> g(mu_);
        min_ = l;
    }

    std::wstring CurrentPath() const {
        std::lock_guard<std::mutex> g(mu_);
        return path_;
    }

    void Log(LogLevel lvl, const char* src, const std::string& msg, DWORD winerr = 0) {
        std::lock_guard<std::mutex> g(mu_);
        if ((int)lvl < (int)min_) return;
        OpenLocked();
        WriteLocked(lvl, src, msg, winerr);
    }

    void Info (const char* s, const std::string& m) { Log(LogLevel::Info,  s, m); }
    void Warn (const char* s, const std::string& m, DWORD e = 0) { Log(LogLevel::Warn,  s, m, e); }
    void Error(const char* s, const std::string& m, DWORD e = 0) { Log(LogLevel::Error, s, m, e); }
    void Debug(const char* s, const std::string& m) { Log(LogLevel::Debug, s, m); }

    std::wstring FfmpegLogPath() const {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t day[16];
        swprintf(day, 16, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
        return dir_ + L"\\ffmpeg-" + day + L".log";
    }

private:
    void OpenLocked() {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t day[16];
        swprintf(day, 16, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
        std::wstring want = dir_ + L"\\" + std::wstring(name_.begin(), name_.end()) + L"-" + day + L".log";
        if (want == path_ && file_.is_open()) return;
        if (file_.is_open()) file_.close();
        path_ = want;
        file_.open(path_, std::ios::app | std::ios::binary);
    }

    void WriteLocked(LogLevel lvl, const char* src, const std::string& msg, DWORD winerr) {
        SYSTEMTIME st; GetLocalTime(&st);
        char ts[40];
        std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::string line;
        line += ts;
        line += " [";
        line += LogLevelName(lvl);
        line += "] [";
        line += src ? src : "-";
        line += "] ";
        line += RedactSecrets(msg);
        if (winerr) {
            line += "  (win=";
            line += std::to_string(winerr);
            auto t = WinErrText(winerr);
            if (!t.empty()) { line += " "; line += t; }
            line += ")";
        }
        line += "\r\n";
        if (file_.is_open()) {
            file_ << line;
            file_.flush();
        }
        OutputDebugStringA(line.c_str());
    }

    mutable std::mutex mu_;
    std::wstring dir_ = L"logs";
    std::string name_ = "app";
    LogLevel min_ = LogLevel::Debug;
    std::wstring path_;
    std::ofstream file_;
};

inline std::wstring ExeDir() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    auto sl = d.find_last_of(L"\\/");
    if (sl != std::wstring::npos) d.resize(sl);
    return d;
}

} // namespace mr
