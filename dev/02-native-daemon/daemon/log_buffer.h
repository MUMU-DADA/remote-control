// log_buffer.h — 进程内的日志环形缓冲
//
// 为什么需要：日志目前只进 logcat / stderr，客户端拿不到。
// 而"API 要能控制服务自身"就包括"看服务自己在说什么" ——
// 排查问题时最有用的往往就是最近几十行日志，而不是再让用户去
// adb logcat 里翻。
//
// 固定容量的环形缓冲：内存占用有上界，长期运行不会涨。
// 满了覆盖最旧的，并记录被丢弃的条数（客户端据此知道日志有缺口）。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace remote_control {

enum class LogLevel : int {
    kDebug = 0,
    kInfo  = 1,
    kWarn  = 2,
    kError = 3,
};

const char* LogLevelName(LogLevel lv);

struct LogLine {
    uint64_t seq = 0;          // 单调递增序号，客户端用它做增量拉取
    int64_t  timeMs = 0;       // 单调时钟（不是墙上时间，避免时区/跳变问题）
    // 墙上时间（秒）。界面要显示"几点几分发生的"，单调时钟给不了这个；
    // 而排序和增量拉取用单调时钟更稳（不受系统时间被改的影响）。
    // 两个都留着，各干各的。
    int64_t  wallSec = 0;
    // 预格式化的 "MM-DD HH:MM:SS"，显示端直接用，不用各自再做一遍时区转换
    char     timeStr[24] = {};
    LogLevel level = LogLevel::kInfo;
    std::string tag;
    std::string text;
};

class LogBuffer {
  public:
    static LogBuffer& Instance();

    // 追加一行。超过容量时覆盖最旧的。
    void Append(LogLevel level, const char* tag, const std::string& text);

    // 取序号 > sinceSeq 的行，最多 maxLines 条。
    //
    // 返回时把当前最大序号写进 *latestSeq，客户端下次用它做 sinceSeq ——
    // 这样断线重连后能续上，而不是每次重拉全部。
    std::vector<LogLine> Since(uint64_t sinceSeq, size_t maxLines,
                               uint64_t* latestSeq) const;

    // 被覆盖掉的条数（>0 说明客户端可能漏了日志）
    uint64_t DroppedCount() const;

    // 级别过滤：低于该级别的日志不再入缓冲。
    // 放在缓冲层而不是写入层，是因为写入点遍布各处，
    // 而在缓冲层过滤只需要一个判断。
    void SetMinLevel(LogLevel lv);
    LogLevel MinLevel() const;

    void Clear();

    // ── 落盘的历史日志 ──
    //
    // 内存环形缓冲关掉进程就没了，排障时最需要的恰恰是"上次为什么退出的"。
    // 所以同时往共享存储写一份，**只保留最近 10KB** —— 常驻服务写的日志
    // 没有上限的话，跑几天就能把 /sdcard 塞满。
    //
    // 放 /sdcard 而不是 /data/local/tmp：上位应用（无 root）也应当能读到它。
    // 上限 10KB，**文件不会超过它 + 一行**。
    //
    // 裁剪留了滞回：超过 10KB 就裁到 7.5KB，而不是"裁到刚好 10KB"。
    // 后者会让下一行又超、又裁一次 —— 每写一行就读写 20KB。
    // 带滞回之后每 ~2.5KB 才裁一次，文件仍然满足"只留最近 10KB"。
    static constexpr size_t kHistoryMaxBytes  = 10 * 1024;
    static constexpr size_t kHistoryTrimTo    = 7680;

    // 设置落盘路径。传空串则关闭落盘。启动时调一次。
    void SetHistoryPath(const std::string& path);
    std::string HistoryPath() const;

    // 读回落盘的那份历史（可能为空）
    std::string ReadHistory() const;

  private:
    LogBuffer() = default;
    // 容量：2048 行。按每行平均 120 字节算约 240KB，
    // 对一个常驻服务是可以接受的常量占用。
    static constexpr size_t kCapacity = 2048;

    void AppendToHistory(const LogLine& line);
    void TrimHistory();

    mutable std::mutex mutex_;
    std::string historyPath_;
    std::vector<LogLine> lines_;      // 环形，逻辑顺序由 firstSeq_ 决定
    size_t   head_ = 0;               // 下一个写入位置
    size_t   count_ = 0;              // 当前有效条数
    uint64_t nextSeq_ = 1;
    uint64_t dropped_ = 0;
    LogLevel minLevel_ = LogLevel::kInfo;
};

// 供 remote_control_log.h 使用的便捷入口
void LogBufferAppend(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace remote_control
