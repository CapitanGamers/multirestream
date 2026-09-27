#include "obs_bridge.h"
#include "ws_tiny.h"
#include "../shared/crypto_win.h"
#include "../shared/json_mini.h"

namespace mr {

ObsResult ObsConfigureAndStart(const std::string& host, int port,
                               const std::string& password,
                               const std::string& rtmp_server,
                               const std::string& rtmp_key,
                               bool start) {
    ObsResult r;
    TinyWs ws;
    if (!ws.Connect(host, std::to_string(port), "/")) {
        r.detail = "OBS WebSocket وصل نشد. در OBS: Tools → WebSocket Server Settings را روشن کنید.";
        return r;
    }
    std::string hello;
    if (!ws.RecvText(hello)) {
        r.detail = "پیام Hello از OBS نیامد";
        return r;
    }
    std::string challenge = json::ExtractString(hello, "challenge");
    std::string salt = json::ExtractString(hello, "salt");
    std::string identify = "{\"op\":1,\"d\":{\"rpcVersion\":1";
    if (!challenge.empty() || hello.find("authentication") != std::string::npos) {
        if (password.empty()) {
            r.detail = "OBS پسورد WebSocket می‌خواهد";
            return r;
        }
        std::string secret = Sha256Base64(password + salt);
        std::string auth = Sha256Base64(secret + challenge);
        identify += ",\"authentication\":\"" + auth + "\"";
    }
    identify += "}}";
    ws.SendText(identify);
    std::string ided;
    ws.RecvText(ided);
    if (ided.find("\"op\":2") == std::string::npos && ided.find("Identified") == std::string::npos) {
        // still try requests; some builds omit the word
        if (ided.find("error") != std::string::npos) {
            r.detail = "احراز هویت OBS رد شد";
            return r;
        }
    }

    std::string set =
        std::string("{\"op\":6,\"d\":{\"requestType\":\"SetStreamServiceSettings\",\"requestId\":\"1\",")
        + "\"requestData\":{\"streamServiceType\":\"rtmp_custom\",\"streamServiceSettings\":{"
        + "\"server\":\"" + json::Escape(rtmp_server) + "\","
        + "\"key\":\"" + json::Escape(rtmp_key) + "\"}}}}";
    ws.SendText(set);
    std::string resp;
    ws.RecvText(resp);

    if (start) {
        ws.SendText("{\"op\":6,\"d\":{\"requestType\":\"StartStream\",\"requestId\":\"2\"}}");
        std::string sr;
        ws.RecvText(sr);
    }
    r.ok = true;
    r.detail = start ? "OBS روی اینجست سرور تنظیم شد و استریم شروع شد"
                     : "OBS روی اینجست سرور تنظیم شد (استریم را خودت بزن)";
    return r;
}

} // namespace mr
