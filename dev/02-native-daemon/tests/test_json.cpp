// test_json.cpp — JSON 解析器与输出器测试
//
// 这两个模块是 HTTP API 的输入输出边界：解析器读请求体，输出器写应答。
// 它们出错的表现不是崩溃，而是"接口悄悄返回了错的东西" —— 所以
// 边界情况（代理对、转义、非法输入）必须钉住。

#include <cstdio>
#include <string>

#include "json_parser.h"
#include "json_writer.h"
#include "test_util.h"

using namespace autod;
using namespace autodtest;

namespace {

void TestParseBasics() {
    printf("\n\033[1;34m[1] 解析：基本类型\033[0m\n");
    json::Value v;
    std::string err;

    Check(json::Parse(R"({"x":10,"y":20,"name":"hello"})", &v, &err), "解析对象");
    Check(v.num("x") == 10 && v.num("y") == 20, "取数字");
    Check(v.str("name") == "hello", "取字符串");
    Check(v.num("missing", -1) == -1, "缺失键返回默认值而不是崩溃");
    Check(!v.has("missing"), "has() 对缺失键为 false");

    Check(json::Parse(R"({"a":{"b":[1,2,3]}})", &v, &err), "解析嵌套");
    Check(v["a"]["b"].size() == 3 && v["a"]["b"].at(1).asInt() == 2, "嵌套取值");

    Check(json::Parse(R"({"f":true,"n":false,"z":null})", &v, &err), "布尔与 null");
    Check(v.flag("f") && !v.flag("n") && v["z"].isNull(), "布尔与 null 取值");

    Check(json::Parse(R"({"pi":3.14,"e":1e3,"neg":-42})", &v, &err), "数字形态");
    Check(v["e"].asInt() == 1000 && v.num("neg") == -42, "科学计数与负数");
}

void TestParseStrings() {
    printf("\n\033[1;34m[2] 解析：字符串与转义\033[0m\n");
    json::Value v;
    std::string err;

    Check(json::Parse(R"({"s":"a\"b\\c\nd\u4e2d\ud83d\ude00"})", &v, &err), "含转义的字符串");
    const std::string s = v.str("s");
    Check(s.find("a\"b\\c") != std::string::npos, "引号与反斜杠");
    Check(s.find('\n') != std::string::npos, "\\n 转成换行");
    Check(s.find("中") != std::string::npos, "\\u4e2d → 中");
    // 代理对必须合成一个码点，而不是两个孤立的 UTF-8 序列
    Check(s.find("\xF0\x9F\x98\x80") != std::string::npos, "代理对合成 emoji");
}

void TestParseErrors() {
    printf("\n\033[1;34m[3] 解析：非法输入必须被拒\033[0m\n");
    json::Value v;
    std::string err;
    struct Case { const char* text; const char* what; };
    const Case cases[] = {
        {R"({"a":})",     "缺值"},
        {R"({"a":1,})",   "尾随逗号"},
        {R"({"a" 1})",    "缺冒号"},
        {"[1,2",          "未闭合数组"},
        {"",              "空输入"},
        {R"({"a":"x})",   "未闭合字符串"},
        {"{a:1}",         "键没加引号"},
        {"nope",          "不是值"},
    };
    for (const auto& c : cases) {
        Check(!json::Parse(c.text, &v, &err), "拒绝: %s", c.what);
    }
    // 错误信息要能定位。
    //
    // 这里不能钉死措辞 —— 解析引擎换成 jsoncpp 之后格式变了
    // （原来是「位置 N」，现在是 "Line 1, Column 6"）。
    // 要钉的是「有没有定位信息」，不是「用的哪种写法」。
    // 钉死措辞的测试会在换实现时红一次，然后被人随手改掉，从此失去意义。
    json::Parse("{\"a\":", &v, &err);
    const bool hasPos = err.find("Line") != std::string::npos ||
                        err.find("Column") != std::string::npos ||
                        err.find("位置") != std::string::npos;
    Check(hasPos, "错误信息带位置: %s", err.c_str());
    Check(!err.empty(), "错误信息非空");
}

void TestWriter() {
    printf("\n\033[1;34m[4] 输出：结构、转义与合法性\033[0m\n");
    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("count", 136)
        .Field("name", "含\"引号\"和\\反斜杠\n换行")
        .Field("ratio", 0.125)
        .Key("apps").Arr()
            .Obj().Field("package", "com.a").Field("versionCode", 42).EndObj()
            .Obj().Field("package", "com.b").Field("system", false).EndObj()
        .EndArr()
        .Key("nothing").Null()
     .EndObj();

    Check(w.ok(), "结构完整（括号配平）");
    const std::string out = w.str();
    Check(out.find("\"ok\":true,") != std::string::npos, "字段之间有逗号");
    Check(out.find(",{\"package\"") != std::string::npos, "数组元素之间有逗号");

    // 用解析器回读 —— 自己写的东西自己读得回来，是最基本的自洽性检查
    json::Value back;
    std::string err;
    Check(json::Parse(out, &back, &err), "输出能被自己的解析器读回");
    Check(back.num("count") == 136 && back["apps"].size() == 2, "回读内容一致");
    Check(back["apps"].at(1)["system"].isBool(), "嵌套布尔回读正确");

    // 空对象 / 空数组
    json::Writer w2;
    w2.Obj().Key("a").Arr().EndArr().Key("b").Obj().EndObj().Field("c", 1).EndObj();
    json::Value b2;
    Check(json::Parse(w2.str(), &b2, &err), "空数组/空对象合法");
    Check(b2["a"].size() == 0 && b2["b"].size() == 0 && b2.num("c") == 1,
          "空容器与后续字段都对");
}

}  // namespace

int main() {
    printf("\033[1m=== JSON 解析器 / 输出器测试 ===\033[0m\n");
    TestParseBasics();
    TestParseStrings();
    TestParseErrors();
    TestWriter();
    return Summary("JSON");
}
