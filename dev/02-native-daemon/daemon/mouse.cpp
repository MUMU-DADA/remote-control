// mouse.cpp — uinput 虚拟鼠标

#include "mouse.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "remote_control_log.h"

namespace remote_control {
namespace {

// 进程里是否已经建起虚拟鼠标。理由同 keyboard.cpp：
// 设备是延迟创建的，而读它的可能是另一条线程。
std::atomic<bool> gMouseReady{false};

// 设备创建后内核需要一点时间注册，太快发事件会丢
constexpr int kSettleUs = 100 * 1000;

// 按键保持时间（Click 用）。
//
// 太短会有应用收不到（InputReader 的批量采样可能把按下与抬起并进同一个
// 采样周期，于是只有一个"瞬移"而没有 click）。30ms 是实测能稳定触发的最短档。
constexpr int kClickHoldUs = 30 * 1000;

// 单次 SYN 里最多报多少像素。
//
// ⚠️ 这不是"性能优化"，是**接口语义**：InputReader 的
//    CursorMotionAccumulator::process() 是 `mRelX = rawEvent->value;` ——
//    **赋值，不是累加**。所以同一个 SYN 里发 10 次 REL_X=5，光标只走 5 像素，
//    不是 50。想走 50 就得发 10 组 (REL_X, SYN)。
//
//    单步给太大也不行：部分设备/版本对单次 REL 有上限，且大跳变会被
//    指针加速曲线放大得不成比例。32 px 一步是稳妥值。
constexpr int kMaxStepPx = 32;

}  // namespace

// ── 按钮名解析 ──────────────────────────────────────────────────────────────
bool Mouse::ResolveButton(const std::string& name, uint16_t* out) {
    if (name.empty() || out == nullptr) return false;
    std::string k;
    for (char c : name) k.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));

    if (k == "left"   || k == "1" || k == "btn_left"   || k == "primary")   { *out = BTN_LEFT;   return true; }
    if (k == "right"  || k == "2" || k == "btn_right"  || k == "secondary") { *out = BTN_RIGHT;  return true; }
    if (k == "middle" || k == "3" || k == "btn_middle" || k == "wheel")     { *out = BTN_MIDDLE; return true; }
    // 侧键：不少鼠标有，Android 也认（BACK/FORWARD）
    if (k == "back"   || k == "8" || k == "btn_side")                       { *out = BTN_SIDE;   return true; }
    if (k == "forward"|| k == "9" || k == "btn_extra")                      { *out = BTN_EXTRA;  return true; }
    return false;
}

const char* Mouse::KnownButtonNames() {
    return "left|right|middle|back|forward";
}

const char* Mouse::BackendName() { return "uinput-mouse"; }
bool Mouse::AnyReady() { return gMouseReady.load(); }

// ── 设备创建 ────────────────────────────────────────────────────────────────
bool Mouse::Init(std::string* error) {
    if (fd_ >= 0) return true;

    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) {
            *error = std::string("open(\"/dev/uinput\") 失败: ") + strerror(errno) +
                     "（需要 root 或 uhid 组；SELinux 也要放行 uhid_device）";
        }
        return false;
    }

    // 只声明这三类事件 —— 鼠标需要的全部。
    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0) {
        if (error) *error = std::string("UI_SET_EVBIT 失败: ") + strerror(errno);
        Close();
        return false;
    }

    // 按钮。和键盘一样：不声明就用不了，而且内核**不报错**，只是静默丢弃。
    const uint16_t buttons[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
    for (uint16_t b : buttons) {
        if (ioctl(fd_, UI_SET_KEYBIT, b) < 0) {
            ALOGW("UI_SET_KEYBIT(%u) 失败: %s", b, strerror(errno));
        }
    }

    // 相对轴。
    const uint16_t rels[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL};
    for (uint16_t r : rels) {
        if (ioctl(fd_, UI_SET_RELBIT, r) < 0) {
            if (error) *error = std::string("UI_SET_RELBIT(") + std::to_string(r) +
                                ") 失败: " + strerror(errno);
            Close();
            return false;
        }
    }

    // INPUT_PROP_POINTER：告诉 InputReader"这是个指点设备"。
    // 不设的话它可能被当成轨迹球/触摸板，指针控制器（光标）不会出现。
    if (ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_POINTER) < 0) {
        ALOGW("UI_SET_PROPBIT(INPUT_PROP_POINTER) 失败: %s —— 光标可能不会显示",
              strerror(errno));
    }

    uinput_setup us{};
    const char* name = "remote-control-mouse";
    strncpy(us.name, name, UINPUT_MAX_NAME_SIZE - 1);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x18d1;   // Google
    us.id.product = 0x0003;
    us.id.version = 1;

    if (ioctl(fd_, UI_DEV_SETUP, &us) < 0 ||
        ioctl(fd_, UI_DEV_CREATE) < 0) {
        if (error) *error = std::string("创建虚拟鼠标失败: ") + strerror(errno);
        Close();
        return false;
    }

    usleep(kSettleUs);

    ALOGI("已创建虚拟鼠标 \"%s\"", name);
    gMouseReady.store(true);
    return true;
}

void Mouse::Close() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
    gMouseReady.store(false);
}

Mouse::~Mouse() { Close(); }

// ── 事件发送 ────────────────────────────────────────────────────────────────
bool Mouse::Emit(uint16_t type, uint16_t code, int32_t value) {
    input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    // 时间戳留 0 让内核填 —— 理由同 keyboard.cpp
    const ssize_t n = write(fd_, &ev, sizeof(ev));
    if (n != static_cast<ssize_t>(sizeof(ev))) {
        ALOGE("写鼠标事件失败: %s", n < 0 ? strerror(errno) : "长度不符");
        return false;
    }
    return true;
}

bool Mouse::Sync() { return Emit(EV_SYN, SYN_REPORT, 0); }

// ── Move ────────────────────────────────────────────────────────────────────
bool Mouse::Move(int dx, int dy, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "虚拟鼠标未创建（先调用 Init）";
        return false;
    }
    // 拆步的理由见 kMaxStepPx 的注释：同一个 SYN 里的 REL 是**覆盖**不是累加。
    int remainingX = dx;
    int remainingY = dy;
    while (remainingX != 0 || remainingY != 0) {
        const int stepX = std::max(-kMaxStepPx, std::min(kMaxStepPx, remainingX));
        const int stepY = std::max(-kMaxStepPx, std::min(kMaxStepPx, remainingY));
        if (stepX != 0 && !Emit(EV_REL, REL_X, stepX)) {
            if (error) *error = "写 REL_X 失败";
            return false;
        }
        if (stepY != 0 && !Emit(EV_REL, REL_Y, stepY)) {
            if (error) *error = "写 REL_Y 失败";
            return false;
        }
        if (!Sync()) {
            if (error) *error = "写 SYN_REPORT 失败";
            return false;
        }
        remainingX -= stepX;
        remainingY -= stepY;
    }
    return true;
}

// ── Button ──────────────────────────────────────────────────────────────────
bool Mouse::Button(const std::string& name, bool down, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "虚拟鼠标未创建（先调用 Init）";
        return false;
    }
    uint16_t code = 0;
    if (!ResolveButton(name, &code)) {
        if (error) *error = "未知按钮: " + name + "（可用: " + KnownButtonNames() + "）";
        return false;
    }
    if (!Emit(EV_KEY, code, down ? 1 : 0) || !Sync()) {
        if (error) *error = "写按钮事件失败";
        return false;
    }
    return true;
}

// ── Click ───────────────────────────────────────────────────────────────────
bool Mouse::Click(const std::string& name, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "虚拟鼠标未创建（先调用 Init）";
        return false;
    }
    uint16_t code = 0;
    if (!ResolveButton(name, &code)) {
        if (error) *error = "未知按钮: " + name + "（可用: " + KnownButtonNames() + "）";
        return false;
    }
    // 按下与抬起要落在**不同的采样周期**里，否则应用只看到状态没变。
    if (!Emit(EV_KEY, code, 1) || !Sync()) {
        if (error) *error = "写按钮按下失败";
        return false;
    }
    usleep(kClickHoldUs);
    if (!Emit(EV_KEY, code, 0) || !Sync()) {
        if (error) *error = "写按钮抬起失败";
        return false;
    }
    return true;
}

// ── Wheel ───────────────────────────────────────────────────────────────────
bool Mouse::Wheel(int delta, bool horizontal, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "虚拟鼠标未创建（先调用 Init）";
        return false;
    }
    if (delta == 0) return true;
    // 一次报一格：REL_WHEEL 的值是"格数"，但一次报太多会被应用当成
    // 一次巨大的滚动，滚动动画直接跳到底。逐格发更接近真实滚轮。
    const int step = delta > 0 ? 1 : -1;
    for (int i = 0; i != delta; i += step) {
        if (!Emit(EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, step) || !Sync()) {
            if (error) *error = "写滚轮事件失败";
            return false;
        }
    }
    return true;
}

}  // namespace remote_control
