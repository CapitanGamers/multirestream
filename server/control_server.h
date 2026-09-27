#pragma once
#include "../shared/protocol.h"
#include "../shared/config.h"

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>

namespace mr {

struct ClientEvent {
    std::string json_line; // already includes trailing \n
};

class ControlServer {
public:
    using CmdFn = std::function<std::string(const std::string& line)>;
    using LogFn = std::function<void(const std::string&)>;

    ControlServer(int port, const std::string& auth, CmdFn on_cmd, LogFn log);
    ~ControlServer();

    bool Start();
    void Stop();
    void Broadcast(const std::string& json_line);
    int ClientCount() const;

private:
    void AcceptLoop();
    void ClientLoop(unsigned long long sock);

    int port_;
    std::string auth_;
    CmdFn on_cmd_;
    LogFn log_;
    std::atomic<bool> run_{false};
    std::thread accept_thread_;
    unsigned long long listen_sock_ = ~0ull;
    mutable std::mutex mu_;
    std::vector<unsigned long long> clients_;
};

} // namespace mr
