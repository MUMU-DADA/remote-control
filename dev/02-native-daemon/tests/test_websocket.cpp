// test_websocket.cpp — WebSocket 握手测试
//
// 为什么这个必须离线测：握手算错的表现是**浏览器直接拒绝连接**，
// 页面上什么都不显示，控制台里也只有一句 "WebSocket connection failed"。
// 没有可读的错误信息，只能靠对着 RFC 的标准向量验证。

#include "websocket.h"
#include "test_util.h"

#include <cstdio>
#include <string>

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

}  // namespace

int main() {
    printf("\033[1m=== WebSocket 握手测试 ===\033[0m\n");
    TestSha1();
    TestBase64();
    TestHandshake();
    return Summary("WebSocket");
}
