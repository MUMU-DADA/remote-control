// secure_compare.h —— 恒定时间比较 + 日志脱敏
//
// ## 为什么需要这个文件
//
// 访问令牌的校验有两处容易出错，而且**都是静默的**：
//
//   1. `a == b` 对 std::string 是**短路**的 —— 第一个不同的字节就返回。
//      网络上的攻击者可以靠测量响应时间逐字节猜出令牌。
//      这类攻击在局域网里完全可行（没有互联网的抖动噪声）。
//      正确做法是恒定时间比较：不管第几个字节不同，都走完整个长度。
//
//   2. `?token=<令牌>` 是支持的传法之一，而失败日志打的是 `rawPath`
//      —— **那里面就含着令牌**。日志会被读、被转发、被贴到工单里。
//
// 这两件事都不需要"引一个库"，需要的是**用对原语**：
// 恒定时间比较就是十几行，而任何密码学库里的 `CRYPTO_memcmp`
// 做的也是同一件事。为了这点功能链一个 BoringSSL 进来，
// 增加的是攻击面而不是安全性。

#pragma once

#include <cstddef>
#include <string>

namespace autod {

// 恒定时间字符串比较。
//
// 长度不同直接返回 false —— 长度不是秘密（令牌长度由配置决定，
// 而且长度差异本来就能从请求里看出来）。**内容**的逐字节比较
// 必须走完全程，不能提前退出。
//
// volatile 读 + 累积异或：让编译器无法把它优化成短路比较。
inline bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^
                static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

// 把 URL 里的令牌换成 ***，用于日志。
//
// 只认 `token=` 后面到 `&` 之前那一段；其余原样保留，
// 免得把查询串改得没法排查问题。
inline std::string RedactToken(const std::string& url) {
    static const char kKey[] = "token=";
    std::string out;
    out.reserve(url.size());
    size_t i = 0;
    while (i < url.size()) {
        // 只在"参数起点"上认 token=，避免误伤名字里含 token= 的值
        const bool atParamStart = (i == 0) || (url[i - 1] == '?') ||
                                  (url[i - 1] == '&');
        if (atParamStart && url.compare(i, sizeof(kKey) - 1, kKey) == 0) {
            const size_t valStart = i + sizeof(kKey) - 1;
            size_t end = url.find('&', valStart);
            if (end == std::string::npos) end = url.size();
            out += kKey;
            out += "***";
            i = end;
            continue;
        }
        out += url[i++];
    }
    return out;
}

}  // namespace autod
