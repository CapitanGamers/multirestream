#pragma once
#include "../shared/config.h"
#include "../shared/protocol.h"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>

namespace mr {

struct DestRuntime {
    Destination dest;
    bool process_alive = false;
    std::uint32_t pid = 0;
    std::int64_t bytes_out = 0;
    std::string last_error;
};

struct EngineStatus {
    bool running = false;
    bool publisher_connected = false;
    double kbps = 0.0;
    std::int64_t total_bytes = 0;
};

class RestreamEngine {
public:
    using LogFn = std::function<void(const std::string&)>;

    explicit RestreamEngine(LogFn log);
    ~RestreamEngine();

    void SetConfig(const AppConfig& cfg);
    AppConfig GetConfig() const;

    bool Start();
    void Stop();
    bool Running() const { return running_.load(); }
    bool PublisherConnected() const { return publisher_connected_.load(); }
    EngineStatus Status() const;

    std::vector<DestRuntime> Destinations() const;
    void SetDestinationEnabled(const std::string& id, bool enabled);
    void UpsertDestination(const Destination& d);

    // Called by supervisor thread / UI timer.
    void Tick();

    std::string BuildFfmpegCommand() const;

private:
    bool SpawnFfmpeg();
    void KillFfmpeg();
    void PumpLog(void* read_pipe, std::wstring log_path);

    mutable std::mutex mu_;
    AppConfig cfg_;
    LogFn log_;
    std::atomic<bool> running_{false};
    std::atomic<bool> publisher_connected_{false};
    std::atomic<double> kbps_{0.0};
    std::atomic<std::int64_t> total_bytes_{0};
    std::int64_t restart_after_ms_ = 0;
    void* process_handle_ = nullptr;
    void* job_handle_ = nullptr;
    unsigned long process_id_ = 0;
    std::thread pump_thread_;
};

} // namespace mr
