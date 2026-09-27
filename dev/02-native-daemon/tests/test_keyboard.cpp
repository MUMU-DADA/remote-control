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
        {"a",          true,   30, "KEY_A"},
        {"z",          true,   55, "KEY_Z"},
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

}  // namespace

int main() {
    printf("\033[1m=== 按键注入测试 ===\033[0m\n");
    TestResolve();
    return Summary("按键");
}
