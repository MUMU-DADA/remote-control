// websocket.cpp — RFC 6455 服务端子集

#include "websocket.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <vector>

#include "remote_control_log.h"

namespace remote_control {
namespace {

// ── SHA-1 ───────────────────────────────────────────────────────────────────
//
// 自己实现而不是找库：只为了握手算一次摘要，为它引入依赖不划算，
// 而且 SHA-1 的参考实现总共也就几十行。
// （SHA-1 在这里**不用于安全** —— RFC 6455 的握手只是防止
//   "普通 HTTP 请求被误当成 WebSocket"，不是密码学用途。）
struct Sha1Ctx {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t len = 0;
    uint8_t  buf[64] = {};
    size_t   bufLen = 0;

    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            const uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void Update(const uint8_t* p, size_t n) {
        len += n;
        while (n > 0) {
            const size_t take = (n < 64 - bufLen) ? n : (64 - bufLen);
            memcpy(buf + bufLen, p, take);
            bufLen += take;
            p += take;
            n -= take;
            if (bufLen == 64) { Block(buf); bufLen = 0; }
        }
    }

    void Final(uint8_t out[20]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (bufLen != 56) Update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
        }
        Update(lenBytes, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
    }
};

const char kBase64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 从 fd 精确读 n 字节（处理短读）
bool ReadFull(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return false;                       // 对端关闭
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool WriteFull(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < n) {
        const ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

// ── SHA-1 / base64 ──────────────────────────────────────────────────────────
std::string Sha1(const std::string& data) {
    Sha1Ctx ctx;
    ctx.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint8_t digest[20];
    ctx.Final(digest);
    return std::string(reinterpret_cast<char*>(digest), 20);
}

std::string Base64Encode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                           (static_cast<uint8_t>(data[i + 1]) << 8) |
                           static_cast<uint8_t>(data[i + 2]);
        out += kBase64Chars[(v >> 18) & 63];
        out += kBase64Chars[(v >> 12) & 63];
        out += kBase64Chars[(v >> 6) & 63];
        out += kBase64Chars[v & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out += kBase64Chars[(v >> 18) & 63];
        out += kBase64Chars[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                           (static_cast<uint8_t>(data[i + 1]) << 8);
        out += kBase64Chars[(v >> 18) & 63];
        out += kBase64Chars[(v >> 12) & 63];
        out += kBase64Chars[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& in, std::string* out) {
    // 反向表：-1 表示非法字符
    static int8_t rev[256];
    static bool inited = false;
    if (!inited) {
        memset(rev, -1, sizeof(rev));
        for (int i = 0; i < 64; ++i) {
            rev[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
        }
        inited = true;
    }

    out->clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int8_t v = rev[static_cast<uint8_t>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// ── 握手 ────────────────────────────────────────────────────────────────────
bool WsComputeAccept(const std::string& key, std::string* acceptOut) {
    if (key.empty()) return false;

    // key 必须是 base64(16 字节)，也就是 24 个字符。
    // 校验它不只是形式主义：不校验的话任何带 Upgrade 头的请求
    // 都会被当成 WebSocket，而客户端那边会因为 accept 不匹配而挂住。
    std::string raw;
    if (!Base64Decode(key, &raw) || raw.size() != 16) {
        ALOGW("WebSocket 握手：Sec-WebSocket-Key 不是合法的 16 字节 base64");
        return false;
    }

    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    *acceptOut = Base64Encode(Sha1(key + kGuid));
    return true;
}

// ── 帧读写 ──────────────────────────────────────────────────────────────────
bool WsReadFrame(int fd, WsFrame* out, std::string* error) {
    for (;;) {   // ping 会被自动回应，然后继续等真正的数据帧
        uint8_t hdr[2];
        if (!ReadFull(fd, hdr, 2)) {
            if (error) error->clear();     // 对端正常关闭，不算错误
            return false;
        }

        out->fin    = (hdr[0] & 0x80) != 0;
        out->opcode = hdr[0] & 0x0F;
        const bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!ReadFull(fd, ext, 2)) { if (error) *error = "读长度失败"; return false; }
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!ReadFull(fd, ext, 8)) { if (error) *error = "读长度失败"; return false; }
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }

        // 上限保护：不设的话一个恶意长度就能让我们尝试分配几个 GB。
        // 我们的消息都是几十字节，1 MB 已经宽松得离谱。
        constexpr uint64_t kMaxPayload = 1u << 20;
        if (len > kMaxPayload) {
            if (error) *error = "帧过大: " + std::to_string(len);
            return false;
        }

        // RFC 6455：客户端发来的帧**必须**打掩码。
        // 不打掩码是协议错误 —— 明确的错误比容忍更安全。
        if (!masked) {
            if (error) *error = "客户端帧没有掩码（RFC 6455 要求必须掩码）";
            return false;
        }

        uint8_t mask[4];
        if (!ReadFull(fd, mask, 4)) { if (error) *error = "读掩码失败"; return false; }

        out->payload.assign(static_cast<size_t>(len), '\0');
        if (len > 0 && !ReadFull(fd, &out->payload[0], static_cast<size_t>(len))) {
            if (error) *error = "读载荷失败";
            return false;
        }
        for (size_t i = 0; i < out->payload.size(); ++i) {
            out->payload[i] = static_cast<char>(out->payload[i] ^ mask[i % 4]);
        }

        // 控制帧就地处理
        if (out->opcode == kWsPing) {
            WsWriteFrame(fd, kWsPong, out->payload);
            continue;
        }
        if (out->opcode == kWsPong) {
            continue;   // 我们不发 ping，忽略对端的 pong
        }
        if (out->opcode == kWsClose) {
            if (error) error->clear();
            return false;
        }
        // 分片：我们不做续帧。消息都很小，客户端也不会分片；
        // 真收到就明确拒绝，而不是拼一个半截的消息。
        if (!out->fin) {
            if (error) *error = "不支持分片帧";
            return false;
        }
        return true;
    }
}

bool WsWriteFrame(int fd, uint8_t opcode, const std::string& payload) {
    std::vector<uint8_t> buf;
    buf.reserve(payload.size() + 10);
    buf.push_back(static_cast<uint8_t>(0x80 | opcode));   // FIN + opcode

    const size_t n = payload.size();
    // 服务端发出的帧**不打掩码**（RFC 6455 5.1）
    if (n < 126) {
        buf.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        buf.push_back(126);
        buf.push_back(static_cast<uint8_t>(n >> 8));
        buf.push_back(static_cast<uint8_t>(n));
    } else {
        buf.push_back(127);
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (i * 8)));
        }
    }
    buf.insert(buf.end(), payload.begin(), payload.end());
    return WriteFull(fd, buf.data(), buf.size());
}

bool WsWriteText(int fd, const std::string& text) {
    return WsWriteFrame(fd, kWsText, text);
}

}  // namespace remote_control
