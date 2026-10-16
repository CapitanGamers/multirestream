#pragma once
// Minimal RFC6455 WebSocket client (text frames only). Good enough for
// Chrome CDP and OBS WebSocket on localhost.

#include "../shared/win_util.h"
#include "../shared/crypto_win.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <cstring>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace mr {

class TinyWs {
public:
    SOCKET s = INVALID_SOCKET;

    bool Connect(const std::string& host, const std::string& port, const std::string& path) {
        Close();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return false;
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
        DWORD to = 8000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
            freeaddrinfo(res); Close(); return false;
        }
        freeaddrinfo(res);

        unsigned char raw[16];
        BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        std::string key = Base64Encode(raw, 16);
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Origin: http://" << host << "\r\n"
            << "\r\n";
        auto hs = req.str();
        if (!SendAll(hs.data(), (int)hs.size())) { Close(); return false; }
        std::string acc;
        char buf[2048];
        while (acc.find("\r\n\r\n") == std::string::npos) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) { Close(); return false; }
            acc.append(buf, n);
            if (acc.size() > 8192) { Close(); return false; }
        }
        return acc.find("101") != std::string::npos;
    }

    bool SendText(const std::string& msg) {
        return SendRawFrame(0x1, msg.data(), msg.size());
    }

    bool RecvText(std::string& out, int max_wait_loops = 40) {
        out.clear();
        char buf[4096];
        for (int i = 0; i < max_wait_loops; ++i) {
            if (TryParse(out)) return true;
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return !out.empty();
            rx_acc_.append(buf, n);
            if (TryParse(out)) return true;
        }
        return !out.empty();
    }

    void Close() {
        if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
        rx_acc_.clear();
    }

    ~TinyWs() { Close(); }

private:
    std::string rx_acc_;

    bool SendAll(const char* data, int len) {
        int sent = 0;
        while (sent < len) {
            int r = send(s, data + sent, len - sent, 0);
            if (r <= 0) return false;
            sent += r;
        }
        return true;
    }

    bool SendRawFrame(uint8_t opcode, const void* data, size_t len) {
        if (s == INVALID_SOCKET) return false;
        std::vector<unsigned char> frame;
        frame.reserve(len + 14);
        frame.push_back(static_cast<unsigned char>(0x80 | (opcode & 0x0F)));

        unsigned char mask[4]{};
        BCryptGenRandom(nullptr, mask, sizeof(mask), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        if (len < 126) {
            frame.push_back(static_cast<unsigned char>(0x80 | len));
        } else if (len <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<unsigned char>(len & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            uint64_t n64 = len;
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<unsigned char>((n64 >> (8 * i)) & 0xFF));
        }
        frame.insert(frame.end(), mask, mask + 4);
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(bytes[i] ^ mask[i & 3]);

        return SendAll(reinterpret_cast<const char*>(frame.data()), (int)frame.size());
    }

    bool TryParse(std::string& out) {
        while (rx_acc_.size() >= 2) {
            const auto* p = reinterpret_cast<const unsigned char*>(rx_acc_.data());
            bool fin    = (p[0] & 0x80) != 0;
            int opcode  = p[0] & 0x0F;
            bool masked = (p[1] & 0x80) != 0;
            uint64_t len = p[1] & 0x7F;
            size_t off = 2;

            if (len == 126) {
                if (rx_acc_.size() < 4) return false;
                len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
                off = 4;
            } else if (len == 127) {
                if (rx_acc_.size() < 10) return false;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
                off = 10;
            }
            unsigned char mask[4]{};
            if (masked) {
                if (rx_acc_.size() < off + 4) return false;
                std::memcpy(mask, p + off, 4);
                off += 4;
            }
            if (rx_acc_.size() < off + static_cast<size_t>(len)) return false;

            std::string payload(static_cast<size_t>(len), '\0');
            for (uint64_t i = 0; i < len; ++i) {
                unsigned char c = p[off + i];
                payload[static_cast<size_t>(i)] = masked ? static_cast<char>(c ^ mask[i & 3]) : static_cast<char>(c);
            }
            rx_acc_.erase(0, off + static_cast<size_t>(len));

            if (opcode == 0x9) {
                SendRawFrame(0xA, payload.data(), payload.size());
                continue;
            }
            if (opcode == 0xA) continue;
            if (opcode == 0x8) return true;
            if (opcode == 0x1 || opcode == 0x0) {
                out += payload;
                if (fin) return true;
            }
        }
        return false;
    }
};

} // namespace mr
