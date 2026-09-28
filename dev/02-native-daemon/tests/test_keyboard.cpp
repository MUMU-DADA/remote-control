// test_keyboard.cpp — 键名解析测试
//
// 键名解析是纯逻辑，不碰设备，所以能在主机上跑进回归。
// 之所以要盯住它：输入是**客户端给的字符串**，写错了不会崩，
// 只会静默地按错键 —— 那比崩溃更难发现。

#include <cstdio>
#include <string>

#include "keyboard.h"
#include "test_util.h"

using namespace remote_control;
using namespace remote_control_test;

namespace {

struct Case { const char* name; bool ok; uint32_t code; const char* why; };

void TestResolve() {
    printf("\n\033[1;34m[1] 键名解析\033[0m\n");
    const Case cases[] = {
        // Android 四大键：Linux 键码（不是 Android 的 KEYCODE_*）
        // ⚠️ 172（KEY_HOMEPAGE）不是 102（KEY_HOME）。
        //    设备上的 Generic.kl 把 102 翻成 MOVE_HOME（光标到行首），
        //    172 才是 Android 的 HOME。详见下面 [3] 那段。
        {"home",       true,  172, "KEY_HOMEPAGE → Generic.kl: HOME"},
        {"back",       true,  158, "KEY_BACK"},
        {"menu",       true,  139, "KEY_MENU"},
        {"appswitch",  true,  580, "KEY_APPSELECT"},
        {"power",      true,  116, "KEY_POWER"},
        {"volumeup",   true,  115, "KEY_VOLUMEUP"},
        // 大小写与前缀
        {"HOME",       true,  172, "大小写不敏感"},
        {"KEY_HOME",   true,  172, "内核文档写法（名字指功能，不指 Linux 常量）"},
        {"Key_Home",   true,  172, "混合大小写 + 前缀"},
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

// KnownKeyNames() 的**完整性**。
//
// ⚠️ 这段是补出来的：`browser` 和 `focus` 明明能解析，却没被列进
//    KnownKeyNames() —— 接口报 4099 时给出的"可用键"等于少报了两个。
//    用户照着那份列表找，找不到就以为不支持。
//
//    列表短了不会报错、不会崩，只会让人少用两个能用的键，
//    所以只能靠测试盯住。实现那边已经改成从表生成，这里是第二道锁。
void TestKnownNamesComplete() {
    printf("\n\033[1;34m[2] KnownKeyNames() 覆盖了表里所有具名键\033[0m\n");
    static const char* kExpected[] = {
        "home",       "back",         "menu",      "appswitch", "search",
        "power",      "volumeup",     "volumedown","mute",      "enter",
        "delete",     "backspace",    "space",     "tab",       "escape",
        "up",         "down",         "left",      "right",     "center",
        "playpause",  "nextsong",     "previoussong", "stop",   "camera",
        "browser",    "focus",
    };
    const std::string all = Keyboard::KnownKeyNames();
    std::string missing;
    for (const char* n : kExpected) {
        // 整词匹配 —— 否则 "back" 会命中 "backspace" 而假绿
        const std::string word(n);
        const bool found =
            all.compare(0, word.size() + 1, word + " ") == 0 ||
            all.find(" " + word + " ") != std::string::npos;
        if (!found) missing += word + " ";
    }
    const std::string detail =
        missing.empty() ? std::string("27 个具名键都在") : ("缺: " + missing);
    Check(missing.empty(), "KnownKeyNames() 列出了全部具名键（%s）",
          detail.c_str());
    Check(all.find("a-z") != std::string::npos &&
          all.find("f1-f12") != std::string::npos,
          "列表里保留了 a-z / f1-f12 的简写说明");
}

// 具名键：断言**扫描码**和它经 Generic.kl 翻译后的 **Android keycode**。
//
// ⚠️ 光断言扫描码是不够的 —— 用户感知的是 Android 那边的行为。
//    `home` 原来发 KEY_HOME(102)，扫描码"没错"（Linux 里它确实叫 HOME），
//    但设备上的 Generic.kl 把 102 翻成 MOVE_HOME（光标移到行首），
//    桌面纹丝不动。只测扫描码的用例会一路绿着放它过去。
//
//    期望值取自设备上实际的 /system/usr/keylayout/Generic.kl，不是猜的：
//        key 102  MOVE_HOME     key 150  EXPLORER    key 172  HOME
//        key 353  DPAD_CENTER   （KEY_OK 352 没有映射）
void TestNamedKeysAgainstLayout() {
    printf("\n\033[1;34m[3] 具名键 vs Generic.kl 翻译结果\033[0m\n");
    struct C { const char* name; uint32_t scan; const char* android; };
    static const C kCases[] = {
        {"home",        172, "HOME"},         // 不是 102（那是 MOVE_HOME）
        {"back",        158, "BACK"},
        {"menu",        139, "MENU"},
        {"appswitch",   580, "APP_SWITCH"},
        {"search",      217, "SEARCH"},
        {"power",       116, "POWER"},
        {"volumeup",    115, "VOLUME_UP"},
        {"volumedown",  114, "VOLUME_DOWN"},
        {"mute",        113, "VOLUME_MUTE"},
        {"enter",        28, "ENTER"},
        {"delete",      111, "FORWARD_DEL"},
        {"backspace",    14, "DEL"},
        {"space",        57, "SPACE"},
        {"tab",          15, "TAB"},
        {"escape",        1, "ESCAPE"},
        {"up",          103, "DPAD_UP"},
        {"down",        108, "DPAD_DOWN"},
        {"left",        105, "DPAD_LEFT"},
        {"right",       106, "DPAD_RIGHT"},
        {"center",      353, "DPAD_CENTER"},  // 不是 352（那个没映射）
        {"playpause",   164, "MEDIA_PLAY_PAUSE"},
        {"nextsong",    163, "MEDIA_NEXT"},
        {"previoussong",165, "MEDIA_PREVIOUS"},
        {"stop",        166, "MEDIA_STOP"},
        {"camera",      212, "CAMERA"},
        {"browser",     150, "EXPLORER"},     // 不是 172（那会回桌面）
        // 对焦键：模拟器的 /system/usr/keylayout 里**没有** 528 的映射，
        // 所以这一条只钉住扫描码，Android 侧按下去确实没反应。
        {"focus",       528, "（无映射，按下去没反应）"},
    };
    for (const auto& c : kCases) {
        uint32_t got = 0;
        const bool ok = Keyboard::ResolveKeyCode(c.name, &got);
        Check(ok && got == c.scan, "%-13s → 扫描码 %-4u（期望 %-4u，%s）",
              c.name, got, c.scan, c.android);
    }
    // 单独钉住最容易搞错的那对：Linux 里叫 HOME 的常量**不是**回桌面
    uint32_t homeScan = 0;
    Keyboard::ResolveKeyCode("home", &homeScan);
    Check(homeScan != 102,
          "home **不能**发 102（Generic.kl 把 102 翻成 MOVE_HOME）");
    Check(homeScan == 172, "home 发 172（Generic.kl: key 172 HOME）");
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
    printf("\n\033[1;34m[4] 26 个字母逐个核对\033[0m\n");
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
    TestKnownNamesComplete();
    TestNamedKeysAgainstLayout();
    TestLetters();
    return Summary("按键");
}
