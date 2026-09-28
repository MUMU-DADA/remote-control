// log_buffer.cpp — 进程内日志环形缓冲

#include "log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
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

    // 墙上时间：在缓冲层统一打，而不是让每个写入点自己格式化。
    // 写入点遍布各处，漏一个就会出现没有时间的日志行。
    {
        const time_t now = time(nullptr);
        line.wallSec = static_cast<int64_t>(now);
        struct tm tmv{};
        localtime_r(&now, &tmv);
        strftime(line.timeStr, sizeof(line.timeStr), "%m-%d %H:%M:%S", &tmv);
    }
    line.level  = level;
    line.tag    = tag != nullptr ? tag : "";
    line.text   = text;

    if (lines_.size() < kCapacity) {
        lines_.push_back(line);        // 不 move —— 下面还要用它落盘
        head_ = lines_.size();
        if (head_ == kCapacity) head_ = 0;
        ++count_;
        AppendToHistory(line);
        return;
    }

    // 满了：覆盖 head_ 位置
    lines_[head_] = line;          // 不 move —— 下面还要用它落盘
    head_ = (head_ + 1) % kCapacity;
    ++dropped_;
    AppendToHistory(line);
}

// ── 落盘 ────────────────────────────────────────────────────────────────────
//
// 只保留最近 kHistoryMaxBytes 字节。
// 做法是"追加到超过 kHistoryTrimAt 时裁一次"，而不是每行都裁 ——
// 每行都读回 10KB 再写出去，日志一多就成了主要的 CPU 开销。
//
// 裁剪取的是**文件的尾部**：日志的价值在最近的，砍头留尾。
void LogBuffer::AppendToHistory(const LogLine& line) {
    if (historyPath_.empty()) return;

    // 时间直接用缓冲层算好的，不再重新格式化一遍 ——
    // 两处各写一次的话，格式一旦不一致（比如一处带秒一处不带），
    // 同一个文件里就会出现两种时间格式。
    static const char* kLevelChar = "IWED";
    const int li = static_cast<int>(line.level);
    const char lc = kLevelChar[(li >= 0 && li < 4) ? li : 0];

    std::string out;
    out.reserve(line.text.size() + 48);
    out += line.timeStr;
    out += ' ';
    out += lc;
    out += ' ';
    out += line.text;
    out += '\n';

    // 追加。用 O_APPEND 而不是自己维护偏移 —— 即使有别的写入者
    // （比如用户手工 echo）也不会互相覆盖。
    const int fd = open(historyPath_.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0660);
    if (fd < 0) return;          // 写不了就算了，日志不该影响服务本身
    ssize_t ig = write(fd, out.data(), out.size());
    (void)ig;

    // 超过阈值就裁。fstat 比每次自己记账可靠（文件可能被外部改过）。
    struct stat st{};
    if (fstat(fd, &st) == 0 &&
        static_cast<size_t>(st.st_size) > kHistoryMaxBytes) {
        close(fd);
        TrimHistory();
        return;
    }
    close(fd);
}

void LogBuffer::TrimHistory() {
    // 读出整个文件，留下尾部 kHistoryMaxBytes，重写。
    // 走临时文件 + rename：读的时候可能有人在看这个文件，
    // 直接 truncate 会让他们看到半截内容。
    const int fd = open(historyPath_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    std::string all;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    if (all.size() <= kHistoryTrimTo) return;

    size_t cut = all.size() - kHistoryTrimTo;
    // 从行首开始，避免留下半行 —— 半行日志比少几行更难读
    const size_t nl = all.find('\n', cut);
    if (nl != std::string::npos) cut = nl + 1;

    const std::string tmp = historyPath_ + ".tmp";
    const int wfd = open(tmp.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (wfd < 0) return;
    size_t sent = 0;
    const std::string keep = all.substr(cut);
    while (sent < keep.size()) {
        const ssize_t n = write(wfd, keep.data() + sent, keep.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        sent += static_cast<size_t>(n);
    }
    fsync(wfd);
    close(wfd);
    rename(tmp.c_str(), historyPath_.c_str());
}

void LogBuffer::SetHistoryPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    historyPath_ = path;
    if (historyPath_.empty()) return;

    // 启动时先裁一次：上次退出时可能刚好留了个大文件
    TrimHistory();
}

std::string LogBuffer::HistoryPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return historyPath_;
}

std::string LogBuffer::ReadHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (historyPath_.empty()) return {};
    const int fd = open(historyPath_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string all;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return all;
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
