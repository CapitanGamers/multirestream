#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace mr {

inline constexpr int kControlPortDefault = 7788;
inline constexpr int kRtmpPortDefault = 1935;
inline constexpr const char* kProtocolVersion = "1.0";

enum class Platform { Unknown = 0, Twitch, YouTube, Kick, Facebook, TikTok, Custom };

inline const char* PlatformId(Platform p) {
    switch (p) {
        case Platform::Twitch:   return "twitch";
        case Platform::YouTube:  return "youtube";
        case Platform::Kick:     return "kick";
        case Platform::Facebook: return "facebook";
        case Platform::TikTok:   return "tiktok";
        case Platform::Custom:   return "custom";
        default:                 return "unknown";
    }
}

inline const char* PlatformTitleFa(Platform p) {
    switch (p) {
        case Platform::Twitch:   return "توییچ";
        case Platform::YouTube:  return "یوتیوب";
        case Platform::Kick:     return "کیک";
        case Platform::Facebook: return "فیسبوک";
        case Platform::TikTok:   return "تیک‌تاک";
        case Platform::Custom:   return "سفارشی";
        default:                 return "نامشخص";
    }
}

inline Platform PlatformFromId(const std::string& id) {
    if (id == "twitch")  return Platform::Twitch;
    if (id == "youtube") return Platform::YouTube;
    if (id == "kick")    return Platform::Kick;
    if (id == "facebook")return Platform::Facebook;
    if (id == "tiktok")  return Platform::TikTok;
    if (id == "custom")  return Platform::Custom;
    return Platform::Unknown;
}

// Helper to convert "#RRGGBB" from server to COLORREF
inline COLORREF ParseHexColor(const std::string& hex, COLORREF def = RGB(160,160,160)) {
    if (hex.size() == 7 && hex[0] == '#') {
        int r, g, b;
        if (sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3)
            return RGB(r, g, b);
    }
    return def;
}

struct ChatMessage {
    Platform platform = Platform::Unknown;
    std::string user;
    std::string text;
    std::int64_t ts_ms = 0;
    bool is_mod = false;
    bool is_sub = false;
    std::string logo_text; // آیکون/متن تولید شده در سرور
    std::string color_hex; // رنگ تولید شده در سرور
};

struct ViewerSnapshot {
    Platform platform = Platform::Unknown;
    std::string name;
    int viewers = -1;
    bool live = false;
    bool dest_connected = false;
    std::string status;
};

struct Destination {
    std::string id;
    Platform platform = Platform::Custom;
    std::string title;
    std::string rtmp_url;
    bool enabled = true;
    std::string channel;
    std::string oauth_token;
    std::string client_id;
    std::string api_key;
};

} // namespace mr
