// main.cpp — autod 入口
//
// 职责：
//   1. 初始化 Binder 线程池（截图必需）
//   2. 解析命令行
//   3. 把 socket 请求分发到 Capture / Injector
//   4. 处理信号，优雅退出

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <binder/ProcessState.h>
#endif

#include "autod_log.h"
#include "capture.h"
#include "dispatch.h"
#include "inject.h"
#include "protocol.h"
#include "socket_server.h"

using namespace autod;

namespace {

SocketServer* gServer = nullptr;

void OnSignal(int /*sig*/) {
    // Stop() 只做 shutdown()，是异步信号安全的
    if (gServer != nullptr) gServer->Stop();
}

void PrintUsage(const char* argv0) {
    fprintf(stderr, R"(autod — Android 系统级截图 / 触控服务

用法: %s [选项]

选项:
  --socket <路径>      手动 bind 一个 Unix socket（开发期用）
  --init-socket <名字> 接管 init 创建的 socket（生产用，见 autod.rc）
  --display <id>       指定显示 ID，0 表示自动选主显示
  --touch-range <WxH>  触控坐标范围，默认取显示分辨率
  --uid <uid>          启动后切换到的 UID（需要 root 权限）
  --foreground         前台运行，日志输出到 stderr
  --verbose            详细日志
  -h, --help           显示本帮助

示例:
  # 开发期：前台跑，自己 bind socket
  autod --socket /data/local/tmp/autod.sock --foreground --verbose

  # 生产：由 init 拉起，socket 由 init 创建并打好 SELinux 标签
  autod --init-socket autod
)",
            argv0);
}

}  // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string socketPath;
    std::string initSocketName;
    uint64_t    displayId  = 0;
    uint32_t    touchW     = 0;
    uint32_t    touchH     = 0;
    bool        verbose    = false;

    enum LongOpt {
        kOptSocket = 1000,
        kOptInitSocket,
        kOptDisplay,
        kOptUid,
        kOptTouchRange,
    };
    static const option kLongOptions[] = {
        {"socket",      required_argument, nullptr, kOptSocket},
        {"init-socket", required_argument, nullptr, kOptInitSocket},
        {"display",     required_argument, nullptr, kOptDisplay},
        {"uid",         required_argument, nullptr, kOptUid},
        {"touch-range", required_argument, nullptr, kOptTouchRange},
        {"foreground",  no_argument,       nullptr, 'f'},
        {"verbose",     no_argument,       nullptr, 'v'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "fvh", kLongOptions, nullptr)) != -1) {
        switch (c) {
            case kOptSocket:
                socketPath = optarg;
                break;
            case kOptInitSocket:
                initSocketName = optarg;
                break;
            case kOptDisplay:
                displayId = strtoull(optarg, nullptr, 0);
                break;
            case kOptUid: {
                const uid_t uid = static_cast<uid_t>(strtoul(optarg, nullptr, 10));
                if (setuid(uid) != 0) {
                    fprintf(stderr, "setuid(%u) 失败: %s\n", uid, strerror(errno));
                    return 1;
                }
                break;
            }
            case kOptTouchRange: {
                // 形如 1080x2400。不指定则用显示分辨率。
                unsigned w = 0, h = 0;
                if (sscanf(optarg, "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
                    fprintf(stderr, "错误的 --touch-range 格式: %s（应为 WxH）\n",
                            optarg);
                    return 1;
                }
                touchW = w;
                touchH = h;
                break;
            }
            case 'f':
                // 内建日志本来就写 logd，这个开关只是提示用户别用 & 后台跑
                break;
            case 'v':
                verbose = true;
                break;
            case 'h':
            default:
                PrintUsage(argv[0]);
                return c == 'h' ? 0 : 1;
        }
    }

    if (socketPath.empty() && initSocketName.empty()) {
        fprintf(stderr, "错误: 必须指定 --socket 或 --init-socket\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

#ifdef __ANDROID__
    // 截图必需。setThreadPoolMaxThreadCount(0) 告诉内核不要额外起线程，
    // 但我们仍然需要 startThreadPool() 来收 Binder 回调 —— 否则
    // SyncScreenCaptureListener 会死等。参考 screencap.cpp 里 b/36066697 的说明。
    android::ProcessState::self()->setThreadPoolMaxThreadCount(0);
    android::ProcessState::self()->startThreadPool();
#endif

    Capture capture;
    std::string error;
    if (displayId != 0) capture.SetDisplayId(displayId);
    if (!capture.Init(&error)) {
        ALOGE("autod: 截图子系统初始化失败: %s", error.c_str());
        fprintf(stderr, "截图初始化失败: %s\n", error.c_str());
        return 1;
    }

    // 触控坐标范围：默认取显示分辨率，这样调用方传进来的就是屏幕像素坐标。
    // 不这么做的话坐标会被映射到左上角一小块区域。
    if (touchW == 0 || touchH == 0) {
        std::vector<DisplayInfo> displays;
        if (capture.ListDisplays(&displays, &error) && !displays.empty() &&
            displays.front().width > 0 && displays.front().height > 0) {
            touchW = displays.front().width;
            touchH = displays.front().height;
        } else {
            ALOGW("autod: 拿不到显示分辨率，触控范围回退到 32767x32767；"
                  "此时调用方传的坐标不再是屏幕像素，请用 --touch-range 指定");
        }
    }

    InjectorConfig injectConfig;
    injectConfig.touchWidth  = touchW;
    injectConfig.touchHeight = touchH;
    injectConfig.displayId   = static_cast<int32_t>(displayId);

    Injector injector;
    if (!injector.Init(injectConfig, &error)) {
        ALOGE("autod: 注入子系统初始化失败: %s", error.c_str());
        fprintf(stderr, "注入初始化失败: %s\n", error.c_str());
        fprintf(stderr,
                "提示: Android 12 上 IInputManager 是 Java-only AIDL，"
                "请使用 uinput 后端\n");
        return 1;
    }
    fprintf(stderr, "autod: 触控后端 = %s，坐标范围 %ux%u\n",
            injector.BackendName(), touchW, touchH);

    SocketServer server = initSocketName.empty()
                              ? SocketServer::FromPath(socketPath)
                              : SocketServer::FromInitSocket(initSocketName);

    if (!server.Start(&error)) {
        ALOGE("autod: socket 启动失败: %s", error.c_str());
        fprintf(stderr, "socket 启动失败: %s\n", error.c_str());
        return 1;
    }

    // 优雅退出
    gServer = &server;
    signal(SIGTERM, OnSignal);
    signal(SIGINT,  OnSignal);

    if (verbose) {
        ALOGI("autod: 就绪, socket=%s", server.path().c_str());
    }
    fprintf(stderr, "autod: 就绪, 监听 %s\n", server.path().c_str());

    Dispatcher dispatcher(&capture, &injector);
    server.Run([&dispatcher](const Request& req, int peerUid) {
        return dispatcher.Handle(req, peerUid);
    });

    gServer = nullptr;
    capture.Shutdown();
    fprintf(stderr, "autod: 已退出\n");
    return 0;
}
