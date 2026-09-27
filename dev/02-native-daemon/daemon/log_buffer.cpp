// log_buffer.cpp — 进程内日志环形缓冲

#include "log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mutex>

namespace autod {
namespace {

int64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

const char* LevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::kDebug: return "D";
        case LogLevel::kInfo:  return "I";
        case LogLevel::kWarn:  return "W";
        case LogLevel::kError: return "E";
    }
    return "?";
}

}  // namespace

const char* LogLevelName(LogLevel lv) { return LevelName(lv); }

LogBuffer& LogBuffer::Instance() {
    // 函数内静态：C++11 保证线程安全的首次初始化
    static LogBuffer instance;
    return instance;
}

void LogBuffer::Append(LogLevel level, const char* tag, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (static_cast<int>(level) < static_cast<int>(minLevel_)) return;

    LogLine line;
    line.seq    = nextSeq_++;
    line.timeMs = MonotonicMs();
    line.level  = level;
    line.tag    = tag != nullptr ? tag : "";
    line.text   = text;

    if (lines_.size() < kCapacity) {
        lines_.push_back(std::move(line));
        head_ = lines_.size();
        if (head_ == kCapacity) head_ = 0;
        ++count_;
        return;
    }

    // 满了：覆盖 head_ 位置
    lines_[head_] = std::move(line);
    head_ = (head_ + 1) % kCapacity;
    ++dropped_;
}

std::vector<LogLine> LogBuffer::Since(uint64_t sinceSeq, size_t maxLines,
                                      uint64_t* latestSeq) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (latestSeq != nullptr) {
        *latestSeq = nextSeq_ > 0 ? nextSeq_ - 1 : 0;
    }

    std::vector<LogLine> out;
    if (count_ == 0) return out;

    // 环形按逻辑顺序展开：最旧的一条在 (head_ - count_ + kCapacity) % kCapacity
    const size_t start = (head_ + kCapacity - count_) % kCapacity;
    const size_t total = count_;

    // 只取 seq > sinceSeq 的。环形里 seq 是单调的，所以可以二分，
    // 但容量只有 2048，线性扫足够且更不容易写错。
    for (size_t i = 0; i < total; ++i) {
        const LogLine& ln = lines_[(start + i) % kCapacity];
        if (ln.seq <= sinceSeq) continue;
        out.push_back(ln);
        if (out.size() >= maxLines) break;
    }
    return out;
}

uint64_t LogBuffer::DroppedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void LogBuffer::SetMinLevel(LogLevel lv) {
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = lv;
}

LogLevel LogBuffer::MinLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return minLevel_;
}

void LogBuffer::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    head_  = 0;
    count_ = 0;
    // nextSeq_ 不重置：客户端手里的 sinceSeq 仍然有效，
    // 清空后它只会拉到空结果，而不会因为序号回绕而重复拉到旧日志。
}

// 供 autod_log.h 使用
void LogBufferAppend(LogLevel level, const char* tag, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogBuffer::Instance().Append(level, tag, buf);
}

}  // namespace autod
