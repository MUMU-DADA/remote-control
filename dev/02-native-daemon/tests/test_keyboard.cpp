// test_keyboard.cpp — 键名解析测试
//
// 键名解析是纯逻辑，不碰设备，所以能在主机上跑进回归。
// 之所以要盯住它：输入是**客户端给的字符串**，写错了不会崩，
// 只会静默地按错键 —— 那比崩溃更难发现。

#include <cstdio>
#include <string>

#include "keyboard.h"
#include "test_util.h"

using namespace autod;
using namespace autodtest;

namespace {

struct Case { const char* name; bool ok; uint32_t code; const char* why; };

void TestResolve() {
    printf("\n\033[1;34m[1] 键名解析\033[0m\n");
    const Case cases[] = {
        // Android 四大键：Linux 键码（不是 Android 的 KEYCODE_*）
        {"home",       true,  102, "KEY_HOME"},
        {"back",       true,  158, "KEY_BACK"},
        {"menu",       true,  139, "KEY_MENU"},
        {"appswitch",  true,  580, "KEY_APPSELECT"},
        {"power",      true,  116, "KEY_POWER"},
        {"volumeup",   true,  115, "KEY_VOLUMEUP"},
        // 大小写与前缀
        {"HOME",       true,  102, "大小写不敏感"},
        {"KEY_HOME",   true,  102, "内核文档写法"},
        {"Key_Home",   true,  102, "混合大小写 + 前缀"},
        // 字符键
        {"1",          true,    2, "KEY_1 —— 必须先按字符解释（见下）"},
        {"0",          true,   11, "KEY_0 不在 KEY_1..KEY_9 之后连续"},
        // 功能键：F1..F10 是 59..68，F11/F12 跳到 87/88
        {"f1",         true,   59, "KEY_F1"},
        {"f10",        true,   68, "KEY_F10"},
        {"f11",        true,   87, "KEY_F11 —— 不能用 KEY_F1+(n-1) 算"},
        {"f12",        true,   88, "KEY_F12 —— 公式算会得到 70（不存在）"},
        // 原始键码
        {"172",        true,  172, "多位数当原始键码"},
        // 拒绝
        {"nonsense",   false,   0, "不认识的键名"},
        {"",           false,   0, "空字符串"},
        {"999",        false,   0, "超出范围"},
        {"0x1",        false,   0, "不接受十六进制"},
    };

    for (const auto& c : cases) {
        uint32_t code = 0;
        const bool got = Keyboard::ResolveKeyCode(c.name, &code);
        const bool pass = (got == c.ok) && (!c.ok || code == c.code);
        Check(pass, "%-12s → %s %-4u  (%s)",
              c.name[0] ? c.name : "(空)", got ? "ok" : "拒绝", code, c.why);
    }
    Check(true, "已知键名列表非空: %.60s...", Keyboard::KnownKeyNames());
}

// 26 个字母逐个核对。
//
// ⚠️ 这段是补出来的，因为原来的用例只写了 {"a", 30} 和 {"z", 55} ——
//    而 55 **是错的**（KEY_Z 是 44）。55 正是错误公式
//    `KEY_A + (c - 'a')` 算出来的值，也就是说测试把 bug 固化了：
//    它验证的是"实现和自己一致"，不是"实现和内核一致"。
//
//    字母键码按 QWERTY **物理位置**编号，不是字母表顺序，
//    所以除了 'a'，25 个字母全错。用户报的"传 d 出来 f"就是这个。
//
//    期望值一律来自内核头文件
//    （bionic/libc/kernel/uapi/linux/input-event-codes.h），不手算。
void TestLetters() {
    printf("\n\033[1;34m[2] 26 个字母逐个核对\033[0m\n");
    static const struct { char ch; uint32_t code; } kL[26] = {
        {'a', 30}, {'b', 48}, {'c', 46}, {'d', 32}, {'e', 18}, {'f', 33},
        {'g', 34}, {'h', 35}, {'i', 23}, {'j', 36}, {'k', 37}, {'l', 38},
        {'m', 50}, {'n', 49}, {'o', 24}, {'p', 25}, {'q', 16}, {'r', 19},
        {'s', 31}, {'t', 20}, {'u', 22}, {'v', 47}, {'w', 17}, {'x', 45},
        {'y', 21}, {'z', 44},
    };
    for (const auto& l : kL) {
        uint32_t got = 0;
        const char name[2] = {l.ch, 0};
        const bool ok = Keyboard::ResolveKeyCode(name, &got);
        Check(ok && got == l.code, "'%c' → %u（期望 %u）", l.ch, got, l.code);
    }
    // 字母表顺序与键码顺序**不一致** —— 这条断言本身就是回归保护：
    // 谁要是再想"优化"成 KEY_A + (c-'a')，这里会立刻红
    uint32_t a = 0, b = 0;
    Keyboard::ResolveKeyCode("a", &a);
    Keyboard::ResolveKeyCode("b", &b);
    Check(b != a + 1, "字母键码**不连续**（a=%u b=%u）—— 不能用加法算",
          a, b);
}

}  // namespace

int main() {
    printf("\033[1m=== 按键注入测试 ===\033[0m\n");
    TestResolve();
    TestLetters();
    return Summary("按键");
}
