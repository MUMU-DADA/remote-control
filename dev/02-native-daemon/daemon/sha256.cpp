// sha256.cpp — FIPS 180-4 的 SHA-256 实现

#include "sha256.h"

#include <string.h>
#include <unistd.h>

#include <cstdio>

namespace remote_control {
namespace {

// 前 64 个素数立方根小数部分的前 32 位
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() {
    // 前 8 个素数平方根小数部分的前 32 位
    st_[0] = 0x6a09e667; st_[1] = 0xbb67ae85;
    st_[2] = 0x3c6ef372; st_[3] = 0xa54ff53a;
    st_[4] = 0x510e527f; st_[5] = 0x9b05688c;
    st_[6] = 0x1f83d9ab; st_[7] = 0x5be0cd19;
}

void Sha256::Transform(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = st_[0], b = st_[1], c = st_[2], d = st_[3];
    uint32_t e = st_[4], f = st_[5], g = st_[6], h = st_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    st_[0] += a; st_[1] += b; st_[2] += c; st_[3] += d;
    st_[4] += e; st_[5] += f; st_[6] += g; st_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits_ += static_cast<uint64_t>(len) * 8;

    if (bufLen_ > 0) {
        const size_t need = 64 - bufLen_;
        const size_t take = len < need ? len : need;
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            Transform(buf_);
            bufLen_ = 0;
        }
    }
    while (len >= 64) {
        Transform(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(buf_, p, len);
        bufLen_ = len;
    }
}

void Sha256::Final(uint8_t out[32]) {
    const uint64_t bits = bits_;
    // 0x80 之后补零，最后 8 字节是大端的总位数
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (bufLen_ != 56) Update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    }
    // ⚠️ 这两次 Update 会把 bits_ 也加上去，但我们后面不再用它了
    Update(lenBytes, 8);

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>(st_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(st_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(st_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(st_[i]);
    }
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string Sha256Hex(const void* data, size_t len) {
    Sha256 h;
    h.Update(data, len);
    uint8_t d[32];
    h.Final(d);
    return ToHex(d, 32);
}

bool Sha256FileHex(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;

    Sha256 h;
    uint8_t buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        h.Update(buf, n);
    }
    const bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) return false;

    uint8_t d[32];
    h.Final(d);
    if (out != nullptr) *out = ToHex(d, 32);
    return true;
}

const std::string& SelfBuildId() {
    static const std::string id = [] {
        std::string h;
        return Sha256SelfHex(&h) ? h : std::string("unknown");
    }();
    return id;
}

bool Sha256SelfHex(std::string* out) {
    // /proc/self/exe 在设备上是 "…/remote-control (deleted)" 也可能指得到，
    // 但 readlink 出来的路径跟着打开是可靠的。
    char path[512];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return false;
    path[n] = '\0';

    // fexecve(memfd) 后 /proc/self/exe 通常是 "/memfd:<name> (deleted)"，
    // 这个名字无法重新打开。launcher 已在 exec 前校验载荷，使用它传入
    // 的 build id 维持 --ready-file、/info 和 /describe 的版本语义。
    if (strncmp(path, "/memfd:", 7) == 0) {
        const char* expected = getenv("REMOTE_CONTROL_BUILD_ID");
        if (expected == nullptr || strlen(expected) != 64) return false;
        for (size_t i = 0; i < 64; ++i) {
            const unsigned char c = static_cast<unsigned char>(expected[i]);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        if (out != nullptr) {
            *out = expected;
            for (char& c : *out) {
                if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return true;
    }
    return Sha256FileHex(path, out);
}

}  // namespace remote_control
