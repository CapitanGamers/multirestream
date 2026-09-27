#pragma once
#include "protocol.h"
#include "json_mini.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace mr {

struct AppConfig {
    int control_port = kControlPortDefault;
    int rtmp_port = kRtmpPortDefault;
    std::string ingest_app = "live";
    std::string ingest_key = "stream";
    std::string bind_ip = "0.0.0.0";
    std::string public_host;         // IP/DNS that OBS on the home PC must use
    std::string auth_token;          // clients must present this
    std::string ffmpeg_path = "ffmpeg\\bin\\ffmpeg.exe";
    std::string log_dir = "logs";
    std::vector<Destination> destinations;

    std::string IngestUrlHint(const std::string& public_host) const {
        return "rtmp://" + public_host + ":" + std::to_string(rtmp_port) +
               "/" + ingest_app + "/" + ingest_key;
    }
};

inline std::string DefaultConfigJson() {
    return R"({
  "control_port": 7788,
  "rtmp_port": 1935,
  "ingest_app": "live",
  "ingest_key": "CHANGE_ME_SECRET",
  "bind_ip": "0.0.0.0",
  "public_host": "91.216.104.42",
  "auth_token": "change-this-dashboard-token",
  "ffmpeg_path": "ffmpeg\\\\bin\\\\ffmpeg.exe",
  "destinations": [
    {
      "id": "twitch",
      "platform": "twitch",
      "title": "Twitch",
      "enabled": true,
      "rtmp_url": "rtmp://live.twitch.tv/app/YOUR_TWITCH_STREAM_KEY",
      "channel": "your_twitch_login",
      "oauth_token": "",
      "client_id": ""
    },
    {
      "id": "youtube",
      "platform": "youtube",
      "title": "YouTube",
      "enabled": true,
      "rtmp_url": "rtmp://a.rtmp.youtube.com/live2/YOUR_YOUTUBE_STREAM_KEY",
      "channel": "",
      "api_key": "",
      "oauth_token": ""
    },
    {
      "id": "kick",
      "platform": "kick",
      "title": "Kick",
      "enabled": false,
      "rtmp_url": "rtmps://fa723fc1b171.global-contribute.live-video.net/app/YOUR_KICK_KEY",
      "channel": "your_kick_slug",
      "oauth_token": ""
    }
  ]
}
)";
}

// Extremely small subset parser: finds destination objects by scanning braces.
inline bool LoadConfigFromFile(const std::string& path, AppConfig& out, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string j = ss.str();

    out.control_port = static_cast<int>(json::ExtractInt(j, "control_port", out.control_port));
    out.rtmp_port    = static_cast<int>(json::ExtractInt(j, "rtmp_port", out.rtmp_port));
    {
        auto v = json::ExtractString(j, "ingest_app");  if (!v.empty()) out.ingest_app = v;
        v = json::ExtractString(j, "ingest_key");       if (!v.empty()) out.ingest_key = v;
        v = json::ExtractString(j, "bind_ip");          if (!v.empty()) out.bind_ip = v;
        v = json::ExtractString(j, "public_host");      if (!v.empty()) out.public_host = v;
        v = json::ExtractString(j, "auth_token");       if (!v.empty()) out.auth_token = v;
        v = json::ExtractString(j, "ffmpeg_path");      if (!v.empty()) out.ffmpeg_path = v;
    }

    out.destinations.clear();
    auto dpos = j.find("\"destinations\"");
    if (dpos == std::string::npos) return true;
    auto arr = j.find('[', dpos);
    if (arr == std::string::npos) return true;
    int brace = 0;
    std::size_t obj_start = std::string::npos;
    for (std::size_t i = arr + 1; i < j.size(); ++i) {
        if (j[i] == ']' && brace == 0) break;
        if (j[i] == '{') {
            if (brace == 0) obj_start = i;
            ++brace;
        } else if (j[i] == '}') {
            if (brace > 0) --brace;
            if (brace == 0 && obj_start != std::string::npos) {
                std::string obj = j.substr(obj_start, i - obj_start + 1);
                Destination d;
                d.id         = json::ExtractString(obj, "id");
                d.title      = json::ExtractString(obj, "title");
                d.rtmp_url   = json::ExtractString(obj, "rtmp_url");
                d.channel    = json::ExtractString(obj, "channel");
                d.oauth_token= json::ExtractString(obj, "oauth_token");
                d.client_id  = json::ExtractString(obj, "client_id");
                d.api_key    = json::ExtractString(obj, "api_key");
                d.enabled    = json::ExtractBool(obj, "enabled", true);
                d.platform   = PlatformFromId(json::ExtractString(obj, "platform"));
                if (d.id.empty()) d.id = PlatformId(d.platform);
                if (d.title.empty()) d.title = PlatformTitleFa(d.platform);
                out.destinations.push_back(std::move(d));
                obj_start = std::string::npos;
            }
        }
    }
    return true;
}

inline bool SaveConfigToFile(const std::string& path, const AppConfig& cfg, std::string& err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    out << "{\n";
    out << "  \"control_port\": " << cfg.control_port << ",\n";
    out << "  \"rtmp_port\": " << cfg.rtmp_port << ",\n";
    out << "  \"ingest_app\": \"" << json::Escape(cfg.ingest_app) << "\",\n";
    out << "  \"ingest_key\": \"" << json::Escape(cfg.ingest_key) << "\",\n";
    out << "  \"bind_ip\": \"" << json::Escape(cfg.bind_ip) << "\",\n";
    out << "  \"public_host\": \"" << json::Escape(cfg.public_host) << "\",\n";
    out << "  \"auth_token\": \"" << json::Escape(cfg.auth_token) << "\",\n";
    out << "  \"ffmpeg_path\": \"" << json::Escape(cfg.ffmpeg_path) << "\",\n";
    out << "  \"destinations\": [\n";
    for (std::size_t i = 0; i < cfg.destinations.size(); ++i) {
        const auto& d = cfg.destinations[i];
        out << "    {\n";
        out << "      \"id\": \"" << json::Escape(d.id) << "\",\n";
        out << "      \"platform\": \"" << PlatformId(d.platform) << "\",\n";
        out << "      \"title\": \"" << json::Escape(d.title) << "\",\n";
        out << "      \"enabled\": " << (d.enabled ? "true" : "false") << ",\n";
        out << "      \"rtmp_url\": \"" << json::Escape(d.rtmp_url) << "\",\n";
        out << "      \"channel\": \"" << json::Escape(d.channel) << "\",\n";
        out << "      \"oauth_token\": \"" << json::Escape(d.oauth_token) << "\",\n";
        out << "      \"client_id\": \"" << json::Escape(d.client_id) << "\",\n";
        out << "      \"api_key\": \"" << json::Escape(d.api_key) << "\"\n";
        out << "    }" << (i + 1 < cfg.destinations.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return true;
}

} // namespace mr
