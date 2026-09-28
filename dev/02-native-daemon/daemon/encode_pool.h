// encode_pool.h — 编码并发限制
//
// ## 为什么需要它（以及不需要什么）
//
// 做完共享抓帧之后，"N 个客户端 = N 次抓帧"已经解决了。剩下的是
// **N 个客户端 = N 个并发编码**：
//
//   共享帧发布 → 所有客户端同时被唤醒 → 同时开始编码
//
// 每个编码本身不贵（JPEG 1.1ms，占帧预算的 3%），但它们是**同时**
// 发生的。10 个客户端在 4 核上就是 10 路并发编码互相抢 CPU，
// 结果每一个都变慢 —— 包括那台设备上正在跑的别的线程（抓帧、
// 触控注入）。
//
// ## 为什么是"限制并发"而不是"工作队列"
//
// 一个真正的工作池（提交任务 → 空闲 worker 编码 → 回调送出去）
// 在这里没有好处：
//
//   - 编码的结果要发回**特定的那条连接**，回调会把控制流拆散
//   - 每个客户端的编码参数不同（格式/质量/降采样），任务不可互换
//   - 编码本身是纯 CPU 的，换哪个线程做都一样
//
// 需要的只是"同时最多 K 个在编"。那就是一个计数信号量。
//
// ## 代价
//
// 超过 K 个客户端时，多出来的要**排队** —— 帧率会掉。
// 这是有意的取舍：与其 10 个都卡，不如 4 个流畅 + 6 个慢一点。
// 而且共享抓帧保证了排队不会反过来拖慢抓帧（抓帧在自己的线程里）。

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace autod {

class EncodePool {
  public:
    static EncodePool& Instance();

    // 同时能编几路。默认 = CPU 核数（下限 2）。
    //
    // AUTOD_ENCODE_CONCURRENCY 可以覆盖（0 = 不限，调试对比用）。
    int Limit() const { return limit_; }

    // 拿一个名额。拿不到就等，最多 timeoutMs；超时返回 false。
    //
    // 超时不是错误 —— 调用方应该**跳过这一帧**继续下一轮，
    // 而不是一直堵在这里（那会把整条连接拖死）。
    bool Acquire(int timeoutMs);

    // 还回去。和 Acquire 配对，用 RAII 保证异常/提前 return 也能还。
    void Release();

    // RAII 包装。**用它**，别手写 Acquire/Release ——
    // 编码路径有好几个提前 return 的分支，漏一个名额就永久丢了，
    // 丢够 K 个之后整个服务再也编不出任何东西。
    class Guard {
      public:
        explicit Guard(int timeoutMs) : ok_(Instance().Acquire(timeoutMs)) {}
        ~Guard() { if (ok_) Instance().Release(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool acquired() const { return ok_; }

      private:
        bool ok_;
    };

    struct Stats {
        int      limit    = 0;
        int      inUse    = 0;
        uint64_t acquired = 0;
        uint64_t timeouts = 0;   // 因为拿不到名额而跳过的帧数
    };
    Stats GetStats() const;

  private:
    EncodePool();
    ~EncodePool() = default;
    EncodePool(const EncodePool&) = delete;
    EncodePool& operator=(const EncodePool&) = delete;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    int      limit_    = 0;   // 0 = 不限
    int      inUse_    = 0;
    uint64_t acquired_ = 0;
    uint64_t timeouts_ = 0;
};

}  // namespace autod
