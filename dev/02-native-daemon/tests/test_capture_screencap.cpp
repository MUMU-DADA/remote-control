// test_capture_screencap.cpp —— screencap 截图后端的验证
//
// 用 tests/fake_screencap 冒充 /system/bin/screencap，
// 在开发机上验证 capture_screencap.cpp 的整条路径：
//
//   fork/exec → 头部解析 → 尺寸计算 → memfd + mmap → 像素校验
//
// 也覆盖异常路径：未知像素格式、输出被截断、子进程非零退出。
//
// 运行:
//   make test_capture_screencap && ./test_capture_screencap ./fake_screencap

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <atomic>
#include <thread>
#include <vector>

#include "../daemon/capture.h"
#include "test_util.h"

using namespace remote_control;
using namespace remote_control_test;

namespace {

const char* gFakePath = "./fake_screencap";

// 每个用例前重置环境变量。
//
// ⚠️ 必须把所有 FAKE_SCREENCAP_* 都清掉 —— 只设当前这一个的话，
//    上一个用例设的变量会残留，造成跨用例状态泄漏。
//    （第一版就是栽在这里：用例 5 报的是用例 4 的错误信息。）
void ResetEnv() {
    unsetenv("FAKE_SCREENCAP_SIZE");
    unsetenv("FAKE_SCREENCAP_BAD_FORMAT");
    unsetenv("FAKE_SCREENCAP_TRUNCATE");
    unsetenv("FAKE_SCREENCAP_FAIL");
    setenv("REMOTE_CONTROL_SCREENCAP_PATH", gFakePath, 1);
}

// 每次调用都从干净状态开始，可一次设两组键值。
//
// ⚠️ 不要连着调用两次「单变量版」—— 第二次的 ResetEnv 会把第一次设的清掉。
//    第一版就是这个坑：用例 6 里先设 FAIL 再调一次（为了清别的变量），
//    结果 FAIL 被清掉，用例静默地测了个寂寞。
void SetupEnv(const char* k1, const char* v1,
              const char* k2 = nullptr, const char* v2 = nullptr) {
    ResetEnv();
    if (k1) setenv(k1, v1, 1);
    if (k2) setenv(k2, v2, 1);
}

// 读一帧并校验像素。返回是否成功。
bool GrabAndCheck(Capture* cap, uint32_t expectW, uint32_t expectH,
                  std::string* err) {
    Frame frame;
    if (!cap->Grab(&frame, err)) return false;

    if (frame.width != expectW || frame.height != expectH) {
        *err = "尺寸不符: " + std::to_string(frame.width) + "x" +
               std::to_string(frame.height);
        return false;
    }
    if (frame.stride != expectW) {
        *err = "stride 不符: " + std::to_string(frame.stride);
        return false;
    }
    if (frame.size != static_cast<uint64_t>(expectW) * expectH * 4) {
        *err = "数据量不符: " + std::to_string(frame.size);
        return false;
    }

    void* base = mmap(nullptr, frame.size, PROT_READ, MAP_SHARED, frame.fd, 0);
    if (base == MAP_FAILED) {
        *err = "mmap 失败";
        return false;
    }

    const auto* px = static_cast<const uint8_t*>(base);
    auto at = [&](uint32_t x, uint32_t y, int ch) -> int {
        return px[(static_cast<uint64_t>(y) * expectW + x) * 4 + ch];
    };

    // 四角 + 中点
    struct { uint32_t x, y; int ch; int want; const char* name; } kChecks[] = {
        {0,          0,          0, 0,   "左上 R"},
        {expectW-1,  0,          0, 255, "右上 R"},
        {0,          expectH-1,  1, 255, "左下 G"},
        {expectW-1,  expectH-1,  1, 255, "右下 G"},
        {0,          0,          2, 0x40,"左上 B"},
    };
    for (const auto& c : kChecks) {
        const int got = at(c.x, c.y, c.ch);
        if (got != c.want) {
            *err = std::string(c.name) + " = " + std::to_string(got) +
                   "，期望 " + std::to_string(c.want);
            munmap(base, frame.size);
            return false;
        }
    }

    munmap(base, frame.size);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) gFakePath = argv[1];

    printf("\033[1m=== screencap 截图后端验证 ===\033[0m\n");
    printf("用 %s 冒充 /system/bin/screencap\n", gFakePath);

    if (access(gFakePath, X_OK) != 0) {
        fprintf(stderr, "找不到可执行的 %s（先 make）\n", gFakePath);
        return 2;
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[1] 正常路径\033[0m  64x48 RGBA_8888\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_SIZE", "64x48");
    {
        Capture cap;
        std::string err;
        Check(cap.Init(&err), "Init 成功（%s）", err.empty() ? "ok" : err.c_str());
        Check(std::string(Capture::BackendName()).find("screencap") !=
                      std::string::npos,
              "后端标识 = %s", Capture::BackendName());

        const bool ok = GrabAndCheck(&cap, 64, 48, &err);
        Check(ok, "抓帧 + 像素校验%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[2] 非方形尺寸\033[0m  100x7 —— 验证不是碰巧对上\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_SIZE", "100x7");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        const bool ok = GrabAndCheck(&cap, 100, 7, &err);
        Check(ok, "100x7 抓帧%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[3] 连续抓帧\033[0m  验证 fd 不泄漏、可重复调用\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_SIZE", "32x32");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);

        int fdsBefore = 0;
        for (int i = 0; i < 3; ++i) {
            Frame f;
            if (!cap.Grab(&f, &err)) {
                Check(false, "第 %d 次抓帧失败: %s", i, err.c_str());
                break;
            }
            fdsBefore = f.fd;
        }
        // 帧析构后 fd 应被关闭；用 fcntl 探测
        Check(fcntl(fdsBefore, F_GETFD) == -1,
              "抓帧结束后 memfd 已关闭（无 fd 泄漏）");
        Check(true, "连续 3 次抓帧均成功");
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[4] 异常路径：未知像素格式\033[0m\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_BAD_FORMAT", "1");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(!ok, "未知格式被拒绝（返回失败）");
        Check(err.find("不支持的像素格式") != std::string::npos,
              "错误信息明确: %s", err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[5] 异常路径：输出被截断\033[0m\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_SIZE", "64x48", "FAKE_SCREENCAP_TRUNCATE", "1");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(!ok, "读不满时返回失败，不返回半截数据");
        Check(err.find("不完整") != std::string::npos,
              "错误信息明确: %s", err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[6] 异常路径：子进程非零退出\033[0m\n");
    // -----------------------------------------------------------------------
    SetupEnv("FAKE_SCREENCAP_FAIL", "1");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(!ok, "子进程失败时返回失败");
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[7] 异常路径：screencap 不存在\033[0m\n");
    // -----------------------------------------------------------------------
    setenv("REMOTE_CONTROL_SCREENCAP_PATH", "/nonexistent/screencap", 1);
    {
        Capture cap;
        std::string err;
        const bool ok = cap.Init(&err);
        Check(!ok, "Init 提前发现可执行文件不存在（不用等第一次请求）");
        Check(err.find("不可执行") != std::string::npos, "错误信息: %s",
              err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[8] 异常路径：尺寸超出合理范围\033[0m\n");
    // -----------------------------------------------------------------------
    // width/height 来自子进程的输出。一旦是垃圾值，rowBytes * height
    // 会溢出回绕成小数字，于是"读少量字节就成功"，返回一帧尺寸错误的图。
    SetupEnv("FAKE_SCREENCAP_SIZE", "20000x4");   // 20000 > 16384 上限
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(!ok, "超范围尺寸被拒绝，不返回错误尺寸的帧");
        Check(err.find("超出合理范围") != std::string::npos,
              "错误信息明确: %s", err.c_str());
    }

    // 边界值：正好在上限上应该放行（16384 x 1）
    SetupEnv("FAKE_SCREENCAP_SIZE", "16384x1");
    {
        Capture cap;
        std::string err;
        cap.Init(&err);
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(ok, "正好在上限（16384）的尺寸被放行%s%s",
              ok ? "" : " —— ", ok ? "" : err.c_str());
    }

    // -----------------------------------------------------------------------
    printf("\n\033[1;34m[9] 缺少 screencap 时 Grab 也应安全失败\033[0m\n");
    // -----------------------------------------------------------------------
    // ⚠️ 必须显式设置，不能依赖上一个用例留下的环境 ——
    //    SetupEnv() 会把 REMOTE_CONTROL_SCREENCAP_PATH 重置回假命令，
    //    靠状态传递会得到"测了个寂寞"的假通过。
    ResetEnv();
    setenv("REMOTE_CONTROL_SCREENCAP_PATH", "/nonexistent/screencap", 1);
    {
        Capture cap;
        std::string err;
        Frame f;
        const bool ok = cap.Grab(&f, &err);
        Check(!ok, "Grab 返回失败而不是崩溃");
        Check(err.find("头部失败") != std::string::npos ||
                      err.find("不可执行") != std::string::npos,
              "错误信息可读: %s", err.c_str());
    }

    printf("\n[10] Per-request width restoration\n");
    SetupEnv("FAKE_SCREENCAP_SIZE", "64x48");
    {
        Capture cap;
        std::string error;
        cap.SetTargetWidth(720);
        Frame frame;
        const bool ok = cap.Grab(&frame, &error, 480);
        Check(ok && cap.TargetWidth() == 720,
              "successful per-request capture restores the configured width");
        setenv("REMOTE_CONTROL_SCREENCAP_PATH", "/nonexistent/screencap", 1);
        const bool failed = !cap.Grab(&frame, &error, 320);
        Check(failed && cap.TargetWidth() == 720,
              "failed per-request capture also restores the configured width");
    }

    printf("\n[11] Concurrent capture and display configuration\n");
    SetupEnv("FAKE_SCREENCAP_SIZE", "64x48");
    {
        Capture cap;
        std::string error;
        cap.Init(&error);
        cap.SetTargetWidth(720);
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&, i] {
                for (int j = 0; j < 8; ++j) {
                    std::string err;
                    if (i < 3) {
                        Frame frame;
                        if (!cap.Grab(&frame, &err, 320 + 80 * i) ||
                            frame.width != 64 || frame.height != 48 ||
                            cap.TargetWidth() != 720) {
                            ++failures;
                        }
                    } else {
                        cap.SetDisplayId(j + 1);
                        cap.Shutdown();
                        if (!cap.ResolveDisplay(&err)) ++failures;
                        std::vector<DisplayInfo> displays;
                        if (!cap.ListDisplays(&displays, &err) ||
                            displays.size() != 1 || displays[0].width != 64 ||
                            displays[0].height != 48) {
                            ++failures;
                        }
                    }
                }
            });
        }
        for (auto& worker : workers) worker.join();
        Check(failures == 0 && cap.TargetWidth() == 720,
              "capture and configuration entry points remain consistent concurrently");
    }

    return Summary("结果");
}
