// main.cpp — autod 入口
//
// 职责：
//   1. 初始化 Binder 线程池（截图必需）
//   2. 解析命令行
//   3. 把 socket 请求分发到 Capture / Injector
//   4. 处理信号，优雅退出

#include <getopt.h>
#include <mutex>
#include <pthread.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "autod_log.h"
#include "autod_platform.h"

#if AUTOD_HAS_BINDER_PLATFORM
#include <binder/ProcessState.h>
#endif
#include "capture.h"
#include "dispatch.h"
#include "inject.h"
#include "protocol.h"
#include "log_buffer.h"
#include "selftest.h"
#include "http_server.h"
#include "rest_api.h"
#include "service_state.h"
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
  --uid <uid>          所有初始化完成后降到该 UID（需要 root）
  --gid <gid>          配套的 GID，省略则用与 uid 相同的值
  --selftest           检查运行环境后退出（首次部署时先跑这个）
  --http-bind <地址>    启用 HTTP/JSON API 并绑定该地址（如 127.0.0.1）
                        不指定则不启用。绑非回环地址时**必须**配 --http-token
  --http-port <端口>    HTTP 端口，默认 8088
  --http-token <令牌>   访问 HTTP API 所需的 Bearer token
  --socket-mode <8进制>  socket 文件权限，默认 0660
                        放宽到 0666 可让上位应用以自己的 UID 连入；
                        但那意味着同设备任何进程都能控制本服务，请自行权衡
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


// ---------------------------------------------------------------------------
// 降权
// ---------------------------------------------------------------------------
//
// ⚠️ 必须在所有子系统初始化完成之后才调用。
// /dev/uinput 默认是 0600 root:root，socket 也可能在 /dev/socket 下 ——
// 一旦降权，这两样都打不开了。
//
// 顺序也不能反：先 setgid 再 setuid。反过来 setgid 会因为已经没有
// CAP_SETGID 而失败。
bool DropPrivileges(uid_t uid, gid_t gid, std::string* error) {
    if (setgroups(0, nullptr) != 0) {
        if (error) *error = std::string("setgroups(0) 失败: ") + strerror(errno);
        return false;
    }
    if (setgid(gid) != 0) {
        if (error) *error = std::string("setgid 失败: ") + strerror(errno);
        return false;
    }
    if (setuid(uid) != 0) {
        if (error) *error = std::string("setuid 失败: ") + strerror(errno);
        return false;
    }
    return true;
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
    uid_t       targetUid  = 0;
    gid_t       targetGid  = 0;
    bool        dropPrivileges = false;
    bool        selfTest       = false;
    mode_t      socketMode     = 0660;
    std::string httpBind;                 // 空 = 不启用 HTTP API
    uint16_t    httpPort       = 8088;
    std::string httpToken;

    enum LongOpt {
        kOptSocket = 1000,
        kOptInitSocket,
        kOptDisplay,
        kOptUid,
        kOptTouchRange,
        kOptGid,
        kOptSelfTest,
        kOptSocketMode,
        kOptHttpBind,
        kOptHttpPort,
        kOptHttpToken,
    };
    static const option kLongOptions[] = {
        {"socket",      required_argument, nullptr, kOptSocket},
        {"init-socket", required_argument, nullptr, kOptInitSocket},
        {"display",     required_argument, nullptr, kOptDisplay},
        {"uid",         required_argument, nullptr, kOptUid},
        {"gid",         required_argument, nullptr, kOptGid},
        {"touch-range", required_argument, nullptr, kOptTouchRange},
        {"selftest",    no_argument,       nullptr, kOptSelfTest},
        {"socket-mode", required_argument, nullptr, kOptSocketMode},
        {"http-bind",   required_argument, nullptr, kOptHttpBind},
        {"http-port",   required_argument, nullptr, kOptHttpPort},
        {"http-token",  required_argument, nullptr, kOptHttpToken},
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
            case kOptUid:
                // 只记录。真正降权要等 /dev/uinput 和 socket 都打开之后 ——
                // 见 main() 里 DropPrivileges() 的调用点。
                targetUid = static_cast<uid_t>(strtoul(optarg, nullptr, 10));
                dropPrivileges = true;
                break;
            case kOptGid:
                targetGid = static_cast<gid_t>(strtoul(optarg, nullptr, 10));
                break;
            case kOptSelfTest:
                selfTest = true;
                break;

            case kOptHttpBind:
                httpBind = optarg;
                break;
            case kOptHttpPort: {
                const long v = strtol(optarg, nullptr, 10);
                if (v <= 0 || v > 65535) {
                    fprintf(stderr, "错误: --http-port 需为 1-65535\n");
                    return 1;
                }
                httpPort = static_cast<uint16_t>(v);
                break;
            }
            case kOptHttpToken:
                httpToken = optarg;
                break;

            case kOptSocketMode: {
                // 八进制解析：写成 0666 或 666 都接受
                const unsigned long v = strtoul(optarg, nullptr, 8);
                if (v == 0 || v > 0777) {
                    fprintf(stderr, "错误: --socket-mode 需为八进制权限位（如 0660/0666）\n");
                    return 1;
                }
                socketMode = static_cast<mode_t>(v);
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

    // 把启动配置登记进 ServiceState —— API 的 GetConfig 要能查到它，
    // SetConfig 也要以它为基准判断"哪些改了能生效"。
    {
        ServiceState::Config sc;
        sc.socketPath      = socketPath;
        sc.usingInitSocket = !initSocketName.empty();
        sc.initSocketName  = initSocketName;
        sc.socketMode      = static_cast<uint32_t>(socketMode);
        sc.displayId       = displayId;
        sc.touchWidth      = touchW;
        sc.touchHeight     = touchH;
        sc.verbose         = verbose;
        sc.dropUid         = dropPrivileges ? static_cast<int32_t>(targetUid) : -1;
        sc.dropGid         = dropPrivileges ? static_cast<int32_t>(targetGid) : -1;
        ServiceState::Instance().SetInitialConfig(sc);
    }
    // 日志级别也走同一套：--verbose 就是 debug 级
    LogBuffer::Instance().SetMinLevel(verbose ? LogLevel::kDebug : LogLevel::kInfo);

    // 自检不需要 socket，放在必填检查之前
#if AUTOD_HAS_BINDER_PLATFORM
    // 必须在任何截图动作之前启动 Binder 线程池，自检也不例外。
    //
    // 截图走的是异步回调：captureDisplay() 提交请求后，
    // SurfaceFlinger 通过 Binder 回调把结果送回来。没人处理这个回调，
    // SyncScreenCaptureListener::waitForResults() 就会**永久阻塞**。
    //
    // （这个坑实测踩到过：--selftest 原本放在这段之前 return，
    //   于是在真机上表现为"打印完显示信息就卡死"。）
    android::ProcessState::self()->setThreadPoolMaxThreadCount(0);
    android::ProcessState::self()->startThreadPool();
#endif

    if (selfTest) {
        return RunSelfTest(verbose, touchW, touchH);
    }

    if (socketPath.empty() && initSocketName.empty()) {
        fprintf(stderr, "错误: 必须指定 --socket 或 --init-socket\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

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
    // init 模式下 socket 由 init 创建并按 .rc 里的 socket 行设权限，
    // 此时 --socket-mode 无效（我们不会去改 init 建的文件）。
    if (initSocketName.empty()) {
        server.SetSocketMode(socketMode);

        // 让 SetConfig("socket-mode", ...) 能真正作用到已监听的 socket 上。
        // 不做这一步的话，"改成功了"只是改了个数字，用户下次连接还是老权限 ——
        // 这种"报告成功但实际没变"是最难排查的一类问题。
        ServiceState::Instance().SetSocketChmodHook(
            [](uint32_t mode) {
                const std::string& p = ServiceState::Instance()
                                            .GetConfig().socketPath;
                if (!p.empty() && chmod(p.c_str(), mode) != 0) {
                    ALOGW("socket-mode 热改失败: chmod(%s, %04o): %s", p.c_str(),
                          mode, strerror(errno));
                }
            });
    }

    ServiceState::Instance().SetBackends(&capture, &injector);
    {
        InjectorConfig ic;
        ic.touchWidth  = injectConfig.touchWidth;
        ic.touchHeight = injectConfig.touchHeight;
        ic.deviceName  = injectConfig.deviceName;
        ic.displayId   = injectConfig.displayId;
        ServiceState::Instance().SetInjectorConfig(ic);
    }

    if (!server.Start(&error)) {
        ALOGE("autod: socket 启动失败: %s", error.c_str());
        fprintf(stderr, "socket 启动失败: %s\n", error.c_str());
        return 1;
    }

    // 降权放在这里 —— 所有需要特权的初始化都已完成：
    //   /dev/uinput 已打开（Injector::Init）
    //   SurfaceFlinger Binder 已连接（Capture::Init）
    //   socket 已 bind / 已从 init 接管
    // 这些 fd 降权后仍然可用。
    if (dropPrivileges) {
        if (targetGid == 0) targetGid = static_cast<gid_t>(targetUid);
        if (!DropPrivileges(targetUid, targetGid, &error)) {
            ALOGE("autod: 降权失败: %s", error.c_str());
            fprintf(stderr, "降权失败: %s\n", error.c_str());
            return 1;
        }
        fprintf(stderr, "autod: 已降到 uid=%u gid=%u\n", targetUid, targetGid);
    }

    // 优雅退出
    gServer = &server;
    signal(SIGTERM, OnSignal);
    signal(SIGINT,  OnSignal);
    // 忽略 SIGPIPE。
    //
    // socket 侧用 MSG_NOSIGNAL 就够了，但 HTTP 的流式响应用的是
    // write()（没有 per-call 的等价标志）。客户端关掉 MJPEG 页面时
    // 就会触发 SIGPIPE —— 默认行为是**直接杀掉进程**，
    // 那意味着"关一次网页就把服务干掉了"。
    // 忽略之后 write 返回 EPIPE，流式回调据此正常退出。
    signal(SIGPIPE, SIG_IGN);

    if (verbose) {
        ALOGI("autod: 就绪, socket=%s", server.path().c_str());
    }
    fprintf(stderr, "autod: 就绪, 监听 %s\n", server.path().c_str());

    Dispatcher dispatcher(&capture, &injector);

    // ── HTTP/JSON API ──
    //
    // 跑在独立线程里：它是**另一条传输**，和 Unix socket 并行服务。
    // 不该让其中一条的负载影响另一条，也不该因为 socket 侧在忙
    // 就让 HTTP 请求排队。
    HttpServer httpServer;
    RestApi    restApi(&dispatcher);
    // 用 pthread 而不是 std::thread：后者创建失败会抛异常，
    // 而 AOSP 是 -fno-exceptions，抛出去就是整个进程 terminate。
    // 这里要 join，所以不能走 SpawnDetached（那个是 detached）。
    pthread_t httpThread{};
    bool      httpThreadStarted = false;
    if (!httpBind.empty()) {
        HttpServer::Options opts;
        opts.bindAddr = httpBind;
        opts.port     = httpPort;
        opts.token    = httpToken;
        if (!httpServer.Start(opts, &error)) {
            fprintf(stderr, "autod: HTTP API 启动失败: %s\n", error.c_str());
            return 1;
        }
    }


    // HTTP 线程的上下文。用栈上的结构体而不是 lambda 捕获 ——
    // pthread_create 的入口必须是普通函数指针，捕获得靠传参。
    struct HttpThreadCtx {
        HttpServer* server;
        RestApi*    api;
    };
    HttpThreadCtx httpCtx{&httpServer, &restApi};

    if (httpServer.running()) {
        const int rc = pthread_create(
            &httpThread, nullptr,
            [](void* arg) -> void* {
                auto* c = static_cast<HttpThreadCtx*>(arg);
                // 不再在这里加锁 —— 操作串行化在 Dispatcher::Handle 里。
                // 放这里对流式响应是无效的：streamer 回调是在处理器返回
                // **之后**才跑的，那时锁已经释放了。
                c->server->Run([c](const HttpRequest& req) {
                    return c->api->Handle(req);
                });
                return nullptr;
            },
            &httpCtx);
        if (rc != 0) {
            fprintf(stderr, "autod: 起 HTTP 线程失败(%d)，HTTP API 不可用\n", rc);
            httpServer.Stop();
        } else {
            httpThreadStarted = true;
        }
    }

    server.Run([&dispatcher, &server, &httpServer](
                       const Request& req, const std::string& payload,
                       int reqFd, int peerUid) {
        // 不加锁 —— 串行化在 Dispatcher::Handle 里做。
        // 放这里对流式响应无效（回调在处理器返回之后才跑），
        // 而且两处都加会直接死锁。
        ReplyPacket packet = dispatcher.Handle(req, payload, reqFd, peerUid);
        ServiceState::Instance().CountRequest(req.cmd, packet.reply.status);

        // Shutdown / Restart 命令只是设了个标志（应答要先发出去），
        // 真正退出在这里做。Stop() 会 shutdown 监听 fd，让 Run() 的
        // accept 立刻返回。
        if (ServiceState::Instance().ShutdownRequested()) {
            server.Stop();
            httpServer.Stop();   // 让 HTTP 的 accept 也立刻返回
        }
        return packet;
    });

    gServer = nullptr;
    httpServer.Stop();
    if (httpThreadStarted) pthread_join(httpThread, nullptr);
    capture.Shutdown();

    // Restart 用退出码 1：init 的 `oneshot` + 外部监督脚本据此区分
    // "正常关闭"和"要求重启"。自己不明说，监督方就只能一律重启，
    // 那 Shutdown 就没意义了。
    if (ServiceState::Instance().RestartRequested()) {
        ALOGI("autod: 按请求重启（退出码 1）");
        fprintf(stderr, "autod: 按请求重启（退出码 1）\n");
        return 1;
    }

    ALOGI("autod: 已退出");
    fprintf(stderr, "autod: 已退出\n");
    return 0;
}
