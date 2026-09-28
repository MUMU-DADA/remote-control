// encode_pool.cpp — 编码并发限制的实现

#include "encode_pool.h"

#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "remote_control_log.h"

namespace remote_control {

EncodePool::EncodePool() {
    // 默认 = CPU 核数。
    //
    // 为什么不设成"不限"：核数就是并行编码的物理上限，超过它只是
    // 让每个都变慢，还会挤占抓帧和触控注入的 CPU。下限 2 是防止
    // 单核设备上退化成完全串行（那会让一个慢客户端卡住所有人）。
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) n = 2;
    if (n > 32) n = 32;
    limit_ = static_cast<int>(n);

    // 覆盖开关。设成 0 表示不限 —— 用来对比"有/没有限制"的差别。
    const char* v = getenv("REMOTE_CONTROL_ENCODE_CONCURRENCY");
    if (v != nullptr) {
        const int x = atoi(v);
        if (x == 0) {
            limit_ = 0;
            ALOGW("编码并发不限（REMOTE_CONTROL_ENCODE_CONCURRENCY=0）—— 仅用于对比测试");
        } else if (x > 0 && x <= 64) {
            limit_ = x;
        }
    }
    ALOGI("编码并发上限: %s", limit_ == 0 ? "不限" : std::to_string(limit_).c_str());
}

EncodePool& EncodePool::Instance() {
    static EncodePool pool;
    return pool;
}

bool EncodePool::Acquire(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);

    if (limit_ == 0) {          // 不限
        ++inUse_;
        ++acquired_;
        return true;
    }

    if (inUse_ < limit_) {
        ++inUse_;
        ++acquired_;
        return true;
    }

    // 满了，等一个名额。
    //
    // 超时**不是错误**：调用方应该跳过这一帧去下一轮，而不是一直
    // 堵着。堵着的话这条连接就彻底停了 —— 连客户端的 fps/画质
    // 控制消息都收不到（它们在同一个循环里处理）。
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                 [this] { return inUse_ < limit_; });
    if (!ok) {
        ++timeouts_;
        return false;
    }
    ++inUse_;
    ++acquired_;
    return true;
}

void EncodePool::Release() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (inUse_ > 0) --inUse_;
    }
    cv_.notify_one();
}

EncodePool::Stats EncodePool::GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    Stats s;
    s.limit    = limit_;
    s.inUse    = inUse_;
    s.acquired = acquired_;
    s.timeouts = timeouts_;
    return s;
}

}  // namespace remote_control
