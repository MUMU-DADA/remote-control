// sha256.h — 自带 SHA-256（约 120 行）
//
// 为什么要自己写：这套东西用它做**载荷校验**（壳在 exec 之前核对
// /data 里那份二进制的哈希）和 **buildId**（服务自报正在跑的是哪一版）。
// 为这两件事去链 OpenSSL 不划算 —— AOSP 里 libcrypto 的可用性随分区与
// 版本变化，而这个算法是冻结的、实现只有百来行，没有维护面。
//
// ⚠️ 校验用的哈希**必须**来自可信来源：这里的期望值是槽目录名
//    （/data/misc/remote-control/releases/<sha256>/），由部署脚本写入，
//    不是从同一个目录里读出来的"自证"文件。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace remote_control {

class Sha256 {
  public:
    Sha256();
    void Update(const void* data, size_t len);
    void Final(uint8_t out[32]);

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

  private:
    void Transform(const uint8_t block[64]);

    uint32_t      st_[8];
    uint64_t      bits_ = 0;
    uint8_t       buf_[64];
    size_t        bufLen_ = 0;
};

// 小写十六进制
std::string ToHex(const uint8_t* data, size_t len);

std::string Sha256Hex(const void* data, size_t len);

// 整个文件；读不到或读失败返回 false
bool Sha256FileHex(const std::string& path, std::string* out);

// 当前可执行文件（/proc/self/exe）。拿来当 buildId —— 这样不用在构建
// 系统里额外插一个版本号，二进制一换哈希就变，天然不会对不上。
bool Sha256SelfHex(std::string* out);

// 本进程二进制的 sha256，**算一次就缓存**。这就是服务的 buildId：
//   · `--version` 打印它
//   · /describe 与 /info 带上它 —— 热替换之后靠它确认"跑的确实是新版"
//   · 壳用它核对载荷（载荷把自哈希写进 ready 文件）
// 算不出来时返回 "unknown"（读不到 /proc/self/exe 的极端情况）。
const std::string& SelfBuildId();

}  // namespace remote_control
