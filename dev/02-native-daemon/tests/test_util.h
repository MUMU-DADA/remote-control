// test_util.h —— 测试公共工具
//
// 几个测试都要用：断言计数、查找 uinput 设备节点、读回事件、解析能力位图。
// 抽出来避免重复。

#pragma once

#include <cstdint>   // uint16_t 等；不要依赖 <linux/input.h> 间接引入

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include <string>
#include <vector>

namespace remote_control_test {

inline int gChecks = 0;
inline int gFailed = 0;

// 断言。注意：调用方不要把「会写变量的函数调用」和「读该变量」
// 塞进同一个 Check() 实参列表 —— C++ 实参求值顺序未指定，会打印旧值。
inline void Check(bool cond, const char* fmt, ...) {
    ++gChecks;
    va_list ap;
    va_start(ap, fmt);
    printf(cond ? "  \033[1;32m✓\033[0m " : "  \033[1;31m✗\033[0m ");
    if (!cond) ++gFailed;
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// 非断言的信息输出（缩进与 Check 对齐）
inline void Info(const char* fmt, ...) {
    printf("    \033[2m");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\033[0m\n");
}

inline int Summary(const char* title) {
    printf("\n\033[1m=== %s ===\033[0m\n", title);
    if (gFailed == 0) {
        printf("\033[1;32m全部通过\033[0m  (%d 项检查)\n", gChecks);
        return 0;
    }
    printf("\033[1;31m%d / %d 项失败\033[0m\n", gFailed, gChecks);
    return 1;
}

// ---------------------------------------------------------------------------
// 设备节点发现
// ---------------------------------------------------------------------------

inline std::string FindEventNode(const std::string& deviceName) {
    for (int i = 0; i < 64; ++i) {
        const std::string namePath =
                "/sys/class/input/event" + std::to_string(i) + "/device/name";
        FILE* f = fopen(namePath.c_str(), "r");
        if (!f) continue;

        char buf[256] = {0};
        const bool ok = fgets(buf, sizeof(buf), f) != nullptr;
        fclose(f);
        if (!ok) continue;

        std::string name(buf);
        while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) {
            name.pop_back();
        }
        if (name == deviceName) {
            return "/dev/input/event" + std::to_string(i);
        }
    }
    return {};
}

inline std::string ReadDeviceBlock(const std::string& deviceName) {
    FILE* f = fopen("/proc/bus/input/devices", "r");
    if (!f) return {};

    std::string block;
    std::string line;
    char buf[512];
    bool inBlock = false;

    while (fgets(buf, sizeof(buf), f)) {
        line = buf;
        if (line.rfind("N: Name=", 0) == 0) {
            inBlock = line.find("\"" + deviceName + "\"") != std::string::npos;
            if (inBlock) block.clear();
        }
        if (inBlock) block += line;
    }
    fclose(f);
    return block;
}

// 解析 "B: KEY=400 0 0 0 0 0"。
// 内核的 input_seq_print_bitmap() 从高位字往低位字打印，并跳过前导全零字，
// 所以返回的 vector 下标 0 是最高位的字。
inline std::vector<unsigned long> ExtractBitmapWords(const std::string& block,
                                                     const std::string& key) {
    std::vector<unsigned long> words;

    const std::string prefix = "B: " + key + "=";
    const size_t pos = block.find(prefix);
    if (pos == std::string::npos) return words;

    const size_t lineEnd = block.find('\n', pos);
    const std::string line =
            block.substr(pos + prefix.size(), lineEnd - pos - prefix.size());

    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        words.push_back(strtoul(line.substr(start, i - start).c_str(),
                                nullptr, 16));
    }
    return words;
}

inline bool BitmapHasBit(const std::vector<unsigned long>& words, int bit) {
    constexpr int kBitsPerWord = 64;   // x86_64 / aarch64 上 unsigned long 是 64 位
    const int wordIdx = bit / kBitsPerWord;
    const int offset  = bit % kBitsPerWord;

    const int listIdx = static_cast<int>(words.size()) - 1 - wordIdx;
    if (listIdx < 0 || listIdx >= static_cast<int>(words.size())) return false;
    return (words[listIdx] >> offset) & 1UL;
}

// ---------------------------------------------------------------------------
// 事件读回
// ---------------------------------------------------------------------------

inline std::vector<input_event> DrainEvents(int fd) {
    std::vector<input_event> out;
    input_event ev{};
    while (true) {
        const ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev))) break;
        out.push_back(ev);
    }
    return out;
}

// 阻塞等待最多 timeoutMs，收集到至少 minEvents 个事件或超时为止
inline std::vector<input_event> WaitEvents(int fd, size_t minEvents,
                                           int timeoutMs) {
    std::vector<input_event> out;
    const int stepUs = 2000;
    int waitedUs = 0;
    const int timeoutUs = timeoutMs * 1000;

    while (waitedUs < timeoutUs) {
        auto batch = DrainEvents(fd);
        out.insert(out.end(), batch.begin(), batch.end());
        if (out.size() >= minEvents) break;
        usleep(stepUs);
        waitedUs += stepUs;
    }
    return out;
}

inline void DumpEvents(const std::vector<input_event>& evs) {
    static const char* kTypeNames[] = {"SYN", "KEY", "REL", "ABS", "MSC", "SW "};
    for (const auto& e : evs) {
        const char* tn = (e.type < 6) ? kTypeNames[e.type] : "???";
        printf("      %-4s code=%-3u value=%d\n", tn, e.code, e.value);
    }
}

inline int CountOf(const std::vector<input_event>& evs, uint16_t type,
                   uint16_t code, int32_t value) {
    int n = 0;
    for (const auto& e : evs) {
        if (e.type == type && e.code == code && e.value == value) ++n;
    }
    return n;
}

inline int CountSyn(const std::vector<input_event>& evs) {
    return CountOf(evs, EV_SYN, SYN_REPORT, 0);
}

inline bool LastValueOf(const std::vector<input_event>& evs, uint16_t type,
                        uint16_t code, int32_t* out) {
    bool found = false;
    for (const auto& e : evs) {
        if (e.type == type && e.code == code) {
            *out  = e.value;
            found = true;
        }
    }
    return found;
}

}  // namespace remote_control_test
