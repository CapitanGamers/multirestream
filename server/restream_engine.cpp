#include "restream_engine.h"
#include "../shared/win_util.h"
#include "../shared/logger.h"

#include <windows.h>
#include <sstream>

namespace mr {

RestreamEngine::RestreamEngine(LogFn log) : log_(std::move(log)) {}

RestreamEngine::~RestreamEngine() {
    Stop();
}

void RestreamEngine::SetConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> g(mu_);
    cfg_ = cfg;
}

AppConfig RestreamEngine::GetConfig() const {
    std::lock_guard<std::mutex> g(mu_);
    return cfg_;
}

std::string RestreamEngine::BuildFfmpegCommand() const {
    std::lock_guard<std::mutex> g(mu_);
    std::ostringstream cmd;
    std::string ff = cfg_.ffmpeg_path;
    if (ff.find(':') == std::string::npos) {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe;
        auto sl = dir.find_last_of("\\/");
        if (sl != std::string::npos) dir.resize(sl + 1);
        ff = dir + ff;
    }
    cmd << "\"" << ff << "\"";
    cmd << " -hide_banner -loglevel info";
    cmd << " -thread_queue_size 512";
    cmd << " -listen 1 -timeout 3600";
    cmd << " -rtmp_live live";
    cmd << " -i \"rtmp://" << cfg_.bind_ip << ":" << cfg_.rtmp_port
        << "/" << cfg_.ingest_app << "/" << cfg_.ingest_key << "\"";
    cmd << " -c copy";

    std::string tee;
    int n = 0;
    for (const auto& d : cfg_.destinations) {
        if (!d.enabled || d.rtmp_url.empty()) continue;
        if (d.rtmp_url.find("YOUR_") != std::string::npos) continue;
        if (n++) tee += "|";
        tee += "[f=flv:onfail=ignore]" + d.rtmp_url;
    }
    if (tee.empty()) {
        cmd << " -f null -";
    } else {
        cmd << " -f tee -map 0 \"" << tee << "\"";
    }
    return cmd.str();
}

bool RestreamEngine::SpawnFfmpeg() {
    KillFfmpeg();
    const std::string cmd8 = BuildFfmpegCommand();
    Logger::I().Info("ffmpeg", "starting pipeline");
    Logger::I().Debug("ffmpeg", cmd8);
    if (log_) log_("FFmpeg pipeline start (جزئیات در logs)");

    std::wstring wcmd = Utf8ToWide(cmd8);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    std::wstring flog = Logger::I().FfmpegLogPath();
    HANDLE logf = CreateFileW(flog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logf == INVALID_HANDLE_VALUE) {
        Logger::I().Warn("ffmpeg", "cannot open ffmpeg log file", GetLastError());
        logf = nullptr;
    } else {
        SetFilePointer(logf, 0, nullptr, FILE_END);
        const char banner[] = "\r\n----- ffmpeg spawn -----\r\n";
        DWORD wr = 0;
        WriteFile(logf, banner, (DWORD)sizeof(banner) - 1, &wr, nullptr);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (logf) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = logf;
        si.hStdError = logf;
    }
    PROCESS_INFORMATION pi{};

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    BOOL ok = CreateProcessW(
        nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);

    if (logf) CloseHandle(logf);

    if (!ok) {
        DWORD e = GetLastError();
        Logger::I().Error("ffmpeg", "CreateProcess failed — is ffmpeg.exe on PATH?", e);
        if (log_) log_("FFmpeg start failed, win=" + std::to_string(e));
        if (job) CloseHandle(job);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);

    process_handle_ = pi.hProcess;
    job_handle_ = job;
    process_id_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
    running_.store(true);
    Logger::I().Info("ffmpeg", "started pid=" + std::to_string(process_id_));
    if (log_) log_("FFmpeg started, pid=" + std::to_string(process_id_));
    return true;
}

void RestreamEngine::KillFfmpeg() {
    if (process_handle_) {
        HANDLE h = static_cast<HANDLE>(process_handle_);
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 4000);
        }
        CloseHandle(h);
        process_handle_ = nullptr;
    }
    if (job_handle_) {
        CloseHandle(static_cast<HANDLE>(job_handle_));
        job_handle_ = nullptr;
    }
    process_id_ = 0;
    running_.store(false);
    publisher_connected_.store(false);
}

bool RestreamEngine::Start() {
    return SpawnFfmpeg();
}

void RestreamEngine::Stop() {
    KillFfmpeg();
    if (log_) log_("Restream stopped.");
}

void RestreamEngine::Tick() {
    if (!process_handle_) return;
    HANDLE h = static_cast<HANDLE>(process_handle_);
    DWORD code = 0;
    if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
        Logger::I().Error("ffmpeg", "process exited, code=" + std::to_string(code) + " — restart in 2s");
        if (log_) log_("FFmpeg exited with code " + std::to_string(code) + " — restarting in 2s");
        KillFfmpeg();
        Sleep(2000);
        SpawnFfmpeg();
    }
}

std::vector<DestRuntime> RestreamEngine::Destinations() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<DestRuntime> out;
    out.reserve(cfg_.destinations.size());
    for (const auto& d : cfg_.destinations) {
        DestRuntime r;
        r.dest = d;
        r.process_alive = running_.load() && d.enabled;
        r.pid = process_id_;
        out.push_back(std::move(r));
    }
    return out;
}

void RestreamEngine::UpsertDestination(const Destination& d) {
    bool restart = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        bool found = false;
        for (auto& x : cfg_.destinations) {
            if (x.id == d.id || (x.platform == d.platform && d.platform != Platform::Custom)) {
                if (!d.rtmp_url.empty() && x.rtmp_url != d.rtmp_url) restart = true;
                if (!d.rtmp_url.empty()) x.rtmp_url = d.rtmp_url;
                if (!d.channel.empty()) x.channel = d.channel;
                if (!d.oauth_token.empty()) x.oauth_token = d.oauth_token;
                x.enabled = d.enabled;
                if (!d.title.empty()) x.title = d.title;
                found = true;
                break;
            }
        }
        if (!found) {
            cfg_.destinations.push_back(d);
            restart = true;
        }
    }
    if (restart && running_.load()) {
        if (log_) log_("Destination updated — restarting pipeline");
        SpawnFfmpeg();
    }
}

void RestreamEngine::SetDestinationEnabled(const std::string& id, bool enabled) {
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& d : cfg_.destinations) {
            if (d.id == id) d.enabled = enabled;
        }
    }
    if (running_.load()) {
        if (log_) log_("Destination '" + id + "' toggled — restarting pipeline");
        SpawnFfmpeg();
    }
}

} // namespace mr
