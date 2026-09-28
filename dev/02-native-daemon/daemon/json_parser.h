// json_parser.h — 极简 JSON 解析器
//
// json_writer.h 只解决"写"。HTTP API 要读请求体，所以这里补上"读"。
//
// 为什么自己写而不是引库：和 writer 同样的理由 —— remote-control 要同时编进
// AOSP（Soong）和 NDK，引第三方库意味着多一份依赖和多一份同步成本。
//
// 能力范围（够 HTTP 请求体用即可）：
//   - 对象、数组、嵌套
//   - 字符串（含 \uXXXX 转义与代理对）、数字、true/false/null
//   - 不做流式解析、不做注释、不做尾随逗号
//
// 出错时返回 false 并给出位置，而不是抛异常 —— 这个项目全程不用异常。

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace remote_control {
namespace json {

class Value {
  public:
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Value() = default;

    Type type() const { return type_; }
    bool isNull()   const { return type_ == kNull; }
    bool isBool()   const { return type_ == kBool; }
    bool isNumber() const { return type_ == kNumber; }
    bool isString() const { return type_ == kString; }
    bool isArray()  const { return type_ == kArray; }
    bool isObject() const { return type_ == kObject; }

    // 取值。类型不符时返回默认值而不是崩溃 —— 解析外部输入时
    // "字段类型不对"是常态，不该让服务为此挂掉。
    bool        asBool(bool def = false) const {
        return type_ == kBool ? bool_ : def;
    }
    double      asDouble(double def = 0) const {
        return type_ == kNumber ? num_ : def;
    }
    int64_t     asInt(int64_t def = 0) const {
        return type_ == kNumber ? static_cast<int64_t>(num_) : def;
    }
    const std::string& asString() const {
        static const std::string kEmpty;
        return type_ == kString ? str_ : kEmpty;
    }

    // 数组
    size_t size() const {
        if (type_ == kArray) return arr_.size();
        if (type_ == kObject) return obj_.size();
        return 0;
    }
    const Value& at(size_t i) const {
        static const Value kNullValue;
        return (type_ == kArray && i < arr_.size()) ? arr_[i] : kNullValue;
    }

    // 对象。缺失的键返回 null 值，不抛异常、不插入。
    bool has(const std::string& key) const {
        return type_ == kObject && obj_.find(key) != obj_.end();
    }
    const Value& operator[](const std::string& key) const {
        static const Value kNullValue;
        if (type_ != kObject) return kNullValue;
        auto it = obj_.find(key);
        return it == obj_.end() ? kNullValue : it->second;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        if (type_ == kObject) {
            for (const auto& kv : obj_) out.push_back(kv.first);
        }
        return out;
    }

    // 便捷：取字符串，缺失/类型不符时用默认值
    std::string str(const std::string& key, const std::string& def = "") const {
        const Value& v = (*this)[key];
        return v.isString() ? v.asString() : def;
    }
    int64_t num(const std::string& key, int64_t def = 0) const {
        return (*this)[key].asInt(def);
    }
    bool flag(const std::string& key, bool def = false) const {
        return (*this)[key].asBool(def);
    }

    // 由解析器填充
    void SetNull()               { type_ = kNull; }
    void SetBool(bool v)         { type_ = kBool;   bool_ = v; }
    void SetNumber(double v)     { type_ = kNumber; num_ = v; }
    void SetString(std::string v){ type_ = kString; str_ = std::move(v); }
    void SetArray()              { type_ = kArray; }
    void SetObject()             { type_ = kObject; }
    void Push(Value v)           { arr_.push_back(std::move(v)); }
    void Put(const std::string& k, Value v) { obj_[k] = std::move(v); }

  private:
    Type        type_ = kNull;
    bool        bool_ = false;
    double      num_  = 0;
    std::string str_;
    std::vector<Value> arr_;
    std::map<std::string, Value> obj_;
};

// 解析。失败时 *error 里带位置信息，便于定位请求体哪里写错了。
bool Parse(const std::string& text, Value* out, std::string* error);

}  // namespace json
}  // namespace remote_control
