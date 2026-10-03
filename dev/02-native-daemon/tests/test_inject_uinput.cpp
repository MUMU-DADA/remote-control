// test_inject_uinput.cpp —— uinput 注入后端的真实设备验证
//
// 这个测试会在开发机上**真的创建一个虚拟触摸屏**，注入事件，
// 再从 /dev/input/eventN 读回来逐条校验。
//
// 不是 mock，走的是完整的内核 input 子系统路径。
//
// 编译运行:
//   make -C dev/02-native-daemon/tests
//   sudo ./dev/02-native-daemon/tests/test_inject_uinput
//
// 需要 root（/dev/uinput 权限）。

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include <string>
#include <vector>

#include "../daemon/inject.h"
#include "test_util.h"

namespace {

using remote_control_test::BitmapHasBit;
using remote_control_test::Check;
using remote_control_test::CountOf;
using remote_control_test::CountSyn;
using remote_control_test::DrainEvents;
using remote_control_test::DumpEvents;
using remote_control_test::ExtractBitmapWords;
using remote_control_test::FindEventNode;
using remote_control_test::LastValueOf;
using remote_control_test::ReadDeviceBlock;
using remote_control_test::WaitEvents;

int CountNamedDevices(const std::string& deviceName) {
    FILE* f = fopen("/proc/bus/input/devices", "r");
    if (!f) return -1;

    const std::string expected = "N: Name=\"" + deviceName + "\"";
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (std::string(line).rfind(expected, 0) == 0) ++count;
    }
    fclose(f);
    return count;
}

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

void TestTap(remote_control::Injector& inj, int readFd) {
    printf("\n\033[1;34m[1] 单击\033[0m  (540, 960)，持续 30ms\n");

    DrainEvents(readFd);   // 清空

    remote_control::TouchPoint p;
    p.id = 0;
    p.x  = 540;
    p.y  = 960;
    p.pressure = 1.0f;
    p.size     = 0.02f;

    std::string err;
    const bool ok = inj.Tap(p, 30, false, &err);
    Check(ok, "Tap() 返回成功%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());

    // 轮询等待，不用固定睡眠 —— 构建占满 CPU 时事件到达会推迟
    const auto evs = WaitEvents(readFd, 11, 3000);

    if (evs.empty()) {
        Check(false, "读回事件 —— 一个都没有（设备没被识别？）");
        return;
    }
    printf("    读回 %zu 个事件:\n", evs.size());
    DumpEvents(evs);

    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 1) == 1, "按下时 BTN_TOUCH=1 出现一次");
    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 0) == 1, "抬起时 BTN_TOUCH=0 出现一次");
    Check(CountSyn(evs) == 2, "有 2 个 SYN_REPORT（DOWN 一个、UP 一个）");

    // 注意：Check() 的实参求值顺序在 C++ 里是未指定的，
    // 所以不能把「调用 LastValueOf」和「读取被它写入的变量」
    // 写在同一个 Check() 调用里 —— 那样打印出来的是旧值。
    int32_t lastTracking = 999;
    const bool gotTracking =
            LastValueOf(evs, EV_ABS, ABS_MT_TRACKING_ID, &lastTracking);
    Check(gotTracking && lastTracking == -1,
          "ABS_MT_TRACKING_ID 最终为 -1（协议 B 的抬起标记），实际 %d",
          lastTracking);

    int32_t x = -1;
    const bool gotX = LastValueOf(evs, EV_ABS, ABS_MT_POSITION_X, &x);
    Check(gotX && x == 540, "ABS_MT_POSITION_X = 540，实际 %d", x);

    int32_t y = -1;
    const bool gotY = LastValueOf(evs, EV_ABS, ABS_MT_POSITION_Y, &y);
    Check(gotY && y == 960, "ABS_MT_POSITION_Y = 960，实际 %d", y);
}

void TestSwipe(remote_control::Injector& inj, int readFd) {
    printf("\n\033[1;34m[2] 滑动\033[0m  (200,1600) → (800,400)，200ms\n");

    DrainEvents(readFd);

    remote_control::TouchPoint from;
    from.id = 0; from.x = 200; from.y = 1600;
    remote_control::TouchPoint to;
    to.id = 0; to.x = 800; to.y = 400;

    std::string err;
    const bool ok = inj.Swipe(from, to, 200, 0, false, &err);
    Check(ok, "Swipe() 返回成功%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());

    auto evs = WaitEvents(readFd, 1, 3000);
    for (int i = 0; i < 20 && CountSyn(evs) < 4; ++i) {
        usleep(50000);
        auto more = DrainEvents(readFd);
        evs.insert(evs.end(), more.begin(), more.end());
    }

    const int moves = CountSyn(evs) - 2;   // 减去 DOWN/UP
    Check(moves >= 8, "产生了 %d 个 MOVE 事件（200ms 按 60Hz 应约 12 个）", moves);
    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 1) == 1, "BTN_TOUCH 按下一次");
    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 0) == 1, "BTN_TOUCH 抬起一次");

    // 坐标应单调从起点走到终点
    int32_t firstX = -1, lastX = -1;
    for (const auto& e : evs) {
        if (e.type == EV_ABS && e.code == ABS_MT_POSITION_X) {
            if (firstX < 0) firstX = e.value;
            lastX = e.value;
        }
    }
    Check(firstX == 200, "首个 X 坐标 = 200，实际 %d", firstX);
    Check(lastX == 800, "末个 X 坐标 = 800，实际 %d", lastX);
}

void TestMultiTouch(remote_control::Injector& inj, int readFd) {
    printf("\n\033[1;34m[3] 双指多点触控\033[0m  —— 验证协议 B 的槽位管理\n");

    DrainEvents(readFd);

    remote_control::TouchPoint f1;
    f1.id = 0; f1.x = 300; f1.y = 800;
    remote_control::TouchPoint f2;
    f2.id = 1; f2.x = 700; f2.y = 800;

    std::string err;
    bool ok = true;
    ok &= inj.TouchDown(f1, false, &err);
    ok &= inj.TouchDown(f2, false, &err);   // 第二根手指
    ok &= inj.TouchMove(f1, false, &err);
    ok &= inj.TouchMove(f2, false, &err);
    ok &= inj.TouchUp(f2, false, &err);
    ok &= inj.TouchUp(f1, false, &err);
    Check(ok, "多点触控序列执行成功%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());

    auto evs = WaitEvents(readFd, 1, 3000);
    for (int i = 0; i < 20 && CountSyn(evs) < 6; ++i) {
        usleep(50000);
        auto more = DrainEvents(readFd);
        evs.insert(evs.end(), more.begin(), more.end());
    }

    // 两个不同槽位应各自被使用过
    std::vector<int32_t> slots;
    for (const auto& e : evs) {
        if (e.type == EV_ABS && e.code == ABS_MT_SLOT) {
            if (slots.empty() || slots.back() != e.value) slots.push_back(e.value);
        }
    }
    Check(slots.size() >= 2, "使用了 %zu 个不同槽位（双指应为 2）", slots.size());

    // 两个不同的 tracking id
    std::vector<int32_t> ids;
    for (const auto& e : evs) {
        if (e.type == EV_ABS && e.code == ABS_MT_TRACKING_ID && e.value >= 0) {
            bool seen = false;
            for (int32_t v : ids) if (v == e.value) seen = true;
            if (!seen) ids.push_back(e.value);
        }
    }
    Check(ids.size() == 2, "分配了 %zu 个不同的 TRACKING_ID（双指应为 2）",
          ids.size());

    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 1) == 1,
          "BTN_TOUCH=1 只出现一次（第二根手指按下时不应重复置位）");
    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 0) == 1,
          "BTN_TOUCH=0 只出现一次（最后一根手指抬起后才清零）");
}

// 槽位耗尽：设备声明了 10 个槽位，第 11 个指针必须干净失败而不是静默丢弃
void TestSlotExhaustion(remote_control::Injector& inj, int readFd) {
    printf("\n\033[1;34m[4] 槽位耗尽\033[0m  超过 10 个指针时的行为\n");

    DrainEvents(readFd);

    std::string err;
    int succeeded = 0;
    bool failedCleanly = false;

    // 故意按下 12 个指针
    for (int i = 0; i < 12; ++i) {
        remote_control::TouchPoint p;
        p.id = i;
        p.x  = 100 + i * 10;
        p.y  = 200;

        err.clear();
        if (inj.TouchDown(p, false, &err)) {
            ++succeeded;
        } else {
            failedCleanly = true;
            Check(err.find("槽位耗尽") != std::string::npos,
                  "第 %d 个指针被拒绝，错误信息明确: %s", i + 1, err.c_str());
            break;
        }
    }

    Check(succeeded == 10, "成功按下 %d 个指针（设备声明了 10 个槽位）", succeeded);
    Check(failedCleanly, "第 11 个指针干净失败，没有静默丢弃");

    // 收尾：全部抬起，避免影响后续用例
    for (int i = 0; i < succeeded; ++i) {
        remote_control::TouchPoint p;
        p.id = i;
        p.x  = 100 + i * 10;
        p.y  = 200;
        inj.TouchUp(p, false, nullptr);
    }
    // 等事件落定再清空
    WaitEvents(readFd, 1, 2000);
    DrainEvents(readFd);

    // 槽位应已全部释放 —— 再按一个应该成功
    remote_control::TouchPoint after;
    after.id = 99; after.x = 500; after.y = 500;
    err.clear();
    const bool ok = inj.TouchDown(after, false, &err);
    Check(ok, "全部抬起后槽位已释放，能再按下新指针%s%s",
          ok ? "" : " —— ", ok ? "" : err.c_str());
    inj.TouchUp(after, false, nullptr);
}

void TestUnpairedUp(remote_control::Injector& inj) {
    printf("\n\033[1;34m[5] 异常路径\033[0m  没有配对的 UP 不应崩溃\n");

    remote_control::TouchPoint p;
    p.id = 42; p.x = 100; p.y = 100;

    std::string err;
    const bool ok = inj.TouchUp(p, false, &err);
    Check(ok, "孤立的 TouchUp 安全返回（不崩溃）");
}

// 验证内核记录的设备能力位 —— 这决定了 Android 会不会把它当触摸屏
void TestDeviceProperties(const std::string& deviceName) {
    printf("\n\033[1;34m[6] 设备能力位\033[0m  (/proc/bus/input/devices)\n");

    const std::string block = ReadDeviceBlock(deviceName);
    if (block.empty()) {
        Check(false, "在 /proc/bus/input/devices 里找不到 \"%s\"",
              deviceName.c_str());
        return;
    }
    printf("    内核记录:\n");
    for (size_t i = 0; i < block.size();) {
        const size_t nl = block.find('\n', i);
        if (nl == std::string::npos) break;
        printf("      %s\n", block.substr(i, nl - i).c_str());
        i = nl + 1;
    }

    // 注意：不能把「调用 ExtractBitmapWords」和「读取结果」写进同一个
    // Check() —— 实参求值顺序未指定，会打印出旧值。
    const auto propWords = ExtractBitmapWords(block, "PROP");
    Check(!propWords.empty() && (propWords.back() & 0x2),
          "PROP 含 INPUT_PROP_DIRECT（决定能否被识别为直接触摸屏），字=%zu",
          propWords.size());

    // 我们声明的 7 条 ABS 轴，逐个验证内核确实登记了
    const auto absWords = ExtractBitmapWords(block, "ABS");
    struct { int code; const char* name; } kExpectedAbs[] = {
        {ABS_MT_SLOT,         "ABS_MT_SLOT"},
        {ABS_MT_TOUCH_MAJOR,  "ABS_MT_TOUCH_MAJOR"},
        {ABS_MT_TOUCH_MINOR,  "ABS_MT_TOUCH_MINOR"},
        {ABS_MT_POSITION_X,   "ABS_MT_POSITION_X"},
        {ABS_MT_POSITION_Y,   "ABS_MT_POSITION_Y"},
        {ABS_MT_TRACKING_ID,  "ABS_MT_TRACKING_ID"},
        {ABS_MT_PRESSURE,     "ABS_MT_PRESSURE"},
    };
    for (const auto& e : kExpectedAbs) {
        Check(BitmapHasBit(absWords, e.code), "ABS 能力位含 %s (%d)",
              e.name, e.code);
    }

    const auto keyWords = ExtractBitmapWords(block, "KEY");
    Check(BitmapHasBit(keyWords, BTN_TOUCH),
          "KEY 能力位含 BTN_TOUCH (%d)", BTN_TOUCH);
}

}  // namespace

// ---------------------------------------------------------------------------

int main() {
    printf("\033[1m=== uinput 注入后端验证 ===\033[0m\n");
    printf("真实创建虚拟触摸屏并向内核注入事件，再从 /dev/input 读回校验。\n");

    if (geteuid() != 0) {
        fprintf(stderr, "\n\033[1;31m需要 root 权限\033[0m（/dev/uinput 默认 0600）\n");
        return 2;
    }

    const std::string kDeviceName = "remote-control-test-touch";
    const uint32_t kWidth  = 1080;
    const uint32_t kHeight = 1920;

    remote_control::InjectorConfig cfg;
    cfg.touchWidth  = kWidth;
    cfg.touchHeight = kHeight;
    cfg.deviceName  = kDeviceName.c_str();

    printf("\n\033[1;34m[0] 初始化\033[0m\n");
    remote_control::Injector injector;
    std::string err;
    if (!injector.Init(cfg, &err)) {
        fprintf(stderr, "  \033[1;31m✗\033[0m Init 失败: %s\n", err.c_str());
        return 1;
    }
    Check(true, "Init 成功，后端 = %s", injector.BackendName());

    const std::string node = FindEventNode(kDeviceName);
    if (node.empty()) {
        Check(false, "在 /sys/class/input/ 里找不到设备 \"%s\"",
              kDeviceName.c_str());
        return 1;
    }
    Check(true, "系统已枚举出设备节点 %s", node.c_str());

    const int readFd = open(node.c_str(), O_RDONLY | O_NONBLOCK);
    if (readFd < 0) {
        Check(false, "打开 %s 失败: %s", node.c_str(), strerror(errno));
        return 1;
    }
    Check(true, "已打开 %s 用于读回校验", node.c_str());

    TestTap(injector, readFd);
    TestSwipe(injector, readFd);
    TestMultiTouch(injector, readFd);
    TestSlotExhaustion(injector, readFd);
    TestUnpairedUp(injector);
    TestDeviceProperties(kDeviceName);

    close(readFd);

    printf("\n\033[1;34m[7] 后端重初始化\033[0m  设备热重建后释放旧 uinput 设备\n");
    Check(CountNamedDevices(kDeviceName) == 1,
          "初始化后恰有一个同名输入设备");
    cfg.touchWidth = kWidth + 1;
    const bool reinitialized = injector.Init(cfg, &err);
    Check(reinitialized, "更改坐标范围后重新初始化成功%s%s",
          reinitialized ? "" : ": ", reinitialized ? "" : err.c_str());
    if (reinitialized) {
        Check(CountNamedDevices(kDeviceName) == 1,
              "重新初始化后旧设备已销毁，只保留一个同名设备");
    }

    return remote_control_test::Summary("结果");
}
