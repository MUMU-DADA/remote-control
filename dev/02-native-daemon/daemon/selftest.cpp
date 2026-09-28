// selftest.cpp —— 部署自检
//
// 逐项检查 remote-control 运行所需的环境，每项给出可操作的结论。
//
// 为什么需要它：remote-control 依赖四件容易出问题的事——
//   1. /dev/uinput 的 POSIX 权限（0660 uhid:uhid）和 SELinux 标签（uhid_device）
//   2. screencap 命令存在且可执行（NDK 构建的截图后端）
//   3. SurfaceFlinger Binder 可达（AOSP 构建的截图后端）
//   4. 触控坐标范围与真实分辨率一致
// 任何一项不对，症状都是"服务起来了但什么都不工作"，很难定位。

#include "selftest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "json_writer.h"
#include <vector>

#include "remote_control_log.h"
#include "remote_control_platform.h"
#include "capture.h"
#include "inject.h"

namespace remote_control {
namespace {

// 检查结果同时满足两种消费方式：
//   - 人看：RunSelfTest 打印成带颜色的一行行
//   - 机器看：RunSelfTestJson 输出 JSON，供 API 客户端解析
// 为此把"产生结果"与"呈现结果"分开 —— 检查逻辑只往里塞条目。
enum class ItemKind { kPass, kFail, kNote };

struct CheckItem {
    std::string section;
    ItemKind    kind;
    std::string text;
};

std::vector<CheckItem> gItems;
std::string gSection;
bool gPrint = true;          // false = JSON 模式，只收集不打印

int gPassed = 0;
int gFailed = 0;

std::string Format(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

void Add(ItemKind kind, const std::string& text) {
    gItems.push_back({gSection, kind, text});
}

void Pass(const char* fmt, ...) {
    ++gPassed;
    va_list ap;
    va_start(ap, fmt);
    const std::string t = Format(fmt, ap);
    va_end(ap);
    Add(ItemKind::kPass, t);
    if (gPrint) {
        printf("  \033[1;32m✓\033[0m %s\n", t.c_str());
        fflush(stdout);   // 卡住/被杀时也要能看到已经检查到哪一步
    }
}

void Fail(const char* fmt, ...) {
    ++gFailed;
    va_list ap;
    va_start(ap, fmt);
    const std::string t = Format(fmt, ap);
    va_end(ap);
    Add(ItemKind::kFail, t);
    if (gPrint) {
        printf("  \033[1;31m✗\033[0m %s\n", t.c_str());
        fflush(stdout);
    }
}

void Info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string t = Format(fmt, ap);
    va_end(ap);
    Add(ItemKind::kNote, t);
    if (gPrint) {
        printf("    \033[2m%s\033[0m\n", t.c_str());
        fflush(stdout);
    }
}

void Section(const char* title) {
    gSection = title;
    if (gPrint) {
        printf("\n\033[1;34m%s\033[0m\n", title);
        fflush(stdout);
    }
}

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// 1. 运行身份与 /dev/uinput
// ---------------------------------------------------------------------------
void CheckRunEnvironment(bool /*verbose*/) {
    Section("[1] 运行环境");

    const uid_t uid = geteuid();
    if (uid == 0) {
        Pass("以 root 运行 (uid=0)");
    } else {
        Info("以 uid=%u 运行", uid);
        // 非 root 不一定是错 —— 如果 uhid 组已授予就够用
    }

    // /dev/uinput 的存在性与权限
    struct stat st{};
    if (stat("/dev/uinput", &st) != 0) {
        Fail("/dev/uinput 不存在 (%s)", strerror(errno));
        Info("→ 确认内核启用了 CONFIG_INPUT_UINPUT");
        return;
    }

    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        Fail("/dev/uinput 打不开: %s", strerror(errno));
        Info("→ 权限是 %04o，属主 uid=%u gid=%u",
             st.st_mode & 07777, st.st_uid, st.st_gid);
        Info("→ 需要：进程在 uhid 组里，且 SELinux 允许 uhid_device");
        Info("→ Android 上 /dev/uinput 是 0660 uhid:uhid（ueventd.rc）");
        Info("→ init.rc 里加 `group system uhid`");
        return;
    }
    close(fd);
    Pass("/dev/uinput 可写（权限 %04o, uid=%u gid=%u）",
         st.st_mode & 07777, st.st_uid, st.st_gid);

#if REMOTE_CONTROL_HAS_BINDER_PLATFORM
    Pass("平台构建：可访问 Binder / SurfaceFlinger");
#else
    Info("非平台构建：截图走 exec 外部命令");
#endif
}

// ---------------------------------------------------------------------------
// 2. 截图
// ---------------------------------------------------------------------------
void CheckCapture(bool verbose) {
    Section("[2] 截图");

    Capture capture;
    std::string error;
    if (!capture.Init(&error)) {
        Fail("Capture::Init 失败: %s", error.c_str());
        Info("→ screencap 后端：确认 /system/bin/screencap 存在且可执行");
        Info("→ SurfaceFlinger 后端：确认 SELinux 允许 binder_call 到 surfaceflinger");
        return;
    }
    Pass("后端: %s", Capture::BackendName());

    std::vector<DisplayInfo> displays;
    if (capture.ListDisplays(&displays, &error) && !displays.empty()) {
        const DisplayInfo& d = displays.front();
        if (d.width > 0 && d.height > 0) {
            Pass("显示 %s: %ux%u @%uHz",
                 displays.size() > 1 ? "（取第一个）" : "", d.width, d.height,
                 d.refreshHz);
        } else {
            Info("显示尺寸未知（screencap 后端不提供）");
            Info("→ 触控范围必须用 --touch-range 显式指定");
        }
    } else {
        Info("枚举显示失败: %s", error.c_str());
    }

    // 真抓一帧
    const int64_t t0 = NowMs();
    Frame frame;
    error.clear();
    if (!capture.Grab(&frame, &error)) {
        Fail("抓帧失败: %s", error.c_str());
        return;
    }
    const int64_t elapsed = NowMs() - t0;

    Pass("抓帧成功 %ux%u，%llu 字节，耗时 %lld ms", frame.width, frame.height,
         static_cast<unsigned long long>(frame.size),
         static_cast<long long>(elapsed));

    // 帧内容全黑通常意味着抓错了缓冲
    if (frame.fd >= 0 && frame.size > 0) {
        void* base = mmap(nullptr, frame.size, PROT_READ, MAP_SHARED, frame.fd, 0);
        if (base != MAP_FAILED) {
            const auto* px = static_cast<const uint8_t*>(base);
            // 采样若干点看是否全零
            bool allZero = true;
            const uint64_t step = (frame.size / 4 / 64) * 4;   // 采 64 个点
            for (uint64_t i = 0; i + 3 < frame.size && step > 0; i += step) {
                if (px[i] || px[i + 1] || px[i + 2]) { allZero = false; break; }
            }
            munmap(base, frame.size);
            if (allZero) {
                Info("⚠ 采样点全黑 —— 可能抓到了空缓冲，或屏幕本身是黑的");
            } else if (verbose) {
                Info("帧内容非空");
            }
        }
    }

    if (elapsed > 200) {
        Info("说明：%lld ms 是 fork+exec 路径的正常水平；"
             "SurfaceFlinger 直连约 20-35 ms", static_cast<long long>(elapsed));
    }
}

// ---------------------------------------------------------------------------
// 3. 触控
// ---------------------------------------------------------------------------
void CheckInject(bool verbose, uint32_t cliW, uint32_t cliH) {
    Section("[3] 触控");

    // 确定坐标范围：命令行优先，其次问截图后端，最后才回退
    uint32_t w = cliW;
    uint32_t h = cliH;

    if (w == 0 || h == 0) {
        Capture capture;
        std::string err;
        if (capture.Init(&err)) {
            std::vector<DisplayInfo> displays;
            if (capture.ListDisplays(&displays, &err) && !displays.empty()) {
                w = displays.front().width;
                h = displays.front().height;
            }
        }
    }

    if (w == 0 || h == 0) {
        w = 32767;
        h = 32767;
        Info("拿不到显示尺寸，用默认范围 %ux%u 试探", w, h);
        Info("→ 这个结论不可信：坐标换算对不上屏幕");
        Info("→ 用 --touch-range WxH 显式指定（先跑 `wm size` 看分辨率）");
    } else if (cliW != 0) {
        Info("坐标范围来自 --touch-range");
    }

    InjectorConfig cfg;
    cfg.touchWidth  = w;
    cfg.touchHeight = h;

    Injector injector;
    std::string error;
    if (!injector.Init(cfg, &error)) {
        Fail("Injector::Init 失败: %s", error.c_str());
#if REMOTE_CONTROL_HAS_BINDER_PLATFORM
        Info("→ 平台构建：确认 SELinux 允许 uhid_device 或 binder_call 到 virtual_touchpad");
#else
        Info("→ 确认 /dev/uinput 可写（见 [1]）");
#endif
        return;
    }
    Pass("后端: %s", injector.BackendName());

    // 真发一次点击（坐标取屏幕中心，抬起）
    TouchPoint p;
    p.id = 0;
    p.x  = static_cast<int32_t>(w / 2);
    p.y  = static_cast<int32_t>(h / 2);

    const int64_t t0 = NowMs();
    error.clear();
    if (!injector.Tap(p, 20, true, &error)) {
        Fail("测试点击失败: %s", error.c_str());
        return;
    }
    const int64_t elapsed = NowMs() - t0;

    Pass("测试点击成功（屏幕中心 %d,%d，耗时 %lld ms）", p.x, p.y,
         static_cast<long long>(elapsed));

    if (verbose) {
        Info("说明：uinput 会创建一个可枚举的输入设备，"
             "出现在 /proc/bus/input/devices");
        Info("      用 `getevent -pl` 可以看到它");
    }
}

}  // namespace

// ---------------------------------------------------------------------------

namespace {

// 每次跑之前清一次 —— 这个函数既被 --selftest 用，也被 API 反复调用，
// 累计上一次的结果会给出错误的通过/失败数。
void ResetResults(bool print) {
    gItems.clear();
    gSection.clear();
    gPassed = 0;
    gFailed = 0;
    gPrint  = print;
}

void RunChecks(bool verbose, uint32_t touchWidth, uint32_t touchHeight) {
    CheckRunEnvironment(verbose);
    CheckCapture(verbose);
    CheckInject(verbose, touchWidth, touchHeight);
}

}  // namespace

int RunSelfTest(bool verbose, uint32_t touchWidth, uint32_t touchHeight) {
    ResetResults(/*print=*/true);

    printf("\033[1mremote-control 部署自检\033[0m\n");
    printf("逐项检查运行所需的环境。\n");

    RunChecks(verbose, touchWidth, touchHeight);

    printf("\n\033[1m=== 结果 ===\033[0m\n");
    if (gFailed == 0) {
        printf("\033[1;32m全部通过\033[0m（%d 项）\n", gPassed);
        printf("\n下一步：\n");
        printf("  remote-control --socket /data/local/tmp/remote-control.sock --foreground &\n");
        printf("  rcctl --socket /data/local/tmp/remote-control.sock tap 540 1200\n");
        return 0;
    }
    printf("\033[1;31m%d 项失败\033[0m（%d 项通过）\n", gFailed, gPassed);
    printf("\n按上面的 → 提示逐项排查。\n");
    return gFailed;
}

// 同一套检查的 JSON 版本，供 API 的 SelfTest 命令使用。
//
// 复用 RunChecks 而不是重写一遍：自检的检查项会随版本增长，
// 两份实现迟早会不一致（而且不一致的那个一定是没人跑的那个）。
std::string RunSelfTestJson(bool verbose, uint32_t touchWidth,
                            uint32_t touchHeight) {
    ResetResults(/*print=*/false);
    RunChecks(verbose, touchWidth, touchHeight);

    json::Writer w;
    w.Obj()
        .Field("ok", gFailed == 0)
        .Field("passed", static_cast<uint64_t>(gPassed))
        .Field("failed", static_cast<uint64_t>(gFailed))
        .Key("checks").Arr();

    // 按段分组，客户端可以直接按段渲染
    std::string lastSection;
    bool openGroup = false;
    for (const auto& it : gItems) {
        if (it.section != lastSection) {
            if (openGroup) { w.EndArr().EndObj(); openGroup = false; }
            w.Obj().Field("section", it.section).Key("items").Arr();
            lastSection = it.section;
            openGroup = true;
        }
        const char* kind = it.kind == ItemKind::kPass ? "pass"
                         : it.kind == ItemKind::kFail ? "fail"
                                                      : "note";
        w.Obj().Field("kind", kind).Field("text", it.text).EndObj();
    }
    if (openGroup) w.EndArr().EndObj();
    w.EndArr().EndObj();
    return w.str();
}

}  // namespace remote_control
