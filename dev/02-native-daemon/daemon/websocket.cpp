// websocket.cpp — RFC 6455 服务端子集

#include "websocket.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <climits>
#include <utility>

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
constexpr auto kFrameReadTimeout = std::chrono::seconds(2);

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

// Once a frame has started, require its header and payload to arrive by one
// absolute deadline.  The first byte is still read without a deadline so an
// otherwise idle WebSocket can remain open indefinitely.
bool ReadFullUntil(int fd, void* buf, size_t n,
                   std::chrono::steady_clock::time_point deadline) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
        pollfd pfd{fd, POLLIN | POLLHUP | POLLERR, 0};
        const int waitMs = remaining > INT_MAX ? INT_MAX :
                           static_cast<int>(remaining);
        int pr;
        do {
            pr = poll(&pfd, 1, waitMs);
        } while (pr < 0 && errno == EINTR);
        if (pr == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (pr < 0) return false;
        const ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        got += static_cast<size_t>(r);
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
    out->clear();
    if (in.size() % 4 != 0) return false;

    // Function-local static initialization is synchronized by the language;
    // unlike the old mutable `inited` flag, concurrent handshakes are safe.
    static const std::array<int8_t, 256> rev = [] {
        std::array<int8_t, 256> table{};
        table.fill(-1);
        for (int i = 0; i < 64; ++i) {
            table[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
        }
        return table;
    }();

    std::string decoded;
    decoded.reserve((in.size() / 4) * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        const char c0 = in[i];
        const char c1 = in[i + 1];
        const char c2 = in[i + 2];
        const char c3 = in[i + 3];
        const int8_t v0 = rev[static_cast<uint8_t>(c0)];
        const int8_t v1 = rev[static_cast<uint8_t>(c1)];
        if (v0 < 0 || v1 < 0) return false;

        if (c2 == '=') {
            // One output byte: the low four bits of the second sextet are
            // padding and must be zero; the final character must also pad.
            if (c3 != '=' || (v1 & 0x0F) != 0 || i + 4 != in.size()) return false;
            decoded.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            continue;
        }
        const int8_t v2 = rev[static_cast<uint8_t>(c2)];
        if (v2 < 0) return false;
        decoded.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        decoded.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));

        if (c3 == '=') {
            // Two output bytes: the low two bits of the third sextet must be
            // zero and padding is only valid in the final quartet.
            if ((v2 & 0x03) != 0 || i + 4 != in.size()) return false;
            continue;
        }
        const int8_t v3 = rev[static_cast<uint8_t>(c3)];
        if (v3 < 0) return false;
        decoded.push_back(static_cast<char>((v2 << 6) | v3));
    }
    *out = std::move(decoded);
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
    {
        uint8_t hdr[2];
        if (!ReadFull(fd, hdr, 1)) {
            if (error) error->clear();     // 对端正常关闭，不算错误
            return false;
        }
        const auto frameDeadline = std::chrono::steady_clock::now() +
                                   kFrameReadTimeout;
        if (!ReadFullUntil(fd, hdr + 1, 1, frameDeadline)) {
            if (error) *error = "读帧头失败";
            return false;
        }

        out->fin    = (hdr[0] & 0x80) != 0;
        out->opcode = hdr[0] & 0x0F;
        const bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;

        if ((hdr[0] & 0x70) != 0) {
            if (error) *error = "不支持 RSV1/RSV2/RSV3 扩展位";
            return false;
        }
        if (out->opcode != kWsText && out->opcode != kWsBinary &&
            out->opcode != kWsClose && out->opcode != kWsPing &&
            out->opcode != kWsPong) {
            if (error) *error = "未知或不支持的操作码";
            return false;
        }

        if (len == 126) {
            uint8_t ext[2];
            if (!ReadFullUntil(fd, ext, 2, frameDeadline)) {
                if (error) *error = "读长度失败";
                return false;
            }
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!ReadFullUntil(fd, ext, 8, frameDeadline)) {
                if (error) *error = "读长度失败";
                return false;
            }
            if ((ext[0] & 0x80) != 0) {
                if (error) *error = "帧长度不是合法的 63 位无符号数";
                return false;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }

        const bool controlFrame = (out->opcode & 0x08) != 0;
        if (controlFrame && !out->fin) {
            if (error) *error = "控制帧不能分片";
            return false;
        }
        if (controlFrame && len > 125) {
            if (error) *error = "控制帧载荷不能超过 125 字节";
            return false;
        }
        if (!out->fin) {
            if (error) *error = "不支持分片帧";
            return false;
        }
        if (out->opcode == kWsClose && len == 1) {
            if (error) *error = "close 帧载荷不能只有 1 字节";
            return false;
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
        if (!ReadFullUntil(fd, mask, 4, frameDeadline)) {
            if (error) *error = "读掩码失败";
            return false;
        }

        out->payload.assign(static_cast<size_t>(len), '\0');
        if (len > 0 && !ReadFullUntil(fd, &out->payload[0],
                                      static_cast<size_t>(len), frameDeadline)) {
            if (error) *error = "读载荷失败";
            return false;
        }
        for (size_t i = 0; i < out->payload.size(); ++i) {
            out->payload[i] = static_cast<char>(out->payload[i] ^ mask[i % 4]);
        }

        // 控制帧就地处理
        if (out->opcode == kWsPing) {
            // A peer that cannot receive its pong is already unusable. Keep
            // ping handling bounded so it cannot pin the connection thread.
            if (!WsWriteFrameWithTimeout(fd, kWsPong, out->payload, 1000)) {
                if (error) *error = "回应 ping 超时";
                return false;
            }
            return true;
        }
        if (out->opcode == kWsPong) {
            return true;
        }
        if (out->opcode == kWsClose) {
            if (!WsWriteFrameWithTimeout(fd, kWsClose, out->payload, 1000)) {
                if (error) *error = "回应 close 帧失败";
                return false;
            }
            if (error) error->clear();
            return false;
        }
        return true;
    }
}

bool SendBuffersWithTimeout(int fd, std::initializer_list<SocketBuffer> buffers,
                            int timeoutMs) {
    iovec vectors[8];
    if (buffers.size() > sizeof(vectors) / sizeof(vectors[0]) || timeoutMs < 0) {
        errno = EINVAL;
        return false;
    }
    size_t count = 0;
    for (const auto& buffer : buffers) {
        if (buffer.size == 0) continue;
        vectors[count++] = {const_cast<void*>(buffer.data), buffer.size};
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    size_t first = 0;
    while (first < count) {
        if (timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        msghdr message{};
        message.msg_iov = vectors + first;
        message.msg_iovlen = count - first;
        const ssize_t written = sendmsg(fd, &message,
                MSG_NOSIGNAL | (timeoutMs > 0 ? MSG_DONTWAIT : 0));
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            if (written < 0 && timeoutMs > 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const auto remaining = std::chrono::duration_cast<
                        std::chrono::milliseconds>(deadline -
                        std::chrono::steady_clock::now()).count();
                if (remaining <= 0) {
                    errno = ETIMEDOUT;
                    return false;
                }
                pollfd pfd{fd, POLLOUT | POLLHUP | POLLERR, 0};
                const int waitMs = remaining >= INT_MAX ? INT_MAX :
                                   static_cast<int>(remaining + 1);
                const int ready = poll(&pfd, 1, waitMs);
                if (ready > 0 || (ready < 0 && errno == EINTR)) continue;
                if (ready == 0) errno = ETIMEDOUT;
            }
            return false;
        }
        size_t remaining = static_cast<size_t>(written);
        while (first < count && remaining >= vectors[first].iov_len) {
            remaining -= vectors[first].iov_len;
            ++first;
        }
        if (first < count && remaining != 0) {
            vectors[first].iov_base = static_cast<char*>(vectors[first].iov_base) +
                                      remaining;
            vectors[first].iov_len -= remaining;
        }
    }
    return true;
}

bool WsWriteFrameWithTimeout(int fd, uint8_t opcode, const std::string& payload,
                            int timeoutMs) {
    uint8_t header[10];
    size_t headerSize = 0;
    header[headerSize++] = static_cast<uint8_t>(0x80 | opcode);
    const size_t n = payload.size();
    // 服务端发出的帧**不打掩码**（RFC 6455 5.1）
    if (n < 126) {
        header[headerSize++] = static_cast<uint8_t>(n);
    } else if (n <= 0xFFFF) {
        header[headerSize++] = 126;
        header[headerSize++] = static_cast<uint8_t>(n >> 8);
        header[headerSize++] = static_cast<uint8_t>(n);
    } else {
        header[headerSize++] = 127;
        for (int i = 7; i >= 0; --i) {
            header[headerSize++] = static_cast<uint8_t>(
                    static_cast<uint64_t>(n) >> (i * 8));
        }
    }

    // Send the small header and existing encoded buffer in one syscall.
    // A short write can end inside either iovec, so advance both explicitly.
    return SendBuffersWithTimeout(fd, {{header, headerSize},
                                      {payload.data(), n}}, timeoutMs);
}

bool WsWriteFrame(int fd, uint8_t opcode, const std::string& payload) {
    return WsWriteFrameWithTimeout(fd, opcode, payload, 0);
}

bool WsWriteText(int fd, const std::string& text) {
    return WsWriteFrame(fd, kWsText, text);
}

}  // namespace remote_control
