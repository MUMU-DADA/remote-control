// test_textinput.cpp — 整串文本输入的纯逻辑部分（UTF-8 解码 + US 布局映射）
//
// 这一层不需要设备：字符表的对错、UTF-8 的边界、以及"第几个字符打不出来"
// 的定位，全都是可以脱离 uinput 验的。设备侧行为由 functional-sweep 覆盖。
//
// 为什么值得单独测：字符表是**手抄**的，抄错一个上档符号（比如把 ':'
// 映射到 KEY_SEMICOLON 之外的键）不会崩、不会报错，只会打出一个错字 ——
// 而这种错误只有靠逐条断言才抓得住。

#include <linux/input.h>
#include <stdio.h>

#include <string>

#include "test_util.h"
#include "textinput.h"

using namespace remote_control;
using namespace remote_control_test;
using namespace remote_control::textinput;

namespace {

// 断言某个字符映射到预期键码 / Shift
void ExpectMap(uint32_t cp, uint32_t wantCode, bool wantShift, const char* label) {
    uint32_t code = 0;
    bool shift = false;
    const bool ok = MapChar(cp, &code, &shift);
    Check(ok && code == wantCode && shift == wantShift,
          "%-8s → keycode=%u shift=%d%s", label, code, shift ? 1 : 0,
          ok ? "" : "（映射不出来）");
}

void TestUtf8() {
    printf("\n\033[1;34m[1] UTF-8 解码\033[0m\n");

    // ASCII
    {
        const auto d = Decode("abc");
        Check(d.codepoints.size() == 3 && d.invalidBytes == 0, "ASCII 3 个码点");
        Check(d.offsets.size() == 3 && d.offsets[0] == 0 && d.offsets[2] == 2,
              "ASCII 偏移 0,1,2");
    }

    // 中文（3 字节）
    {
        const auto d = Decode("中文");
        Check(d.codepoints.size() == 2 && d.invalidBytes == 0, "中文 2 个码点");
        Check(d.codepoints[0] == 0x4E2D && d.codepoints[1] == 0x6587,
              "中文码点 = U+4E2D U+6587（得到 U+%04X U+%04X）",
              d.codepoints.empty() ? 0 : d.codepoints[0],
              d.codepoints.size() < 2 ? 0 : d.codepoints[1]);
        Check(d.offsets[1] == 3, "第二个中文字的字节偏移 = 3（得到 %zu）", d.offsets[1]);
    }

    // emoji（4 字节，且是代理区之外的补充平面）
    {
        const auto d = Decode("😀");
        Check(d.codepoints.size() == 1 && d.codepoints[0] == 0x1F600,
              "emoji U+1F600（得到 U+%04X）", d.codepoints.empty() ? 0 : d.codepoints[0]);
        Check(d.invalidBytes == 0, "emoji 不算非法");
    }

    // 混合：ASCII + 中文 + emoji
    {
        const auto d = Decode("A中😀");
        Check(d.codepoints.size() == 3, "混合串 3 个码点（得到 %zu）", d.codepoints.size());
        Check(d.offsets[1] == 1 && d.offsets[2] == 4, "混合串偏移 0,1,4（得到 0,%zu,%zu）",
              d.offsets.size() > 1 ? d.offsets[1] : 999,
              d.offsets.size() > 2 ? d.offsets[2] : 999);
    }

    // 非法输入必须被计数，而不是静默丢弃 ——
    // 静默丢了的话，"发了一串乱码进去"会被当成成功
    {
        const auto d = Decode("\xFF\xFE");
        Check(d.invalidBytes == 2 && d.codepoints.empty(),
              "非法起始字节被计数（invalidBytes=%zu）", d.invalidBytes);
    }
    {
        // 截断的多字节序列："中" 只留前两个字节
        const auto d = Decode(std::string("中").substr(0, 2));
        Check(d.invalidBytes == 2 && d.codepoints.empty(),
              "截断的多字节序列被计数（invalidBytes=%zu）", d.invalidBytes);
    }
    {
        // 超长编码：0xC0 0x80 是 NUL 的超长写法，必须拒绝
        const auto d = Decode("\xC0\x80");
        Check(d.invalidBytes == 2 && d.codepoints.empty(),
              "超长编码被拒绝（invalidBytes=%zu）", d.invalidBytes);
    }
    {
        // 代理区 U+D800 = ED A0 80，UTF-8 里不该出现
        const auto d = Decode("\xED\xA0\x80");
        Check(d.invalidBytes == 3 && d.codepoints.empty(),
              "UTF-8 里的代理区被拒绝（invalidBytes=%zu）", d.invalidBytes);
    }

    // Encode 往返
    Check(Encode(0x4E2D) == "中", "Encode(U+4E2D) = 中");
    Check(Encode(0x1F600) == "😀", "Encode(U+1F600) = 😀");
    Check(Encode('A') == "A", "Encode('A') = A");
}

void TestMapChar() {
    printf("\n\033[1;34m[2] US 布局映射（小写/大写/数字）\033[0m\n");
    ExpectMap('a', KEY_A, false, "a");
    ExpectMap('z', KEY_Z, false, "z");
    ExpectMap('A', KEY_A, true, "A");
    ExpectMap('Z', KEY_Z, true, "Z");
    ExpectMap('m', KEY_M, false, "m");
    ExpectMap('1', KEY_1, false, "1");
    ExpectMap('9', KEY_9, false, "9");
    ExpectMap('0', KEY_0, false, "0");   // '0' 不能用 KEY_1+9 推，键码顺序里它是第 10 个
    ExpectMap(' ', KEY_SPACE, false, "(空格)");
    ExpectMap('\n', KEY_ENTER, false, "\\n");
    ExpectMap('\t', KEY_TAB, false, "\\t");

    printf("\n\033[1;34m[3] US 布局映射（上档符号）\033[0m\n");
    ExpectMap('!', KEY_1, true, "!");
    ExpectMap('@', KEY_2, true, "@");
    ExpectMap('#', KEY_3, true, "#");
    ExpectMap('$', KEY_4, true, "$");
    ExpectMap('%', KEY_5, true, "%");
    ExpectMap('^', KEY_6, true, "^");
    ExpectMap('&', KEY_7, true, "&");
    ExpectMap('*', KEY_8, true, "*");
    ExpectMap('(', KEY_9, true, "(");
    ExpectMap(')', KEY_0, true, ")");

    printf("\n\033[1;34m[4] US 布局映射（标点）\033[0m\n");
    ExpectMap('-',  KEY_MINUS,      false, "-");
    ExpectMap('_',  KEY_MINUS,      true,  "_");
    ExpectMap('=',  KEY_EQUAL,      false, "=");
    ExpectMap('+',  KEY_EQUAL,      true,  "+");
    ExpectMap('[',  KEY_LEFTBRACE,  false, "[");
    ExpectMap('{',  KEY_LEFTBRACE,  true,  "{");
    ExpectMap(']',  KEY_RIGHTBRACE, false, "]");
    ExpectMap('}',  KEY_RIGHTBRACE, true,  "}");
    ExpectMap('\\', KEY_BACKSLASH,  false, "\\");
    ExpectMap('|',  KEY_BACKSLASH,  true,  "|");
    ExpectMap(';',  KEY_SEMICOLON,  false, ";");
    ExpectMap(':',  KEY_SEMICOLON,  true,  ":");
    ExpectMap('\'', KEY_APOSTROPHE, false, "'");
    ExpectMap('"',  KEY_APOSTROPHE, true,  "\"");
    ExpectMap('`',  KEY_GRAVE,      false, "`");
    ExpectMap('~',  KEY_GRAVE,      true,  "~");
    ExpectMap(',',  KEY_COMMA,      false, ",");
    ExpectMap('<',  KEY_COMMA,      true,  "<");
    ExpectMap('.',  KEY_DOT,        false, ".");
    ExpectMap('>',  KEY_DOT,        true,  ">");
    ExpectMap('/',  KEY_SLASH,      false, "/");
    ExpectMap('?',  KEY_SLASH,      true,  "?");

    printf("\n\033[1;34m[5] 非 ASCII 一律打不出来（这是平台上限，不是缺陷）\033[0m\n");
    {
        const uint32_t notTypable[] = {0x4E2D /*中*/, 0x1F600 /*😀*/, 0x00E9 /*é*/,
                                       0x3042 /*あ*/, 0x0416 /*Ж*/, 0x0000, 0x0007};
        for (uint32_t cp : notTypable) {
            uint32_t code = 0;
            bool shift = false;
            Check(!MapChar(cp, &code, &shift), "U+%04X 映射不出来（预期）", cp);
        }
    }
}

void TestFirstUntypable() {
    printf("\n\033[1;34m[6] 「第几个字符打不出来」的定位\033[0m\n");
    uint32_t cp = 0;
    size_t idx = 0;

    Check(!FirstUntypable("Hello, world! 123", &cp, &idx), "纯 ASCII 全部可打");

    Check(FirstUntypable("abc中文", &cp, &idx), "含中文时报告打不出来");
    // ⚠️ 序号是**字符**序号而不是字节偏移：报字节偏移的话，
    //    调用方按"第 N 个字符"去数会得到误导性的位置
    Check(idx == 3, "定位到第 4 个字符（下标 3，得到 %zu）", idx);
    Check(cp == 0x4E2D, "定位到的是 U+4E2D（得到 U+%04X）", cp);

    Check(FirstUntypable("😀abc", &cp, &idx) && idx == 0, "emoji 开头 → 下标 0");
    Check(FirstUntypable("ab😀c", &cp, &idx) && idx == 2, "emoji 在中间 → 下标 2");
    Check(FirstUntypable("", &cp, &idx) == false, "空串没有打不出来的字符");
    Check(FirstUntypable("~!@#$%^&*()_+{}|:\"<>?", &cp, &idx) == false,
          "全部上档符号都可打");
}

}  // namespace

int main() {
    printf("\033[1m=== remote-control 整串文本输入（纯逻辑）===\033[0m\n");
    TestUtf8();
    TestMapChar();
    TestFirstUntypable();
    return Summary("整串文本输入");
}
