#pragma once
#include <string>

namespace mr {

struct ObsResult {
    bool ok = false;
    std::string detail;
};

// OBS 28+ websocket v5. Default port 4455.
ObsResult ObsConfigureAndStart(const std::string& host,
                               int port,
                               const std::string& password,
                               const std::string& rtmp_server, // rtmp://vps:1935/live
                               const std::string& rtmp_key,
                               bool start);

} // namespace mr
