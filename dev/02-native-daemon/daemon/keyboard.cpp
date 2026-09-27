// keyboard.cpp — uinput 虚拟键盘

#include "keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <map>

#include "autod_log.h"

namespace autod {
namespace {

// 设备创建后内核需要一点时间注册，太快发事件会丢
constexpr int kSettleUs = 100 * 1000;

// ── 键名表 ──────────────────────────────────────────────────────────────────
//
// 覆盖 Android 上真正会用到的键。不是把 <linux/input-event-codes.h> 抄一遍 ——
// 那张表有 700 多项，绝大多数在 Android 上不存在对应行为，
// 列出来只会让 Describe 的输出变成噪音。
//
// 想用表外的键，直接给数字键码即可（ResolveKeyCode 支持）。
const std::map<std::string, uint32_t>& KeyTable() {
    static const std::map<std::string, uint32_t> kTable = {
        // Android 四大键
        {"home",        KEY_HOME},        // 172
        {"back",        KEY_BACK},        // 158
        {"menu",        KEY_MENU},        // 139
        {"appswitch",   KEY_APPSELECT},   // 580
        {"search",      KEY_SEARCH},      // 217

        // 电源与音量
        {"power",       KEY_POWER},       // 116
        {"volumeup",    KEY_VOLUMEUP},    // 115
        {"volumedown",  KEY_VOLUMEDOWN},  // 114
        {"mute",        KEY_MUTE},        // 113

        // 编辑
        {"enter",       KEY_ENTER},       // 28
        {"delete",      KEY_DELETE},      // 111
        {"backspace",   KEY_BACKSPACE},   // 14
        {"space",       KEY_SPACE},       // 57
        {"tab",         KEY_TAB},         // 15
        {"escape",      KEY_ESC},         // 1
        // 前向删除就是 KEY_DELETE(111)，没有单独的键码 ——
        // 早先写的 KEY_FORWARD_DEL 在标准 <linux/input.h> 里不存在
        // （那是 Android 内核树里的写法），换系统编就炸。

        // 方向
        {"up",          KEY_UP},          // 103
        {"down",        KEY_DOWN},        // 108
        {"left",        KEY_LEFT},        // 105
        {"right",       KEY_RIGHT},       // 106
        {"center",      KEY_OK},          // 352

        // 媒体
        {"playpause",   KEY_PLAYPAUSE},   // 164
        {"nextsong",    KEY_NEXTSONG},    // 163
        {"previoussong",KEY_PREVIOUSSONG},// 165
        {"stop",        KEY_STOPCD},      // 166

        // 相机
        {"camera",      KEY_CAMERA},      // 212
        // 对焦键：Android 树里叫 KEY_FOCUS，标准 Linux 是 KEY_CAMERA_FOCUS(0x210)。
        // 直接用数值，免得编到别的头文件上又找不到。
        {"focus",       0x210},

        // 浏览器
        {"browser",     KEY_HOMEPAGE},    // 172
    };
    return kTable;
}

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

// ── 键名解析 ────────────────────────────────────────────────────────────────
bool Keyboard::ResolveKeyCode(const std::string& name, uint32_t* out) {
    if (name.empty() || out == nullptr) return false;

    std::string key = Lower(name);

    // ⚠️ 顺序很关键：**单字符先按字符解释，再考虑当数字键码**。
    //
    //    "1" 既可能是"数字键 1"（Linux KEY_1 = 2），也可能是"键码 1"（KEY_ESC）。
    //    先走数字解析的话 key "1" 会变成 ESC —— 实测就是这么错的，
    //    而调用方写 "1" 几乎总是想打字符 1。
    //    想要原始键码就写多位数（如 "172"）或带前缀。
    if (key.size() == 1) {
        const char c = key[0];
        if (c >= 'a' && c <= 'z') {
            *out = static_cast<uint32_t>(KEY_A + (c - 'a'));
            return true;
        }
        if (c >= '0' && c <= '9') {
            // KEY_1..KEY_9 = 2..10，KEY_0 = 11
            *out = (c == '0') ? KEY_0 : static_cast<uint32_t>(KEY_1 + (c - '1'));
            return true;
        }
    }

    // 带 key_ 前缀的（从内核文档抄来的写法）
    if (key.compare(0, 4, "key_") == 0) key = key.substr(4);

    // 查表
    auto it = KeyTable().find(key);
    if (it != KeyTable().end()) {
        *out = it->second;
        return true;
    }

    // f1-f12。
    //
    // ⚠️ 不能用 KEY_F1 + (n-1)：F1..F10 是 59..68，然后 **F11/F12 跳到 87/88**
    //    （中间是别的功能键）。实测 f12 会算成 70（那是 KEY_F10 后面不存在的位置）。
    if (key.size() >= 2 && key[0] == 'f') {
        static const uint32_t kF[] = {KEY_F1,  KEY_F2,  KEY_F3,  KEY_F4,
                                      KEY_F5,  KEY_F6,  KEY_F7,  KEY_F8,
                                      KEY_F9,  KEY_F10, KEY_F11, KEY_F12};
        const int n = atoi(key.c_str() + 1);
        if (n >= 1 && n <= 12) {
            *out = kF[n - 1];
            return true;
        }
    }

    // 多位纯数字：当原始键码。放在最后，避免和上面的字符解释冲突。
    {
        char* end = nullptr;
        const long v = strtol(key.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && key.size() > 1 &&
            v > 0 && v < 0x300) {
            *out = static_cast<uint32_t>(v);
            return true;
        }
    }

    return false;
}

const char* Keyboard::KnownKeyNames() {
    return "home back menu appswitch search power volumeup volumedown mute "
           "enter delete backspace space tab escape up down left right center "
           "playpause nextsong previoussong stop camera a-z 0-9 f1-f12，"
           "或直接给数字键码";
}

// ── 生命周期 ────────────────────────────────────────────────────────────────
Keyboard::~Keyboard() { Close(); }

const char* Keyboard::BackendName() const {
    return ready() ? "uinput(虚拟键盘)" : "未就绪";
}

bool Keyboard::Init(std::string* error) {
    Close();

    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) {
            *error = std::string("打开 /dev/uinput 失败: ") + strerror(errno) +
                     "（需要 root 或在 uhid 组里）";
        }
        return false;
    }

    // 只声明 EV_KEY 与 EV_SYN —— 键盘不需要别的。
    // 声明得越多，InputReader 越可能把它归类成复合设备。
    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0) {
        if (error) *error = std::string("UI_SET_EVBIT 失败: ") + strerror(errno);
        Close();
        return false;
    }

    // 声明要用的键位。
    //
    // ⚠️ 不声明就用不了：内核会丢弃未声明键位的事件，而且**不报错** ——
    //    症状是"按键发出去了一点反应都没有"，很难查。
    for (const auto& kv : KeyTable()) {
        if (ioctl(fd_, UI_SET_KEYBIT, kv.second) < 0) {
            ALOGW("UI_SET_KEYBIT(%u) 失败: %s", kv.second, strerror(errno));
        }
    }
    // 字母数字与功能键：整段声明，比逐个列省事且不会漏
    for (uint32_t c = KEY_ESC; c <= KEY_KPDOT; ++c) {
        ioctl(fd_, UI_SET_KEYBIT, c);
    }
    for (uint32_t c = KEY_F1; c <= KEY_F12; ++c) {
        ioctl(fd_, UI_SET_KEYBIT, c);
    }
    for (uint32_t c = KEY_HOME; c <= KEY_END; ++c) {
        ioctl(fd_, UI_SET_KEYBIT, c);
    }
    // Android 的四个系统键与电源/音量
    const uint32_t extra[] = {KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,
                              KEY_BACK, KEY_MENU, KEY_APPSELECT, KEY_SEARCH,
                              KEY_PLAYPAUSE, KEY_NEXTSONG, KEY_PREVIOUSSONG,
                              KEY_STOPCD, KEY_CAMERA, 0x210, KEY_OK,
                              KEY_HOMEPAGE};
    for (uint32_t c : extra) {
        ioctl(fd_, UI_SET_KEYBIT, c);
    }

    uinput_setup us{};
    const char* name = "autod-keyboard";
    strncpy(us.name, name, UINPUT_MAX_NAME_SIZE - 1);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x18d1;   // Google
    us.id.product = 0x0002;
    us.id.version = 1;

    if (ioctl(fd_, UI_DEV_SETUP, &us) < 0 ||
        ioctl(fd_, UI_DEV_CREATE) < 0) {
        if (error) *error = std::string("创建虚拟键盘失败: ") + strerror(errno);
        Close();
        return false;
    }

    // 等内核把设备注册好，否则刚创建就发的按键会丢
    usleep(kSettleUs);

    ALOGI("已创建虚拟键盘 \"%s\"", name);
    return true;
}

void Keyboard::Close() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
}

// ── 事件发送 ────────────────────────────────────────────────────────────────
bool Keyboard::Emit(uint16_t type, uint16_t code, int32_t value) {
    input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    // 时间戳留 0：让内核填当前时间。
    // 自己填 CLOCK_MONOTONIC 也可以，但 uinput 期望的是它的时间基，
    // 填错会让事件被当成"过去很久之前"而丢弃。
    ev.time.tv_sec  = 0;
    ev.time.tv_usec = 0;

    const ssize_t n = write(fd_, &ev, sizeof(ev));
    if (n != static_cast<ssize_t>(sizeof(ev))) {
        ALOGE("写键盘事件失败: %s", n < 0 ? strerror(errno) : "长度不符");
        return false;
    }
    return true;
}

bool Keyboard::Sync() {
    return Emit(EV_SYN, SYN_REPORT, 0);
}

bool Keyboard::Key(uint32_t linuxKeyCode, bool longPress, std::string* error) {
    if (!ready()) {
        if (error) *error = "键盘未就绪（Init 没成功）";
        return false;
    }
    if (linuxKeyCode == 0 || linuxKeyCode > KEY_MAX) {
        if (error) {
            *error = "键码 " + std::to_string(linuxKeyCode) +
                     " 超出范围（1.." + std::to_string(KEY_MAX) + "）";
        }
        return false;
    }

    // 一次完整按键：按下 → SYN → 抬起 → SYN。
    // 两个 SYN 都要有：只发一个的话按住状态不会结束，
    // 系统会认为这个键一直按着。
    if (!Emit(EV_KEY, static_cast<uint16_t>(linuxKeyCode), 1) || !Sync()) {
        if (error) *error = "发送按下事件失败";
        return false;
    }

    // 长按：保持按下更久。Android 的长按判定通常在 500ms 以上，
    // 而普通按键 50ms 就够（太短会被某些输入法当成抖动丢掉）。
    usleep(longPress ? 1000 * 1000 : 50 * 1000);

    if (!Emit(EV_KEY, static_cast<uint16_t>(linuxKeyCode), 0) || !Sync()) {
        if (error) *error = "发送抬起事件失败";
        return false;
    }

    ALOGI("按键注入: keycode=%u%s", linuxKeyCode, longPress ? "（长按）" : "");
    return true;
}

}  // namespace autod
