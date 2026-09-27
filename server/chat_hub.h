#pragma once
#include "../shared/protocol.h"
#include "../shared/config.h"

#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>

namespace mr {

class ChatHub {
public:
    using ChatFn = std::function<void(const ChatMessage&)>;
    using ViewFn = std::function<void(const ViewerSnapshot&)>;
    using LogFn  = std::function<void(const std::string&)>;

    ChatHub(ChatFn on_chat, ViewFn on_view, LogFn log);
    ~ChatHub();

    void SetConfig(const AppConfig& cfg);
    void Start();
    void Stop();

private:
    void Worker();
    void PollTwitch(const Destination& d);
    void PollYouTube(const Destination& d);
    void PollKick(const Destination& d);
    void TwitchIrcOnce(const Destination& d, std::atomic<bool>& run);

    ChatFn on_chat_;
    ViewFn on_view_;
    LogFn log_;
    mutable std::mutex mu_;
    AppConfig cfg_;
    std::atomic<bool> run_{false};
    std::thread worker_;
    std::thread irc_thread_;
};

} // namespace mr
