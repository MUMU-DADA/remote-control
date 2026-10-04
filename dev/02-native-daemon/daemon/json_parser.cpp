// json_parser.cpp —— JSON 解析（**用 jsoncpp**，不再手写）
//
// ## 为什么换掉手写解析器
//
// 解析器是**唯一直接处理不可信输入**的组件：HTTP 请求体、socket 的
// JSON 应答回传，内容完全由客户端决定。手写的递归下降解析器要自己
// 处理深度、转义、UTF-8 边界、数字溢出 —— 每一条都是历史上出过事的
// 地方，而我们没有任何第三方审计。
//
// 换成 **jsoncpp**（AOSP 树内 `external/jsoncpp`，
// 版本 1.9.4，MIT）：社区用了十几年的实现，AOSP 自己也在用，
// 而且是树内依赖 —— 不需要 vendor 源码，Soong 直接链。
//
// ## 为什么保留 Value 接口
//
// `Value` 是本项目自己的薄封装，调用方（rest_api / clipops）按它写的。
// 换引擎不必连调用方一起换 —— 那样改动面和风险都大得多，
// 而 Value 本身**不解析任何东西**，只是一棵已解析好的树。
//
// ⚠️ 真正有安全含义的只有 `Parse()`。json_writer 是纯输出，
//    不碰不可信输入，所以留着。
//
// ## 两条纪律
//
//   1. **深度限制**。jsoncpp 的 reader 是递归的，默认 stackLimit=1000
//      —— 深嵌套的请求体能把栈打穿。我们的请求体全是浅对象，
//      压到 32 绰绰有余。
//   2. **绝不把 jsoncpp 的断言变成崩溃**。AOSP 编 jsoncpp 时带
//      `-DJSON_USE_EXCEPTION=0`，此时它的内部断言是 `abort()` 而不是
//      抛异常。所以这里只走 `parse()` 的返回值这条路，
//      不去碰任何可能触发断言的操作。

#include "json_parser.h"

#include <json/json.h>

#include <cstdio>

namespace remote_control {
namespace json {

namespace {

// jsoncpp 的树 → 本项目的 Value。
//
// 类型映射是直白的；只有数字要留意：jsoncpp 区分 int/uint/real，
// 我们统一收成 double（Value 内部就是 double，取值时再转 int64）。
void FromJson(const Json::Value& src, Value* dst) {
    switch (src.type()) {
        case Json::nullValue:
            dst->SetNull();
            break;
        case Json::booleanValue:
            dst->SetBool(src.asBool());
            break;
        case Json::intValue:
        case Json::uintValue:
        case Json::realValue:
            dst->SetNumber(src.asDouble());
            break;
        case Json::stringValue:
            dst->SetString(src.asString());
            break;
        case Json::arrayValue: {
            dst->SetArray();
            for (const auto& e : src) {
                Value child;
                FromJson(e, &child);
                dst->Push(std::move(child));
            }
            break;
        }
        case Json::objectValue: {
            dst->SetObject();
            for (const auto& key : src.getMemberNames()) {
                Value child;
                FromJson(src[key], &child);
                dst->Put(key, std::move(child));
            }
            break;
        }
        default:
            dst->SetNull();
            break;
    }
}

}  // namespace

bool Parse(const std::string& text, Value* out, std::string* error) {
    if (out == nullptr) {
        if (error) *error = "输出参数为空";
        return false;
    }

    Json::CharReaderBuilder builder;
    // 深嵌套是典型的解析器攻击面，而我们的请求体都是浅对象。
    builder.settings_["stackLimit"] = 32;
    // 顶层必须是对象或数组 —— 一个裸字符串当请求体没有意义，
    // 早先手写版也要求这一条，保持行为一致。
    builder.settings_["strictRoot"] = true;
    // 允许注释/尾逗号会放宽输入面，这里都要严格
    builder.settings_["allowComments"] = false;
    builder.settings_["allowTrailingCommas"] = false;
    // 拒绝重复键：两个同名键谁赢取决于实现，不如直接报错
    builder.settings_["rejectDups"] = true;
    // 允许多个顶层值会让 "{} {}" 这种输入被静默接受一半
    builder.settings_["failIfExtra"] = true;

    Json::Value root;
    std::string errs;
    Json::CharReader* reader = builder.newCharReader();
    if (reader == nullptr) {
        if (error) *error = "无法创建 JSON 解析器";
        return false;
    }
    // ⚠️ reader 的所有权在我们手里（newCharReader 返回裸指针），
    //    这里手动 delete。jsoncpp 在 AOSP 带 -DJSON_USE_EXCEPTION=0，
    //    所以更不能用依赖异常安全的写法。
    const bool ok = reader->parse(text.data(),
                                  text.data() + text.size(),
                                  &root, &errs);
    delete reader;

    if (!ok) {
        if (error) *error = errs.empty() ? "JSON 格式错误" : errs;
        return false;
    }
    Value parsed;
    FromJson(root, &parsed);
    *out = std::move(parsed);
    return true;
}

}  // namespace json
}  // namespace remote_control
