// json_writer.h — 极简 JSON 输出器
//
// 为什么自己写而不是引库：autod 要能编进 AOSP（Soong）和 NDK 两种环境，
// 引第三方 JSON 库意味着多一份依赖、多一份同步成本。
//
// 为什么只写不读：请求 payload 用的是 NUL 分隔字符串（参数少而固定，
// 比 JSON 更省事也更难写错），只有应答需要 JSON —— 那就只需要输出器。
//
// 能力范围（够用即可，不追求完整实现）：
//   - 对象、数组、嵌套
//   - 字符串（转义 " \ 和控制字符，UTF-8 原样透传）
//   - 整数、浮点、布尔、null
// 不支持：解析、数字精度控制、流式输出。

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace autod {
namespace json {

class Writer {
public:
    // ── 结构 ──────────────────────────────────────────────────────────────
    Writer& Obj() {
        Sep();
        out_ += '{';
        stack_.push_back(kObj);
        needComma_ = false;
        return *this;
    }
    Writer& EndObj() {
        out_ += '}';
        Pop();
        return *this;
    }
    Writer& Arr() {
        Sep();
        out_ += '[';
        stack_.push_back(kArr);
        needComma_ = false;
        return *this;
    }
    Writer& EndArr() {
        out_ += ']';
        Pop();
        return *this;
    }

    // ── 键 ────────────────────────────────────────────────────────────────
    Writer& Key(const char* k) {
        Sep();
        Escape(k);
        out_ += ':';
        needComma_ = false;   // 值还没写，值写完才需要逗号
        afterKey_  = true;
        return *this;
    }
    Writer& Key(const std::string& k) { return Key(k.c_str()); }

    // ── 值 ────────────────────────────────────────────────────────────────
    Writer& Val(const char* v) {
        Sep();
        if (v == nullptr) {
            out_ += "null";
        } else {
            Escape(v);
        }
        return *this;
    }
    Writer& Val(const std::string& v) { return Val(v.c_str()); }
    Writer& Val(int64_t v) {
        Sep();
        out_ += std::to_string(v);
        return *this;
    }
    Writer& Val(uint64_t v) {
        Sep();
        out_ += std::to_string(v);
        return *this;
    }
    Writer& Val(int v) { return Val(static_cast<int64_t>(v)); }
    Writer& Val(unsigned v) { return Val(static_cast<uint64_t>(v)); }
    Writer& Val(bool v) {
        Sep();
        out_ += v ? "true" : "false";
        return *this;
    }
    Writer& Null() {
        Sep();
        out_ += "null";
        return *this;
    }

    // 直接嵌入一段**已经渲染好**的 JSON。
    //
    // 用途：把一个子对象交给别的模块去渲染（比如 ServiceState 自己知道
    // config/runtime 长什么样），避免为了嵌套而写一堆手工拼字符串。
    //
    // ⚠️ 不做任何校验 —— 传进来的必须是合法 JSON。传了坏数据，
    //    产出的就是坏 JSON。调用方自己保证（本项目的调用点都是
    //    同一个 Writer 渲染出来的）。
    Writer& RawJson(const std::string& rendered) {
        Sep();
        out_ += rendered;
        return *this;
    }
    // 浮点：固定 3 位小数，够表达尺寸/耗时/比例，且不会出现科学计数法
    Writer& Val(double v) {
        Sep();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        out_ += buf;
        return *this;
    }

    // ── 便捷：键 + 值 ─────────────────────────────────────────────────────
    Writer& Field(const char* k, const char* v)  { return Key(k).Val(v); }
    Writer& Field(const char* k, const std::string& v) { return Key(k).Val(v); }
    Writer& Field(const char* k, int64_t v)      { return Key(k).Val(v); }
    Writer& Field(const char* k, uint64_t v)     { return Key(k).Val(v); }
    Writer& Field(const char* k, int v)          { return Key(k).Val(v); }
    Writer& Field(const char* k, unsigned v)     { return Key(k).Val(v); }
    Writer& Field(const char* k, bool v)         { return Key(k).Val(v); }
    Writer& Field(const char* k, double v)       { return Key(k).Val(v); }

    // ── 结果 ──────────────────────────────────────────────────────────────
    const std::string& str() const { return out_; }
    size_t size() const { return out_.size(); }
    bool ok() const { return stack_.empty() && !afterKey_; }
    void Clear() {
        out_.clear();
        stack_.clear();
        needComma_ = false;
        afterKey_  = false;
    }

private:
    enum Kind { kObj, kArr };

    // 每写一个元素前决定要不要补逗号，并标记"下一个元素需要逗号"。
    //
    // ⚠️ 两个状态缺一不可：
    //   needComma_  —— 前面已经有元素，要补逗号
    //   afterKey_   —— 前面刚写完键，值是"跟着键"的，不能补逗号
    // 第一版漏了在写完值之后把 needComma_ 置位，于是所有字段粘在一起：
    //   {"ok":true"count":136}   ← 非法 JSON
    void Sep() {
        if (afterKey_) {          // 刚写完键，值直接跟，不要逗号
            afterKey_  = false;
            needComma_ = true;    // 值写完之后，下一个元素就该补逗号了
            return;
        }
        if (needComma_) out_ += ',';
        needComma_ = true;
    }

    void Pop() {
        if (!stack_.empty()) stack_.pop_back();
        needComma_ = true;
        afterKey_  = false;
    }

    void Escape(const char* s) {
        out_ += '"';
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
             *p != 0; ++p) {
            switch (*p) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                case '\b': out_ += "\\b";  break;
                case '\f': out_ += "\\f";  break;
                default:
                    if (*p < 0x20) {
                        // 其余控制字符用 \u00XX
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
                        out_ += buf;
                    } else {
                        // >= 0x20（含 UTF-8 多字节）原样透传
                        out_ += static_cast<char>(*p);
                    }
            }
        }
        out_ += '"';
    }

    std::string       out_;
    std::string       pad_;      // 未使用，保留给将来的缩进输出
    std::vector<Kind> stack_;
    bool              needComma_ = false;
    bool              afterKey_  = false;
};

}  // namespace json
}  // namespace autod
