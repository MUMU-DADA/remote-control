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

namespace remote_control {

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

// ── 压力 / 接触面积在滑动过程中的变化形状 ──────────────────────────────────
//
// 起点与终点的值由两个 TouchPoint 给出（Swipe/Drag 一直支持线性插值），
// 这里定义的是**中间怎么过渡**。
//
// 已经能用线性表达"从轻到重"，所以真正需要这个枚举的是另外两种情况：
//   · kUpDown —— 笔画：两头轻、中间重（画板、手写）
//   · kEaseIn / kEaseOut —— 起手或收手更陡，模拟真实按压的物理感
enum class CurveShape {
    kLinear  = 0,   // P(t) = P0 + (P1-P0)·t
    kEaseIn  = 1,   // 慢起：因子 t²
    kEaseOut = 2,   // 快起：因子 1-(1-t)²
    kUpDown  = 3,   // 中间重：两头取 min(P0,P1)，中间到 max(P0,P1)
};

// 按曲线算 t 时刻的 pressure / size。
//
// ⚠️ kUpDown 的语义与另外三个**不一样**：它不是"在 P0 与 P1 之间插值"，
//    而是"两头一样轻、中间重"—— 端值取 min(P0,P1)，峰值取 max(P0,P1)。
//    笔画本来就是这个形状；想表达"越来越重"请用 kLinear。
//    这一点写进文档了，因为猜错的代价是"压力曲线看起来没生效"。
void ApplyCurve(CurveShape shape, float t, float p0, float p1, float s0, float s1,
                float* outPressure, float* outSize);

// 解析 "linear" / "ease-in" / "ease-out" / "updown"，并回写规范名。
bool ParseCurve(const std::string& name, CurveShape* out);
const char* CurveName(CurveShape shape);

// 后端初始化参数
struct InjectorConfig {
    // 触控坐标范围。uinput 后端用它设置 ABS 轴范围；
    // 填 0 则用 32767（很多触控控制器的约定值，Android 会自动缩放到屏幕）。
    uint32_t touchWidth  = 0;
    uint32_t touchHeight = 0;

    // 虚拟设备名。uinput 后端用它注册输入设备；binder 后端忽略。
    const char* deviceName = "remote-control-touch";

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
               std::string* error, CurveShape curve = CurveShape::kLinear);

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
              bool async, std::string* error,
              CurveShape curve = CurveShape::kLinear);

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

}  // namespace remote_control
