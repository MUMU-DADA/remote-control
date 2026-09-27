// keyboard.h — 按键注入
//
// Android 12 没有可用的 native 按键注入接口（IInputManager 是 Java-only，
// IInputFlinger 没有注入方法）—— 和触控同样的结论，所以同样走 /dev/uinput，
// 建一个虚拟键盘设备。
//
// 为什么不复用 Injector：那是个触控设备（INPUT_PROP_DIRECT、多点触控槽位），
// 键盘是另一类设备（EV_KEY 位图完全不同）。混在一个 uinput fd 上，
// InputReader 会把它当成"既能触摸又能打字的怪东西"，
// 而 .idc 里 touch.deviceType 与 keyboard 的判定会互相干扰。
// 分成两个设备更干净，也更接近真机（真机的触摸屏和键盘本就是两个节点）。

#pragma once

#include <cstdint>
#include <string>

namespace autod {

class Keyboard {
  public:
    Keyboard() = default;
    ~Keyboard();

    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    // 创建虚拟键盘设备。失败时 error 里说明原因（多半是 /dev/uinput 权限）。
    bool Init(std::string* error);

    void Close();
    bool ready() const { return fd_ >= 0; }
    const char* BackendName() const;

    // 按一次键。
    //
    // longPress=true 时保持按下更久（默认 100ms → 1000ms），
    // 用于需要长按的键（如电源键菜单）。
    bool Key(uint32_t linuxKeyCode, bool longPress, std::string* error);

    // 把 "home" / "KEY_HOME" / "172" 解析成 Linux 键码。
    //
    // 接受三种写法，是因为调用方来自不同地方：人类写名字、
    // 从内核文档抄来的是 KEY_ 前缀、程序里存的可能直接是数字。
    static bool ResolveKeyCode(const std::string& name, uint32_t* out);

    // 已知键名的列表（供 Describe 展示）
    static const char* KnownKeyNames();

  private:
    bool Emit(uint16_t type, uint16_t code, int32_t value);
    bool Sync();

    int fd_ = -1;
};

}  // namespace autod
