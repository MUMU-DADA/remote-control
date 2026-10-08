// test_mouse.cpp — 鼠标后端测试（真实 /dev/uinput，不是 mock）
//
// 覆盖方式与 test_inject_uinput.cpp 一致：**真的建一个虚拟鼠标设备，
// 真的把事件发下去，再从 /dev/input/eventN 读回来逐条核对**。
// 只调 API 看返回值是查不出方向写反、值被覆盖这类问题的 ——
// 而那正是这一层最容易出的错。
//
// 需要 root（/dev/uinput 默认 0600）与已加载的 uinput 模块：
//     sudo make test_mouse && sudo ./test_mouse

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "mouse.h"
#include "test_util.h"

using namespace remote_control;
using namespace remote_control_test;

namespace {

constexpr const char* kDeviceName = "remote-control-mouse";

// 把一组事件里某个 (type, code) 的 value 求和。
// REL 是相对量，"发了多少"要看总和，不是看条数。
int SumOf(const std::vector<input_event>& evs, uint16_t type, uint16_t code) {
    int sum = 0;
    for (const auto& e : evs) {
        if (e.type == type && e.code == code) sum += e.value;
    }
    return sum;
}

// 数 SYN_REPORT 的个数 = 这批事件被拆成了几"帧"。
int SyncCount(const std::vector<input_event>& evs) {
    int n = 0;
    for (const auto& e : evs) {
        if (e.type == EV_SYN && e.code == SYN_REPORT) ++n;
    }
    return n;
}

// ── [1] 按钮名解析（纯逻辑，不需要设备）────────────────────────────────────
void TestButtonNames() {
    printf("\n\033[1;34m[1] 按钮名解析\033[0m\n");

    struct Case { const char* name; bool ok; uint16_t code; };
    const Case cases[] = {
        {"left",    true,  BTN_LEFT},
        {"LEFT",    true,  BTN_LEFT},     // 大小写不敏感
        {"primary", true,  BTN_LEFT},
        {"1",       true,  BTN_LEFT},
        {"right",   true,  BTN_RIGHT},
        {"secondary", true, BTN_RIGHT},
        {"middle",  true,  BTN_MIDDLE},
        {"wheel",   true,  BTN_MIDDLE},   // 中键就是滚轮键
        {"back",    true,  BTN_SIDE},
        {"forward", true,  BTN_EXTRA},
        {"",        false, 0},
        {"leftt",   false, 0},
        {"btn",     false, 0},
    };
    for (const auto& c : cases) {
        uint16_t got = 0;
        const bool ok = Mouse::ResolveButton(c.name, &got);
        Check(ok == c.ok && (!ok || got == c.code), "%-10s → %s",
              c.name[0] ? c.name : "(空)",
              ok ? std::to_string(got).c_str() : "拒绝");
    }
}

}  // namespace

int main() {
    printf("\033[1m=== remote-control 鼠标后端测试 ===\033[0m\n");

    TestButtonNames();

    // ── [2] 建设备 ────────────────────────────────────────────────────────
    printf("\n\033[1;34m[2] 虚拟鼠标设备\033[0m\n");

    if (geteuid() != 0) {
        printf("\n\033[1;31m需要 root（/dev/uinput 默认 0600）。用 sudo 跑。\033[0m\n");
        return 1;
    }

    Mouse mouse;
    std::string err;
    const bool inited = mouse.Init(&err);
    Check(inited, "创建虚拟鼠标%s%s", inited ? "" : " —— ", inited ? "" : err.c_str());
    if (!inited) return Summary("鼠标后端");

    Check(mouse.ready(), "ready() = true");
    Check(Mouse::AnyReady(), "进程级 AnyReady() = true");
    Check(strcmp(Mouse::BackendName(), "uinput-mouse") == 0,
          "BackendName = %s", Mouse::BackendName());

    // 内核视角：设备确实注册了，且能力位是鼠标该有的样子
    const std::string block = ReadDeviceBlock(kDeviceName);
    Check(!block.empty(), "内核里出现 \"%s\"", kDeviceName);

    const auto propBits = ExtractBitmapWords(block, "PROP");
    Check(BitmapHasBit(propBits, INPUT_PROP_POINTER),
          "PROP 含 INPUT_PROP_POINTER（不设就没有光标）");

    const auto relBits = ExtractBitmapWords(block, "REL");
    Check(BitmapHasBit(relBits, REL_X), "REL 含 REL_X");
    Check(BitmapHasBit(relBits, REL_Y), "REL 含 REL_Y");
    Check(BitmapHasBit(relBits, REL_WHEEL), "REL 含 REL_WHEEL");
    Check(BitmapHasBit(relBits, REL_HWHEEL), "REL 含 REL_HWHEEL");

    const auto keyBits = ExtractBitmapWords(block, "KEY");
    Check(BitmapHasBit(keyBits, BTN_LEFT), "KEY 含 BTN_LEFT");
    Check(BitmapHasBit(keyBits, BTN_RIGHT), "KEY 含 BTN_RIGHT");
    Check(BitmapHasBit(keyBits, BTN_MIDDLE), "KEY 含 BTN_MIDDLE");

    // 打开事件节点准备读回
    const std::string node = FindEventNode(kDeviceName);
    Check(!node.empty(), "找到事件节点 %s", node.empty() ? "(无)" : node.c_str());
    if (node.empty()) return Summary("鼠标后端");

    const int fd = open(node.c_str(), O_RDONLY | O_NONBLOCK);
    Check(fd >= 0, "打开 %s", node.c_str());
    if (fd < 0) return Summary("鼠标后端");

    // ── [3] 移动 ──────────────────────────────────────────────────────────
    printf("\n\033[1;34m[3] 相对移动\033[0m\n");
    DrainEvents(fd);

    Check(mouse.Move(100, 50, &err), "Move(100, 50)%s%s", err.empty() ? "" : " —— ",
          err.c_str());
    auto evs = WaitEvents(fd, 6, 1500);
    Check(SumOf(evs, EV_REL, REL_X) == 100, "REL_X 合计 = %d（期望 100）",
          SumOf(evs, EV_REL, REL_X));
    Check(SumOf(evs, EV_REL, REL_Y) == 50, "REL_Y 合计 = %d（期望 50）",
          SumOf(evs, EV_REL, REL_Y));
    // ⚠️ InputReader 的累加器是**赋值**不是累加：同一个 SYN 里发两次 REL_X，
    //    只有最后一次生效。所以必须拆成多帧 —— 这条断言就是在钉住它。
    const int syncs = SyncCount(evs);
    Check(syncs >= 2, "拆成了 %d 帧（>1 才能真的走满 100 像素）", syncs);

    // 负方向
    DrainEvents(fd);
    Check(mouse.Move(-64, -64, &err), "Move(-64, -64)");
    evs = WaitEvents(fd, 4, 1500);
    Check(SumOf(evs, EV_REL, REL_X) == -64, "REL_X 合计 = %d（期望 -64）",
          SumOf(evs, EV_REL, REL_X));
    Check(SumOf(evs, EV_REL, REL_Y) == -64, "REL_Y 合计 = %d（期望 -64）",
          SumOf(evs, EV_REL, REL_Y));
    // 符号不能被打散：全负才说明方向一致，出现正数就是把负数拆坏了
    int positives = 0;
    for (const auto& e : evs) {
        if (e.type == EV_REL && e.value > 0) ++positives;
    }
    Check(positives == 0, "负向移动里没有出现正值事件（%d 个）", positives);

    // 拆步上限：一次 1000 像素应被拆成 ceil(1000/32) = 32 帧
    DrainEvents(fd);
    Check(mouse.Move(1000, 0, &err), "Move(1000, 0)");
    evs = WaitEvents(fd, 64, 2000);
    Check(SumOf(evs, EV_REL, REL_X) == 1000, "REL_X 合计 = %d（期望 1000）",
          SumOf(evs, EV_REL, REL_X));
    Check(SyncCount(evs) == 32, "拆成 %d 帧（期望 32 = ceil(1000/32)）",
          SyncCount(evs));

    // Move(0,0) 不该发任何事件
    DrainEvents(fd);
    Check(mouse.Move(0, 0, &err), "Move(0, 0) 返回成功");
    evs = WaitEvents(fd, 1, 300);
    Check(evs.empty(), "Move(0, 0) 不发事件（收到 %zu 个）", evs.size());

    // ── [4] 滚轮 ──────────────────────────────────────────────────────────
    printf("\n\033[1;34m[4] 滚轮\033[0m\n");
    DrainEvents(fd);
    Check(mouse.Wheel(3, false, &err), "Wheel(+3, 垂直)");
    evs = WaitEvents(fd, 6, 1500);
    Check(CountOf(evs, EV_REL, REL_WHEEL, 1) == 3, "3 次 REL_WHEEL=+1（得到 %d）",
          CountOf(evs, EV_REL, REL_WHEEL, 1));

    DrainEvents(fd);
    Check(mouse.Wheel(-2, false, &err), "Wheel(-2, 垂直)");
    evs = WaitEvents(fd, 4, 1500);
    Check(CountOf(evs, EV_REL, REL_WHEEL, -1) == 2, "2 次 REL_WHEEL=-1（得到 %d）",
          CountOf(evs, EV_REL, REL_WHEEL, -1));

    DrainEvents(fd);
    Check(mouse.Wheel(2, true, &err), "Wheel(+2, 水平)");
    evs = WaitEvents(fd, 4, 1500);
    Check(CountOf(evs, EV_REL, REL_HWHEEL, 1) == 2, "2 次 REL_HWHEEL=+1（得到 %d）",
          CountOf(evs, EV_REL, REL_HWHEEL, 1));
    Check(SumOf(evs, EV_REL, REL_WHEEL) == 0, "水平滚动不碰 REL_WHEEL");

    DrainEvents(fd);
    Check(mouse.Wheel(0, false, &err), "Wheel(0) 返回成功");
    evs = WaitEvents(fd, 1, 300);
    Check(evs.empty(), "Wheel(0) 不发事件（收到 %zu 个）", evs.size());

    // ── [5] 按键 ──────────────────────────────────────────────────────────
    printf("\n\033[1;34m[5] 按键\033[0m\n");
    DrainEvents(fd);
    Check(mouse.Button("left", true, &err), "按下左键");
    evs = WaitEvents(fd, 2, 1500);
    Check(CountOf(evs, EV_KEY, BTN_LEFT, 1) == 1, "BTN_LEFT 按下事件");
    Check(CountOf(evs, EV_KEY, BTN_LEFT, 0) == 0, "此时还没有抬起");

    DrainEvents(fd);
    Check(mouse.Button("left", false, &err), "抬起左键");
    evs = WaitEvents(fd, 2, 1500);
    Check(CountOf(evs, EV_KEY, BTN_LEFT, 0) == 1, "BTN_LEFT 抬起事件");

    DrainEvents(fd);
    Check(mouse.Click("right", &err), "点击右键");
    evs = WaitEvents(fd, 4, 1500);
    Check(CountOf(evs, EV_KEY, BTN_RIGHT, 1) == 1 &&
          CountOf(evs, EV_KEY, BTN_RIGHT, 0) == 1,
          "一次点击 = 按下 + 抬起各一次（%d/%d）",
          CountOf(evs, EV_KEY, BTN_RIGHT, 1), CountOf(evs, EV_KEY, BTN_RIGHT, 0));

    // 非法按钮名必须被拒，而不是静默当成左键
    DrainEvents(fd);
    Check(!mouse.Button("nosuchbutton", true, &err), "未知按钮被拒绝");
    Check(!err.empty(), "拒绝时给出原因：%s", err.c_str());
    evs = WaitEvents(fd, 1, 300);
    Check(evs.empty(), "被拒绝的按键不发任何事件（收到 %zu 个）", evs.size());

    close(fd);
    mouse.Close();
    Check(!Mouse::AnyReady(), "Close() 之后 AnyReady() = false");
    Check(ReadDeviceBlock(kDeviceName).empty(), "Close() 之后内核里也没有了");

    return Summary("鼠标后端");
}
