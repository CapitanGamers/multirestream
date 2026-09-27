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

        // Random-ish key
        unsigned char raw[16];
        for (int i = 0; i < 16; ++i) raw[i] = (unsigned char)(GetTickCount() * 17 + i * 31);
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
        if (send(s, hs.data(), (int)hs.size(), 0) <= 0) { Close(); return false; }
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
        if (s == INVALID_SOCKET) return false;
        std::vector<unsigned char> frame;
        frame.push_back(0x81);
        uint64_t n = msg.size();
        unsigned char mask[4] = { 0xA5, 0x5A, 0x3C, 0xC3 };
        if (n < 126) {
            frame.push_back((unsigned char)(0x80 | n));
        } else if (n <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back((unsigned char)(n >> 8));
            frame.push_back((unsigned char)(n));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) frame.push_back((unsigned char)(n >> (8 * i)));
        }
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < msg.size(); ++i)
            frame.push_back((unsigned char)msg[i] ^ mask[i & 3]);
        return send(s, (char*)frame.data(), (int)frame.size(), 0) == (int)frame.size();
    }

    bool RecvText(std::string& out, int max_wait_loops = 40) {
        out.clear();
        std::string acc;
        char buf[4096];
        for (int i = 0; i < max_wait_loops; ++i) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return !out.empty();
            acc.append(buf, n);
            if (TryParse(acc, out)) return true;
        }
        return !out.empty();
    }

    void Close() {
        if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
    }

    ~TinyWs() { Close(); }

private:
    static bool TryParse(std::string& acc, std::string& out) {
        if (acc.size() < 2) return false;
        auto* p = (const unsigned char*)acc.data();
        bool fin = (p[0] & 0x80) != 0;
        int opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t off = 2;
        if (len == 126) {
            if (acc.size() < 4) return false;
            len = (p[2] << 8) | p[3];
            off = 4;
        } else if (len == 127) {
            if (acc.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            off = 10;
        }
        unsigned char mask[4]{};
        if (masked) {
            if (acc.size() < off + 4) return false;
            memcpy(mask, p + off, 4);
            off += 4;
        }
        if (acc.size() < off + (size_t)len) return false;
        std::string payload(len, '\0');
        for (uint64_t i = 0; i < len; ++i) {
            unsigned char c = p[off + i];
            payload[(size_t)i] = masked ? (char)(c ^ mask[i & 3]) : (char)c;
        }
        acc.erase(0, off + (size_t)len);
        if (opcode == 0x1 || opcode == 0x0) { out += payload; return fin; }
        if (opcode == 0x8) return true;
        if (opcode == 0x9) return false; // ping ignored
        return false;
    }
};

} // namespace mr
