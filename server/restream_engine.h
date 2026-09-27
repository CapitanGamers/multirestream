#pragma once
#include "../shared/config.h"
#include "../shared/protocol.h"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
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

class RestreamEngine {
public:
    using LogFn = std::function<void(const std::string&)>;

    explicit RestreamEngine(LogFn log);
    ~RestreamEngine();

    void SetConfig(const AppConfig& cfg);
    AppConfig GetConfig() const;

    bool Start();          // start ingest listener + fan-out
    void Stop();
    bool Running() const { return running_.load(); }
    bool PublisherConnected() const { return publisher_connected_.load(); }

    std::vector<DestRuntime> Destinations() const;
    void SetDestinationEnabled(const std::string& id, bool enabled);
    void UpsertDestination(const Destination& d);

    // Called by supervisor thread / UI timer.
    void Tick();

    std::string BuildFfmpegCommand() const;

private:
    bool SpawnFfmpeg();
    void KillFfmpeg();

    mutable std::mutex mu_;
    AppConfig cfg_;
    LogFn log_;
    std::atomic<bool> running_{false};
    std::atomic<bool> publisher_connected_{false};
    void* process_handle_ = nullptr;   // HANDLE, stored opaque to keep header lean
    void* job_handle_ = nullptr;
    unsigned long process_id_ = 0;
};

} // namespace mr
