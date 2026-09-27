#pragma once
#include <string>
#include <vector>
#include <map>

namespace mr {

struct ChromeCookie {
    std::string name;
    std::string value;
    std::string domain;
};

struct ChromeSession {
    bool ok = false;
    std::string error;
    std::vector<ChromeCookie> cookies;
    std::map<std::string, std::string> twitch;   // name -> value
    std::map<std::string, std::string> kick;
    std::map<std::string, std::string> youtube;

    std::string TwitchAuth() const {
        auto it = twitch.find("auth-token");
        return it == twitch.end() ? std::string() : it->second;
    }
    std::string TwitchLogin() const {
        auto it = twitch.find("login");
        if (it != twitch.end()) return it->second;
        it = twitch.find("name");
        return it == twitch.end() ? std::string() : it->second;
    }
    std::string CookieHeader(const std::map<std::string, std::string>& m) const {
        std::string h;
        for (auto& kv : m) {
            if (!h.empty()) h += "; ";
            h += kv.first + "=" + kv.second;
        }
        return h;
    }
};

// Reads cookies from a Chrome instance started with remote debugging
// (see scripts/start-chrome-debug.bat). Cookies NEVER leave this PC
// except the derived stream keys / titles you explicitly push to the VPS.
ChromeSession ReadChromeSession(int debug_port = 9333);

bool LaunchChromeDebug(int debug_port = 9333);

} // namespace mr
