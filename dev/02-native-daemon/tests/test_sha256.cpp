// test_sha256.cpp — SHA-256 自实现的正确性
//
// 为什么值得单独测：这个哈希是**载荷校验**的依据。壳在 exec 之前核对
// /data 里那份二进制的哈希，算错了的后果是"好版本被判成坏的"（服务一直
// 回退）或更糟 —— "坏版本被判成好的"（把执行权交给一个被篡改的文件）。
//
// 期望值全部取自 NIST FIPS 180-4 的官方向量，不是自己算一遍再抄回来。
// 那不是测试，那是把 bug 固化 —— 这个项目在字母键码上吃过一次。

#include <cstdio>
#include <string>
#include <vector>

#include "sha256.h"
#include "test_util.h"

using namespace remote_control;
using namespace remote_control_test;

namespace {

void TestVectors() {
    printf("\n\033[1;34m[1] NIST 官方向量\033[0m\n");
    struct V { const char* name; std::string data; const char* hex; };
    const std::vector<V> cases = {
        {"空串", "", "e3b0c44298fc1c149afbf4c8996fb924"
                     "27ae41e4649b934ca495991b7852b855"},
        {"abc", "abc", "ba7816bf8f01cfea414140de5dae2223"
                       "b00361a396177a9cb410ff61f20015ad"},
        {"448 bit（56 字节）",
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039"
         "a33ce45964ff2167f6ecedd419db06c1"},
        {"896 bit（112 字节）",
         "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
         "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b049237"
         "0b249b11e8f07a51afac45037afee9d1"},
    };
    for (const auto& c : cases) {
        const std::string got = Sha256Hex(c.data.data(), c.data.size());
        Check(got == c.hex, "%-20s → %s", c.name, got.c_str());
    }
}

// 100 万个 'a' —— 官方向量里最长的一条，也是唯一会跨很多个 block 的。
void TestMillionA() {
    printf("\n\033[1;34m[2] 100 万个 'a'（官方向量）\033[0m\n");
    Sha256 h;
    std::vector<char> chunk(10000, 'a');
    for (int i = 0; i < 100; ++i) h.Update(chunk.data(), chunk.size());
    uint8_t d[32];
    h.Final(d);
    const std::string got = ToHex(d, 32);
    Check(got == "cdc76e5c9914fb9281a1c7e284d73e67"
                 "f1809a48a497200e046d39ccc7112cd0",
          "1000000 x 'a' → %s", got.c_str());
}

// 分块喂与一次喂必须一样 —— 缓冲区/补齐逻辑的 bug 几乎都藏在这里。
//
// ⚠️ 边界要专门挑：55/56/57/63/64/65 字节正好卡在"一个 block"的
//    两端，padding 那一段最容易错。
void TestStreaming() {
    printf("\n\033[1;34m[3] 分块喂 vs 一次喂（含 block 边界）\033[0m\n");
    std::string data;
    for (int i = 0; i < 300; ++i) {
        data.push_back(static_cast<char>('A' + (i % 26)));
    }
    for (size_t n : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 127u, 128u, 129u, 300u}) {
        const std::string once = Sha256Hex(data.data(), n);
        for (size_t step : {1u, 2u, 3u, 7u, 13u, 64u, 100u}) {
            Sha256 h;
            size_t off = 0;
            while (off < n) {
                const size_t take = (off + step <= n) ? step : (n - off);
                h.Update(data.data() + off, take);
                off += take;
            }
            uint8_t d[32];
            h.Final(d);
            const std::string got = ToHex(d, 32);
            if (got != once) {
                Check(false, "%zu 字节按 %zu 分块 → 与一次喂不一致", n, step);
                return;
            }
        }
    }
    Check(true, "12 种长度 x 7 种分块大小 全部与一次喂一致");
}

void TestFileAndSelf() {
    printf("\n\033[1;34m[4] 文件哈希 / 自身哈希\033[0m\n");
    const char* path = "/tmp/rc-sha256-test.bin";
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        Check(false, "建不了临时文件 %s", path);
        return;
    }
    fputs("abc", f);
    fclose(f);

    std::string hex;
    const bool ok = Sha256FileHex(path, &hex);
    Check(ok && hex == "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad",
          "文件内容 \"abc\" → %s", hex.c_str());
    remove(path);

    Check(!Sha256FileHex("/nonexistent/definitely/not/here", &hex),
          "读不到的文件返回 false（不是崩）");

    // /proc/self/exe 必须能读到，而且和"读文件 + 算哈希"一致 ——
    // 这是 buildId 的来源，设备上就靠它自报版本。
    std::string self;
    const bool selfOk = Sha256SelfHex(&self);
    Check(selfOk && self.size() == 64, "/proc/self/exe 哈希 = %s",
          self.empty() ? "(空)" : self.c_str());
}

}  // namespace

int main() {
    printf("\033[1m=== SHA-256 ===\033[0m\n");
    TestVectors();
    TestMillionA();
    TestStreaming();
    TestFileAndSelf();
    return Summary("SHA-256");
}
