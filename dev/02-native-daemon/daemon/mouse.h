// mouse.h — 鼠标 / 滚轮注入
//
// 为什么单独一个设备：和键盘同样的理由（见 keyboard.h）——
// 触摸屏、键盘、鼠标在内核里本就是三类设备，EV_* 位图完全不同。
// 混在触控设备上，InputReader 会把它归类成复合设备，
// CursorInputMapper 与 TouchInputMapper 的判定互相干扰。
//
// ── 方向约定：**照抄真实鼠标，不做任何取反** ──────────────────────────────
//
//   REL_X  正 = 光标右移
//   REL_Y  正 = 光标下移
//   REL_WHEEL  正 = 滚轮向上（远离使用者）
//   REL_HWHEEL 正 = 滚轮向右
//
// 依据是本机的 InputReader 源码，不是推测：
//   CursorMotionAccumulator::process()  直接取 rawEvent->value，不取反
//   CursorInputMapper::sync()           MODE_POINTER 下 mYScale = 1.0，
//                                       结果交给 PointerController::move()
//                                       （屏幕坐标系，+y 向下）
//   CursorScrollAccumulator::process()  同样直接取 value
//
// 之所以要专门查一遍：这一层历史上最容易靠"我记得要取反"写出方向反了的
// 实现，而症状是"能用但左右上下颠倒"，很难一眼看出来。
//
// ⚠️ 只有相对移动，没有绝对定位。Android 的鼠标设备本身就只有 REL_*，
//    要"移动到 (x,y)"得先知道光标当前在哪，而接口没有这个信息 ——
//    不假装有。需要精确落点时用触控（Tap）而不是鼠标。

#pragma once

#include <cstdint>
#include <string>

namespace remote_control {

class Mouse {
  public:
    Mouse() = default;
    ~Mouse();

    Mouse(const Mouse&) = delete;
    Mouse& operator=(const Mouse&) = delete;

    // 创建虚拟鼠标设备。失败时 error 里说明原因（多半是 /dev/uinput 权限）。
    bool Init(std::string* error);

    void Close();
    bool ready() const { return fd_ >= 0; }

    // 后端名 / 进程里是否已经建起虚拟鼠标。
    // 与 Keyboard 同样：设备是**延迟创建**的，所以进程级状态就是答案。
    static const char* BackendName();
    static bool AnyReady();

    // 相对移动。dx > 0 向右，dy > 0 向下。
    // 单次给很大的值没有意义（InputReader 每个 SYN 只取最后一个 REL 值），
    // 想移动得远就多调几次 —— 内部会按需拆成多步。
    bool Move(int dx, int dy, std::string* error);

    // 按下 / 抬起。name: left | right | middle
    bool Button(const std::string& name, bool down, std::string* error);

    // 点一下（按下 → 短暂保持 → 抬起）。只按不抬会粘住，所以单独给一个。
    bool Click(const std::string& name, std::string* error);

    // 滚轮。delta > 0 向上（horizontal 时为向右）。
    // 一个"格"就是 1，和真实鼠标一致。
    bool Wheel(int delta, bool horizontal, std::string* error);

    // 按钮名 → BTN_* 键码。接受 left/right/middle 与 btn_left/1 这类写法。
    static bool ResolveButton(const std::string& name, uint16_t* out);

    // 已知按钮名（供 Describe 展示）
    static const char* KnownButtonNames();

  private:
    bool Emit(uint16_t type, uint16_t code, int32_t value);
    bool Sync();

    int fd_ = -1;
};

}  // namespace remote_control
