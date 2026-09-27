// inject.h —— 触控注入（对外接口）
//
// 后端可替换：
//   inject_uinput.cpp  走 /dev/uinput（纯 Linux syscall，主机可测，Android 12 可用）
//   inject_binder.cpp  走 IInputManager::injectInputEvent（需要 AOSP 树，Android 12 上受限）
//
// 由 Android.bp 里的编译开关决定链接哪个，上层代码（main.cpp / socket_server）无感知。
//
// 背景：Android 12 的 IInputManager 是 Java-only AIDL，native 进程调不了。
//       详见 docs/06-constraints.md 约束 1。

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace autod {

// 动作常量。
//
// 数值对齐 android.view.MotionEvent，但在这里自己定义 —— 这样本头文件
// 不依赖任何 Android SDK 头文件，主机上的单元测试也能编译。
// 各后端负责翻译成平台常量。
constexpr int32_t kActionDown        = 0;
constexpr int32_t kActionUp          = 1;
constexpr int32_t kActionMove        = 2;
constexpr int32_t kActionCancel      = 3;
constexpr int32_t kActionPointerDown = 5;
constexpr int32_t kActionPointerUp   = 6;

// 单个触控点
struct TouchPoint {
    int32_t id       = 0;      // 指针 ID，多点触控时区分手指
    int32_t x        = 0;
    int32_t y        = 0;
    float   pressure = 1.0f;   // 0.0 ~ 1.0
    float   size     = 0.02f;  // 相对屏幕短边的接触面积
};

// 后端初始化参数
struct InjectorConfig {
    // 触控坐标范围。uinput 后端用它设置 ABS 轴范围；
    // 填 0 则用 32767（很多触控控制器的约定值，Android 会自动缩放到屏幕）。
    uint32_t touchWidth  = 0;
    uint32_t touchHeight = 0;

    // 虚拟设备名。uinput 后端用它注册输入设备；binder 后端忽略。
    const char* deviceName = "autod-touch";

    // 目标显示 ID。binder 后端用它；uinput 后端交给 Android 自行关联。
    int32_t displayId = 0;
};

class InjectorBackend;   // 定义在 inject_backend.h

class Injector {
  public:
    Injector();
    ~Injector();

    Injector(const Injector&)            = delete;
    Injector& operator=(const Injector&) = delete;

    bool Init(const InjectorConfig& config, std::string* error);

    // 单击：DOWN → 等待 durationMs → UP
    bool Tap(const TouchPoint& point, uint32_t durationMs, bool async,
             std::string* error);

    // 滑动：按下起点，按直线插值移动，抬起终点。
    // steps 为 0 时按 durationMs 自动计算（约 60Hz）。
    bool Swipe(const TouchPoint& from, const TouchPoint& to,
               uint32_t durationMs, uint32_t steps, bool async,
               std::string* error);

    // 手动多点触控序列。调用方负责配对 down/move/up。
    // ── 常见手势 ────────────────────────────────────────────────────────────
    //
    // 这三个不是 Tap/Swipe 的语法糖，它们的时序有硬要求：
    //   长按：按下后**不能发 MOVE**，否则系统判成拖拽，长按菜单不弹。
    //   拖拽：起点要先停顿再移动，否则会被判成滑动（fling）。
    //   双击：两次点击的间隔要落在系统阈值内。
    // 让调用方自己拼 Touch* 序列几乎一定会踩这些坑，所以内置。

    // 长按。durationMs 默认 800（Android 的长按阈值约 500ms）
    bool LongPress(const TouchPoint& point, uint32_t durationMs, bool async,
                   std::string* error);

    // 拖拽。与 Swipe 的区别是**起点有停顿、移动更慢**，
    // 这样才会被识别成"按住拖动"而不是"甩一下"。
    bool Drag(const TouchPoint& from, const TouchPoint& to, uint32_t durationMs,
              bool async, std::string* error);

    // 双击。intervalMs 是两次点击之间的间隔，默认 120ms
    // （系统双击阈值约 300ms，留足余量但也不能太慢）。
    bool DoubleTap(const TouchPoint& point, uint32_t intervalMs, bool async,
                   std::string* error);

    bool TouchDown(const TouchPoint& point, bool async, std::string* error);
    bool TouchMove(const TouchPoint& point, bool async, std::string* error);
    bool TouchUp(const TouchPoint& point, bool async, std::string* error);

    // 后端名，用于日志与诊断
    const char* BackendName() const;

  private:
    // 把「动作 + 触控点 + 时间戳」交给后端。
    // downTime 由 SendSingle 统一维护，后端通过 Emit() 拿到。
    bool SendSingle(int32_t action, const TouchPoint& point,
                    int64_t eventTimeNs, bool async, std::string* error);

    std::unique_ptr<InjectorBackend> backend_;
    int64_t downTimeNs_ = 0;           // 当前手势起始时间；0 表示无进行中手势
};

}  // namespace autod
