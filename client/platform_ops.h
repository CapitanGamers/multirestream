#pragma once
#include "chrome_bridge.h"
#include "../shared/protocol.h"
#include <string>
#include <vector>

namespace mr {

struct PlatformAccount {
    Platform platform = Platform::Unknown;
    bool logged_in = false;
    std::string login;
    std::string user_id;
    std::string ingest_url;   // rtmp(s)://host/app
    std::string stream_key;
    std::string current_title;
    std::string note;
};

struct TitleResult {
    Platform platform = Platform::Unknown;
    bool ok = false;
    std::string detail;
};

std::vector<PlatformAccount> DiscoverAccounts(const ChromeSession& ses);
std::vector<TitleResult> ApplyTitle(const ChromeSession& ses,
                                    const std::string& title,
                                    const std::vector<PlatformAccount>& accounts);

std::string DestinationRtmp(const PlatformAccount& a);

} // namespace mr
