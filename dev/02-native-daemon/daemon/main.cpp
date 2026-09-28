// main.cpp — remote-control 入口
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

#include "config_file.h"
#include "thread_util.h"
#include "remote_control_log.h"
#include "remote_control_platform.h"
#include "sha256.h"

#if REMOTE_CONTROL_HAS_BINDER_PLATFORM
#include <binder/ProcessState.h>
#endif
#include "capture.h"
#include "dispatch.h"
#include "frame_hub.h"
#include "inject.h"
#include "protocol.h"
#include "log_buffer.h"
#include "selftest.h"
#include "http_server.h"
#include "rest_api.h"
#include "service_state.h"
#include "socket_server.h"

using namespace remote_control;

namespace {

SocketServer* gServer = nullptr;

void OnSignal(int /*sig*/) {
    // Stop() 只做 shutdown()，是异步信号安全的
    if (gServer != nullptr) gServer->Stop();
}

void PrintUsage(const char* argv0) {
    fprintf(stderr, R"(remote-control — Android 系统级截图 / 触控服务

用法: %s [选项]

选项:
  --socket <路径>      手动 bind 一个 Unix socket（开发期用）
  --init-socket <名字> 接管 init 创建的 socket（生产用，见 remote-control.rc）
  --display <id>       指定显示 ID，0 表示自动选主显示
  --touch-range <WxH>  触控坐标范围，默认取显示分辨率
  --uid <uid>          所有初始化完成后降到该 UID（需要 root）
  --gid <gid>          配套的 GID，省略则用与 uid 相同的值
  --selftest           检查运行环境后退出（首次部署时先跑这个）
  --config <路径>       配置文件，默认 /sdcard/remote-control.conf（首启无鉴权）
  --http-bind <地址>    启用 HTTP/JSON API 并绑定该地址（如 0.0.0.0 对外）
                        不指定则由配置文件决定。默认的 /sdcard/remote-control.conf
                        首启是 0.0.0.0:8088 且**无鉴权**
  --http-port <端口>    HTTP 端口，默认 8088
  --http-token <令牌>   访问令牌。给了就等于开启鉴权；
                        不给则由配置文件的 auth=/token= 决定
  --socket-mode <8进制>  socket 文件权限，默认 0660
                        放宽到 0666 可让上位应用以自己的 UID 连入；
                        但那意味着同设备任何进程都能控制本服务，请自行权衡
  --ready-file <路径>   就绪后把**自身二进制的 sha256** 写进这个文件。
                        启动壳（remote-control-launch）靠它判断载荷起没起来，
                        也靠它确认"跑的就是我选的那一份"。
  --version            打印 buildId（本二进制的 sha256）后退出
  --foreground         前台运行，日志输出到 stderr
  --verbose            详细日志
  -h, --help           显示本帮助

示例:
  # 开发期：前台跑，自己 bind socket
  remote-control --socket /data/local/tmp/remote-control.sock --foreground --verbose

  # 常用：让配置文件决定监听地址/端口/鉴权（上位应用就是这么管的）
  remote-control --socket /data/local/tmp/remote-control.sock

  # 生产：由 init 拉起，socket 由 init 创建并打好 SELinux 标签
  remote-control --init-socket remote-control
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
    // 显式给了 CLI 标志就以 CLI 为准（见下面配置合并那段）
    std::string configPath = ConfigFile::DefaultPath();
    // 日志落盘路径。与配置文件同目录 —— 上位应用（无 root）也要能读它。
    std::string logPath = "/sdcard/remote-control.log";
    // 就绪标记文件。空 = 不写（开发期手工跑不需要）。
    std::string readyFile;
    bool cliBind = false, cliPort = false, cliToken = false;

    enum LongOpt {
        kOptSocket = 1000,
        kOptInitSocket,
        kOptDisplay,
        kOptUid,
        kOptTouchRange,
        kOptGid,
        kOptSelfTest,
        kOptSocketMode,
        kOptConfig,
        kOptHttpBind,
        kOptHttpPort,
        kOptHttpToken,
        kOptReadyFile,
        kOptVersion,
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
        {"config",      required_argument, nullptr, kOptConfig},
        {"http-bind",   required_argument, nullptr, kOptHttpBind},
        {"http-port",   required_argument, nullptr, kOptHttpPort},
        {"http-token",  required_argument, nullptr, kOptHttpToken},
        {"ready-file",  required_argument, nullptr, kOptReadyFile},
        {"version",     no_argument,       nullptr, kOptVersion},
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

            case kOptConfig:
                configPath = optarg;
                break;
            case kOptHttpBind:
                httpBind = optarg;
                cliBind = true;
                break;
            case kOptHttpPort: {
                const long v = strtol(optarg, nullptr, 10);
                if (v <= 0 || v > 65535) {
                    fprintf(stderr, "错误: --http-port 需为 1-65535\n");
                    return 1;
                }
                httpPort = static_cast<uint16_t>(v);
                cliPort = true;
                break;
            }
            case kOptHttpToken:
                httpToken = optarg;
                cliToken = true;
                break;
            case kOptReadyFile:
                readyFile = optarg;
                break;
            case kOptVersion:
                // buildId = 本二进制的 sha256。热替换之后靠它确认跑的是哪一版。
                printf("%s\n", SelfBuildId().c_str());
                return 0;

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
#if REMOTE_CONTROL_HAS_BINDER_PLATFORM
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
        ALOGE("remote-control: 截图子系统初始化失败: %s", error.c_str());
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
            ALOGW("remote-control: 拿不到显示分辨率，触控范围回退到 32767x32767；"
                  "此时调用方传的坐标不再是屏幕像素，请用 --touch-range 指定");
        }
    }

    InjectorConfig injectConfig;
    injectConfig.touchWidth  = touchW;
    injectConfig.touchHeight = touchH;
    injectConfig.displayId   = static_cast<int32_t>(displayId);

    Injector injector;
    if (!injector.Init(injectConfig, &error)) {
        ALOGE("remote-control: 注入子系统初始化失败: %s", error.c_str());
        fprintf(stderr, "注入初始化失败: %s\n", error.c_str());
        fprintf(stderr,
                "提示: Android 12 上 IInputManager 是 Java-only AIDL，"
                "请使用 uinput 后端\n");
        return 1;
    }
    fprintf(stderr, "remote-control: 触控后端 = %s，坐标范围 %ux%u\n",
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
        ALOGE("remote-control: socket 启动失败: %s", error.c_str());
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
            ALOGE("remote-control: 降权失败: %s", error.c_str());
            fprintf(stderr, "降权失败: %s\n", error.c_str());
            return 1;
        }
        fprintf(stderr, "remote-control: 已降到 uid=%u gid=%u\n", targetUid, targetGid);
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

    // 日志同时落盘（只留最近 10KB）。放 /sdcard：上位应用无 root 也能读。
    LogBuffer::Instance().SetHistoryPath(logPath);
    ALOGI("历史日志: %s（最多 %zu 字节）", logPath.c_str(),
          LogBuffer::kHistoryMaxBytes);

    if (verbose) {
        ALOGI("remote-control: 就绪, socket=%s", server.path().c_str());
    }
    fprintf(stderr, "remote-control: 就绪, 监听 %s\n", server.path().c_str());

    Dispatcher dispatcher(&capture, &injector);

    // 共享抓帧：告诉它用哪个 Dispatcher（抓帧要走那把操作锁）。
    //
    // 线程**不在这里启动** —— 第一个画面流订阅者到来时才启，
    // 最后一个离开时停。没人看画面的时候一次都不抓。
    FrameHub::Instance().Configure(&dispatcher);

    // ── HTTP/JSON API ──
    //
    // 跑在独立线程里：它是**另一条传输**，和 Unix socket 并行服务。
    // 不该让其中一条的负载影响另一条，也不该因为 socket 侧在忙
    // 就让 HTTP 请求排队。
    HttpServer httpServer;
    RestApi    restApi(&dispatcher);

    // 让"关闭服务 / 开启鉴权"能踢掉已经连上的客户端。
    //
    // 不给它的话，`{"on":false}` 只挡得住**新**请求 —— 一个正在拉流
    // 的网页会一直拿画面，对用户来说"关掉服务"等于没关。
    restApi.SetHttpServer(&httpServer);

    // 令牌来源改成回调，这样改鉴权**不用重启进程**。
    //
    // 重启也能生效，但那会连带断掉所有连接 —— 那不叫"开启鉴权"，
    // 那叫"重启服务"。而且重启是 supervisor 的事，API 不该依赖它。
    httpServer.SetTokenProvider([]() {
        return ServiceState::Instance().AuthToken();
    });
    // 用 pthread 而不是 std::thread：后者创建失败会抛异常，
    // 而 AOSP 是 -fno-exceptions，抛出去就是整个进程 terminate。
    // 这里要 join，所以不能走 SpawnDetached（那个是 detached）。
    pthread_t httpThread{};
    bool      httpThreadStarted = false;
    // ── 合并持久化配置 ──
    //
    // 优先级：CLI 显式给的 > /sdcard/remote-control.conf > 内置默认。
    //
    // 这个顺序是有讲究的：上位应用写配置文件、不传 CLI 参数，
    // 所以它能生效；而调试时 `--http-port 9999` 这种一次性覆盖
    // 也不会被文件悄悄改掉。
    //
    // 首启（文件不存在）就是"无鉴权 + 127.0.0.1 + 8088"，
    // 和产品要求一致。
    {
        PersistedConfig cfg;
        std::string cfgErr;
        if (!ConfigFile::Load(configPath, &cfg, &cfgErr)) {
            ALOGW("读取配置 %s 失败，用默认值: %s", configPath.c_str(),
                  cfgErr.c_str());
        }
        if (!cliBind)  httpBind  = cfg.bind;
        if (!cliPort)  httpPort  = static_cast<uint16_t>(cfg.port);
        if (!cliToken) httpToken = cfg.token;

        // auth=1 但还没有令牌 → 生成一个并写回文件。
        //
        // 只在这里生成（而不是每次启动都生成）：令牌一旦变了，
        // 已经配好它的客户端就全部失效，用户还得再去文件里看一眼。
        if (cfg.auth && httpToken.empty()) {
            const std::string fresh = ConfigFile::GenerateToken();
            if (fresh.empty()) {
                // 拿不到安全的随机数就**不要假装开了鉴权** ——
                // 用弱令牌比明说"没开"更危险。
                ALOGE("无法生成随机令牌，鉴权未启用（接口将无鉴权）");
            } else {
                httpToken = fresh;
                cfg.token = fresh;
                cfg.tokenWasGenerated = true;
                std::string saveErr;
                if (!ConfigFile::Save(configPath, cfg, &saveErr)) {
                    ALOGW("令牌已生成但写回 %s 失败: %s", configPath.c_str(),
                          saveErr.c_str());
                }
                ALOGI("已生成访问令牌并写入 %s —— 用 `grep token %s` 查看",
                      configPath.c_str(), configPath.c_str());
            }
        }
        // 服务对外开关也持久化在同一个文件里
        ServiceState::Instance().SetServingPersistPath(configPath);
        // enabled=0 表示"不对外提供服务"，而不是"别启动" ——
        // 进程始终要跑，否则开关本身就没入口了。
        ServiceState::Instance().SetServing(cfg.enabled);

        ALOGI("配置: %s | bind=%s port=%u auth=%s", configPath.c_str(),
              httpBind.empty() ? "(未启用)" : httpBind.c_str(), httpPort,
              httpToken.empty() ? "关" : "开");
    }

    // 把令牌交给 ServiceState —— HttpServer 的 provider 从这里读。
    //
    // 放在**配置块之外**：CLI 的 --http-token 和配置文件都可能给出令牌，
    // 上面的分支只处理了配置文件那条路。
    ServiceState::Instance().SetAuthToken(httpToken);

    // ── 配置文件监视 ──
    //
    // 上位应用（和任何编辑器）改的是文件，而守护进程只在启动时读一次。
    // 不监视的话，"拨了开关但没反应"就是必然的 —— 实测踩过：
    // 配置文件里 enabled=1，运行中的服务却还是关着的。
    //
    // 只处理 enabled（软开关）。bind/port/auth 这些改了必须重启进程，
    // 由 supervisor 负责 —— 在这里热改会让正在看的画面流和触控连接
    // 莫名其妙断掉。
    if (!configPath.empty()) {
        struct CfgWatchCtx { std::string path; };
        static CfgWatchCtx ctx{configPath};
        if (!SpawnDetached([]() {
                int64_t lastMtime = 0;
                while (true) {
                    struct stat st{};
                    if (stat(ctx.path.c_str(), &st) == 0) {
                        const int64_t mt = static_cast<int64_t>(st.st_mtime);
                        if (mt != lastMtime) {
                            lastMtime = mt;
                            PersistedConfig c;
                            std::string err;
                            if (ConfigFile::Load(ctx.path, &c, &err)) {
                                // SetServing 内部会判断"值没变就不动"，
                                // 所以不会因为我们自己写回文件而反复触发
                                ServiceState::Instance().SetServing(c.enabled);
                            }
                        }
                    }
                    sleep(2);
                }
            })) {
            ALOGW("起配置监视线程失败，配置文件改动不会实时生效");
        }
    }

    if (!httpBind.empty()) {
        HttpServer::Options opts;
        opts.bindAddr = httpBind;
        opts.port     = httpPort;
        opts.token    = httpToken;
        if (!httpServer.Start(opts, &error)) {
            fprintf(stderr, "remote-control: HTTP API 启动失败: %s\n", error.c_str());
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
            fprintf(stderr, "remote-control: 起 HTTP 线程失败(%d)，HTTP API 不可用\n", rc);
            httpServer.Stop();
        } else {
            httpThreadStarted = true;
        }
    }

    // ── 就绪：告诉壳"这版起来了" ──
    //
    // ⚠️ 写在 server.Run **之前**、HTTP 线程起来**之后** —— 位置很关键：
    //    早了（socket 还没 listen）会把一个其实没起来的版本报成就绪；
    //    晚了就永远写不到（Run 是阻塞的）。
    //
    // 内容必须是**自身二进制的哈希**：壳拿它和期望的槽哈希比对。
    // 只写个"ready"的话，上一版留下的残留文件会让一个起不来的新版通过检查。
    if (!readyFile.empty()) {
        const std::string& id = SelfBuildId();
        FILE* rf = fopen(readyFile.c_str(), "w");
        if (rf != nullptr) {
            fprintf(rf, "%s\n", id.c_str());
            fclose(rf);
            ALOGI("就绪标记已写: %s = %s", readyFile.c_str(), id.c_str());
        } else {
            ALOGW("写就绪标记失败: %s（%s）", readyFile.c_str(), strerror(errno));
        }
    }

    server.Run([&dispatcher, &server, &httpServer](
                       const Request& req, const std::string& payload,
                       int reqFd, int peerUid) {
        // ── 服务对外开关 ──
        //
        // HTTP 那边在 RestApi::Handle 里挡了，socket 这边也要挡 ——
        // 不然"关掉服务"只是关掉了 HTTP，本机进程照样能通过 socket
        // 完整控制设备，软开关就成了摆设。
        //
        // 只放行 ServiceSwitch 本身（带 kFlagForce），否则关掉之后
        // 就没有任何入口能开回来了。
        if (!ServiceState::Instance().Serving() &&
            req.cmd != static_cast<uint32_t>(Cmd::ServiceSwitch)) {
            ALOGW("socket 拒绝（服务已关闭对外能力）cmd=%u", req.cmd);
            ReplyPacket denied;
            denied.reply.magic  = kMagic;
            denied.reply.status = kErrPermission;
            denied.reply.cmd    = req.cmd;
            ServiceState::Instance().CountRequest(req.cmd, kErrPermission);
            return denied;      // 处理器是取返回值的，不是循环体
        }

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
        ALOGI("remote-control: 按请求重启（退出码 1）");
        fprintf(stderr, "remote-control: 按请求重启（退出码 1）\n");
        return 1;
    }

    ALOGI("remote-control: 已退出");
    fprintf(stderr, "remote-control: 已退出\n");
    return 0;
}
