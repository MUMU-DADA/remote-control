// test_websocket.cpp — WebSocket 握手测试
//
// 为什么这个必须离线测：握手算错的表现是**浏览器直接拒绝连接**，
// 页面上什么都不显示，控制台里也只有一句 "WebSocket connection failed"。
// 没有可读的错误信息，只能靠对着 RFC 的标准向量验证。

#include "websocket.h"
#include "test_util.h"

#include <cstdio>
#include <chrono>
#include <cerrno>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <vector>

using namespace remote_control;
using namespace remote_control_test;

namespace {

std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (char c : s) {
        o += d[(static_cast<uint8_t>(c) >> 4) & 15];
        o += d[static_cast<uint8_t>(c) & 15];
    }
    return o;
}

void TestSha1() {
    printf("\n\033[1;34m[1] SHA-1\033[0m\n");
    Check(Hex(Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d",
          "SHA1(\"abc\")");
    Check(Hex(Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
          "SHA1(\"\")");
    Check(Hex(Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
                  "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
          "SHA1(56 字节长串)");
    // 64 字节是 SHA-1 的分块大小，恰好一整块是最容易写错 padding 的地方
    Check(Hex(Sha1(std::string(64, 'a'))) ==
                  "0098ba824b5c16427bd7a1122a5a442a25ec644d",
          "SHA1(64×'a') —— 恰好一块，padding 边界");
    Check(Hex(Sha1(std::string(1000000, 'a'))) ==
                  "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
          "SHA1(100 万×'a') —— 多块累积");
}

void TestBase64() {
    printf("\n\033[1;34m[2] base64\033[0m\n");
    Check(Base64Encode("") == "", "空串");
    Check(Base64Encode("f") == "Zg==", "1 字节（补两个 =）");
    Check(Base64Encode("fo") == "Zm8=", "2 字节（补一个 =）");
    Check(Base64Encode("foo") == "Zm9v", "3 字节（不补）");
    Check(Base64Encode("foobar") == "Zm9vYmFy", "6 字节");

    std::string raw;
    Check(Base64Decode("Zm9vYmFy", &raw) && raw == "foobar", "解码往返");
    Check(!Base64Decode("!!!非法!!!", &raw), "非法字符被拒");
    Check(!Base64Decode("Zg=", &raw), "长度不是 4 的倍数被拒");
    Check(!Base64Decode("Zh==", &raw), "非零 padding bit 被拒");
    Check(!Base64Decode("Zm=9", &raw), "padding 出现在中间被拒");
    // 二进制往返（SHA-1 摘要是二进制，会含 0x00 和高位字节）
    const std::string digest = Sha1("abc");
    std::string back;
    Check(Base64Decode(Base64Encode(digest), &back) && back == digest,
          "二进制摘要往返");
}

void TestHandshake() {
    printf("\n\033[1;34m[3] 握手\033[0m\n");

    // RFC 6455 §1.3 给的标准例子，一字不差
    std::string accept;
    const bool ok = WsComputeAccept("dGhlIHNhbXBsZSBub25jZQ==", &accept);
    Check(ok && accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
          "RFC 6455 标准向量: dGhlIHNhbXBsZSBub25jZQ== → s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    // 不合法输入必须被拒。不校验的话任何带 Upgrade 头的请求都会被
    // 当成 WebSocket，而客户端会因为 accept 不匹配而挂住。
    Check(!WsComputeAccept("", &accept), "空 key 被拒");
    Check(!WsComputeAccept("not-base64!!!", &accept), "非法 base64 被拒");
    Check(!WsComputeAccept("Zm9v", &accept), "长度不对被拒（要 16 字节）");
    // "YWJjZGVmZ2hpamtsbW5v" 解出来是 15 字节（不是 16）。
    // 注意别用 "…bW5vcA==" —— 那个恰好解出 16 字节，是合法的，
    // 写测试时我自己先搞错过一次。
    Check(!WsComputeAccept("YWJjZGVmZ2hpamtsbW5v", &accept),
          "解出 15 字节（不足 16）—— 被拒");
}

void TestCloseHandshake() {
    printf("\n\033[1;34m[4] close 握手\033[0m\n");
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // 客户端 close 帧（code 1000 + reason "bye"），按 RFC 6455 掩码。
    const uint8_t maskedClose[] = {0x88, 0x85, 1, 2, 3, 4,
                                   2, 234, 97, 125, 100};
    const ssize_t sent = write(fds[0], maskedClose, sizeof(maskedClose));
    WsFrame frame;
    std::string error;
    const bool gotFrame = WsReadFrame(fds[1], &frame, &error);

    uint8_t response[7]{};
    size_t received = 0;
    while (received < sizeof(response)) {
        const ssize_t n = read(fds[0], response + received,
                               sizeof(response) - received);
        if (n <= 0) break;
        received += static_cast<size_t>(n);
    }
    Check(sent == static_cast<ssize_t>(sizeof(maskedClose)),
          "发送客户端 close 帧");
    Check(!gotFrame && error.empty(), "close 帧正常结束读取");
    Check(received == sizeof(response) && response[0] == 0x88 &&
                  response[1] == 5 && response[2] == 3 && response[3] == 232 &&
                  response[4] == 'b' && response[5] == 'y' && response[6] == 'e',
          "服务端返回相同 close code 和 reason");
    close(fds[0]);
    close(fds[1]);
}

void TestInvalidCloseFrames() {
    printf("\n\033[1;34m[5] 非法 close 控制帧\033[0m\n");
    const std::vector<std::vector<uint8_t>> frames = {
        {0x08, 0x80, 1, 2, 3, 4},
        {0x88, 0x81, 1, 2, 3, 4, 1},
        {0x88, 0xFE, 1, 2, 3, 4, 0, 126},
    };
    const char* labels[] = {"拒绝分片 close 帧", "拒绝 1 字节 close 载荷",
                            "拒绝超过 125 字节的 close 载荷"};
    for (size_t i = 0; i < frames.size(); ++i) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            Check(false, "创建 socketpair");
            return;
        }
        const ssize_t sent = write(fds[0], frames[i].data(), frames[i].size());
        WsFrame frame;
        std::string error;
        const bool accepted = WsReadFrame(fds[1], &frame, &error);
        Check(sent == static_cast<ssize_t>(frames[i].size()) && !accepted &&
                      !error.empty(),
              "%s", labels[i]);
        close(fds[0]);
        close(fds[1]);
    }
}

void TestInvalidFrameHeaders() {
    printf("\n\033[1;34m[6] 非法帧头\033[0m\n");
    const std::vector<std::vector<uint8_t>> frames = {
        {0xC1, 0x80, 1, 2, 3, 4},                 // RSV1
        {0x80, 0x80, 1, 2, 3, 4},                 // continuation
        {0x8B, 0x80, 1, 2, 3, 4},                 // unknown opcode
        {0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, // 64-bit length high bit
         0, 1},
    };
    const char* labels[] = {"拒绝 RSV 扩展位", "拒绝 continuation 帧",
                            "拒绝未知操作码", "拒绝非法 63 位长度"};
    for (size_t i = 0; i < frames.size(); ++i) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            Check(false, "创建 socketpair");
            return;
        }
        const ssize_t sent = write(fds[0], frames[i].data(), frames[i].size());
        WsFrame frame;
        std::string error;
        const bool accepted = WsReadFrame(fds[1], &frame, &error);
        Check(sent == static_cast<ssize_t>(frames[i].size()) && !accepted &&
                      !error.empty(),
              "%s", labels[i]);
        close(fds[0]);
        close(fds[1]);
    }
}

void TestScatterGatherWrite() {
    printf("\n\033[1;34m[7] scatter/gather 帧写入\033[0m\n");
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    const std::string payload(200, 'x');
    Check(WsWriteFrameWithTimeout(fds[0], kWsBinary, payload, 1000),
          "不复制载荷写入 200 字节二进制帧");
    uint8_t header[4]{};
    size_t got = 0;
    while (got < sizeof(header)) {
        const ssize_t n = read(fds[1], header + got, sizeof(header) - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    Check(got == sizeof(header) && header[0] == 0x82 && header[1] == 126 &&
                  header[2] == 0 && header[3] == 200,
          "帧头长度编码正确");
    std::string received(payload.size(), '\0');
    got = 0;
    while (got < received.size()) {
        const ssize_t n = read(fds[1], &received[got], received.size() - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    Check(got == payload.size() && received == payload,
          "短读场景完整保留载荷");
    close(fds[0]);
    close(fds[1]);
}

void TestWriteDeadline() {
    printf("\n\033[1;34m[8] 慢客户端写超时\033[0m\n");
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    int sndBuf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
    const std::string payload(1u << 20, 'y');
    const auto started = std::chrono::steady_clock::now();
    const bool sent = WsWriteFrameWithTimeout(fds[0], kWsBinary, payload, 50);
    const int sendError = errno;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    Check(!sent && sendError == ETIMEDOUT && elapsed >= 25 && elapsed < 1000,
          "不读的客户端在绝对 deadline 内超时（%lld ms）",
          static_cast<long long>(elapsed));
    close(fds[0]);
    close(fds[1]);
}

bool ReadExact(int fd, void* output, size_t size) {
    char* bytes = static_cast<char*>(output);
    size_t received = 0;
    while (received < size) {
        const ssize_t count = read(fd, bytes + received, size - received);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return true;
}

void TestFrameLengthBoundaries() {
    printf("\n\033[1;34m[9] 二进制帧长度边界\033[0m\n");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    timeval timeout{1, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const size_t lengths[] = {0, 125, 126, 65535, 65536};
    for (const size_t length : lengths) {
        std::string payload(length, '\0');
        for (size_t i = 0; i < length; ++i) payload[i] = static_cast<char>(i);
        bool valid = WsWriteFrameWithTimeout(fds[0], kWsBinary, payload, 1000);
        uint8_t header[2]{};
        valid = valid && ReadExact(fds[1], header, sizeof(header));
        uint64_t wireLength = header[1];
        if (wireLength == 126 || wireLength == 127) {
            const size_t width = wireLength == 126 ? 2 : 8;
            uint8_t extended[8]{};
            valid = valid && ReadExact(fds[1], extended, width);
            wireLength = 0;
            for (size_t i = 0; i < width; ++i) {
                wireLength = wireLength * 256 + extended[i];
            }
        }
        std::string received(length, '\0');
        valid = valid && ReadExact(fds[1], received.data(), received.size());
        Check(valid && header[0] == 0x82 && !(header[1] & 0x80) &&
                      wireLength == length && received == payload,
              "%zu 字节帧的长度和二进制载荷保持完整", length);
    }
    close(fds[0]);
    close(fds[1]);
}

void TestScatterGatherShortWrites() {
    printf("\n\033[1;34m[10] scatter/gather 短写\033[0m\n");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    int sndBuf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
    const std::string header = "multipart header\r\n\r\n";
    const std::string payload(1u << 20, 'z');
    const std::string footer = "\r\n--boundary\r\n";
    const std::string expected = header + payload + footer;
    const pid_t reader = fork();
    if (reader == 0) {
        close(fds[0]);
        std::string received(expected.size(), '\0');
        const bool valid = ReadExact(fds[1], received.data(), received.size());
        close(fds[1]);
        _exit(valid && received == expected ? 0 : 1);
    }
    if (reader < 0) {
        Check(false, "创建短写读取进程");
        close(fds[0]); close(fds[1]);
        return;
    }
    close(fds[1]);
    const bool sent = SendBuffersWithTimeout(fds[0],
            {{header.data(), header.size()}, {nullptr, 0},
             {payload.data(), payload.size()}, {footer.data(), footer.size()}},
            2000);
    close(fds[0]);
    int status = 0;
    while (waitpid(reader, &status, 0) < 0 && errno == EINTR) {}
    Check(sent && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "1KB 发送缓冲下多次短写保持各 iovec 顺序和全部字节");
}

void TestSendFailures() {
    printf("\n\033[1;34m[11] 发送错误边界\033[0m\n");
    Check(!SendBuffersWithTimeout(-1, {{"x", 1}}, -1) && errno == EINVAL,
          "拒绝负超时");
    Check(!SendBuffersWithTimeout(-1,
            {{"x", 1}, {"x", 1}, {"x", 1}, {"x", 1}, {"x", 1},
             {"x", 1}, {"x", 1}, {"x", 1}, {"x", 1}}, 10) && errno == EINVAL,
          "拒绝超出 iovec 上限");
    Check(SendBuffersWithTimeout(-1, {{nullptr, 0}}, 10),
          "空发送不访问 socket");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    close(fds[1]);
    Check(!WsWriteFrameWithTimeout(fds[0], kWsBinary, "closed", 1000),
          "已断开的客户端返回错误，不触发 SIGPIPE");
    close(fds[0]);
}

void TestTricklingReaderDeadline() {
    printf("\n\033[1;34m[12] 持续短写仍受绝对超时限制\033[0m\n");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    int sndBuf = 1024;
    setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
    const pid_t reader = fork();
    if (reader == 0) {
        close(fds[0]);
        char buffer[256];
        while (read(fds[1], buffer, sizeof(buffer)) > 0) usleep(5000);
        close(fds[1]);
        _exit(0);
    }
    if (reader < 0) {
        Check(false, "创建慢读取进程");
        close(fds[0]); close(fds[1]);
        return;
    }
    close(fds[1]);
    const std::string payload(1u << 20, 't');
    const auto started = std::chrono::steady_clock::now();
    const bool sent = WsWriteFrameWithTimeout(fds[0], kWsBinary, payload, 50);
    const int sendError = errno;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    close(fds[0]);
    int status = 0;
    while (waitpid(reader, &status, 0) < 0 && errno == EINTR) {}
    Check(!sent && sendError == ETIMEDOUT && elapsed >= 25 && elapsed < 500,
          "客户端每 5ms 接收仍在 50ms 超时，短写不延长 deadline（%lld ms）",
          static_cast<long long>(elapsed));
}

void TestPingYieldsToStream() {
    printf("\n\033[1;34m[13] ping 不阻塞视频发送循环\033[0m\n");
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "创建 socketpair");
        return;
    }
    timeval timeout{1, 0};
    setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const uint8_t ping[] = {0x89, 0x84, 1, 2, 3, 4,
                           'p' ^ 1, 'i' ^ 2, 'n' ^ 3, 'g' ^ 4};
    const ssize_t written = write(fds[0], ping, sizeof(ping));
    const auto started = std::chrono::steady_clock::now();
    WsFrame frame;
    std::string error;
    const bool read = WsReadFrame(fds[1], &frame, &error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    uint8_t pong[6]{};
    const bool gotPong = ReadExact(fds[0], pong, sizeof(pong));
    Check(written == sizeof(ping) && read && frame.opcode == kWsPing &&
                  frame.payload == "ping" && elapsed < 500 && gotPong &&
                  pong[0] == 0x8a && pong[1] == 4 &&
                  std::string(reinterpret_cast<char*>(pong + 2), 4) == "ping",
          "仅收到 ping 就返回事件循环，并自动发送完整 pong");
    close(fds[0]);
    close(fds[1]);
}

}  // namespace

int main() {
    printf("\033[1m=== WebSocket 握手测试 ===\033[0m\n");
    TestSha1();
    TestBase64();
    TestHandshake();
    TestCloseHandshake();
    TestInvalidCloseFrames();
    TestInvalidFrameHeaders();
    TestScatterGatherWrite();
    TestWriteDeadline();
    TestFrameLengthBoundaries();
    TestScatterGatherShortWrites();
    TestSendFailures();
    TestTricklingReaderDeadline();
    TestPingYieldsToStream();
    return Summary("WebSocket");
}
