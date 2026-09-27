#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace mr {

inline std::string Base64Encode(const unsigned char* data, DWORD len) {
    DWORD out = 0;
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out))
        return {};
    std::string s(out, '\0');
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &out))
        return {};
    s.resize(out);
    while (!s.empty() && (s.back() == '\0' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

inline std::vector<unsigned char> Sha256Raw(const void* data, DWORD len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> out(32);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, (PUCHAR)data, len, 0);
        BCryptFinishHash(hash, out.data(), (ULONG)out.size(), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

inline std::string Sha256Base64(const std::string& in) {
    auto d = Sha256Raw(in.data(), (DWORD)in.size());
    if (d.empty()) return {};
    return Base64Encode(d.data(), (DWORD)d.size());
}

} // namespace mr
