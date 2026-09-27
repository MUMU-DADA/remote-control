// test_integration.cpp —— 端到端集成测试
//
// 用**真实的** Dispatcher / SocketServer / Injector，只把截图后端换成桩。
// 覆盖链路：
//
//   客户端 ──SOCK_SEQPACKET──> SocketServer ──> Dispatcher ──┬──> Capture(桩)
//                                                            │      └─ memfd ──SCM_RIGHTS──> 读回校验像素
//                                                            └──> Injector
//                                                                    └─ /dev/uinput ──> 内核 ──> eventN 读回
//
// 没被覆盖的只有 capture_surfaceflinger.cpp（需要 AOSP 环境的 SurfaceFlinger）。
//
// 编译运行:
//   make test_integration && sudo ./test_integration

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "../daemon/capture.h"
#include "../daemon/dispatch.h"
#include "../daemon/inject.h"
#include "../daemon/protocol.h"
#include "../daemon/socket_server.h"
#include "test_util.h"

using namespace autod;
using namespace autodtest;

namespace {

constexpr const char* kSocketPath = "/tmp/autod-itest.sock";
constexpr const char* kDeviceName = "autod-itest-touch";
constexpr uint32_t    kWidth  = 1080;
constexpr uint32_t    kHeight = 1920;

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------
int ConnectClient() {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, kSocketPath, strlen(kSocketPath));

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 发请求收应答。若应答携带 fd，写入 *outFd（无则 -1）。
bool Transact(int fd, const Request& req, Reply* reply, int* outFd = nullptr) {
    if (outFd) *outFd = -1;

    if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) return false;

    iovec iov{};
    iov.iov_base = reply;
    iov.iov_len  = sizeof(Reply);

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n != static_cast<ssize_t>(sizeof(Reply))) return false;

    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            outFd) {
            memcpy(outFd, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    return true;
}

Request MakeRequest(Cmd cmd) {
    Request r{};
    r.magic = kMagic;
    r.cmd   = static_cast<uint32_t>(cmd);
    return r;
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

void TestInfo(int cfd) {
    printf("\n\033[1;34m[1] Info 请求\033[0m\n");

    Reply reply{};
    Check(Transact(cfd, MakeRequest(Cmd::Info), &reply), "收到应答");
    Check(reply.magic == kMagic, "magic 正确");
    Check(reply.status == kOk, "status = ok");
    Check(reply.width == kWidth && reply.height == kHeight,
          "返回 %ux%u", reply.width, reply.height);
}

// 验证帧通道：memfd → SCM_RIGHTS → mmap → 像素
void TestCaptureFrame(int cfd) {
    printf("\n\033[1;34m[2] Capture 帧通道\033[0m  memfd + SCM_RIGHTS + 像素校验\n");

    Reply reply{};
    int frameFd = -1;
    Check(Transact(cfd, MakeRequest(Cmd::Capture), &reply, &frameFd),
          "收到应答");
    Check(reply.status == kOk, "status = ok");
    Check(frameFd >= 0, "应答里带了 fd");

    if (reply.status != kOk || frameFd < 0) return;

    Check(reply.width == kWidth && reply.height == kHeight,
          "帧尺寸 %ux%u", reply.width, reply.height);
    Check(reply.stride == kWidth, "stride = %u", reply.stride);
    Check(reply.dataSize == static_cast<uint64_t>(kWidth) * kHeight * 4,
          "数据量 %llu 字节",
          static_cast<unsigned long long>(reply.dataSize));

    const uint64_t size = reply.dataSize;
    void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, frameFd, 0);
    if (base == MAP_FAILED) {
        Check(false, "mmap 失败: %s", strerror(errno));
        close(frameFd);
        return;
    }
    Check(true, "fd 可 mmap，%llu 字节", static_cast<unsigned long long>(size));

    // 桩后端画的是渐变图：R = x/(w-1)*255, G = y/(h-1)*255, B = 0x40
    // 抽几个点验证数据穿过整条链路没被破坏
    const auto* px = static_cast<const uint8_t*>(base);
    auto pixelAt = [&](uint32_t x, uint32_t y, int ch) -> int {
        return px[(static_cast<uint64_t>(y) * kWidth + x) * 4 + ch];
    };

    Check(pixelAt(0, 0, 2) == 0x40, "左上角 B 通道 = 0x%02x", pixelAt(0, 0, 2));
    Check(pixelAt(kWidth - 1, 0, 0) == 255,
          "右上角 R 通道 = %d（应为 255）", pixelAt(kWidth - 1, 0, 0));
    Check(pixelAt(0, kHeight - 1, 1) == 255,
          "左下角 G 通道 = %d（应为 255）", pixelAt(0, kHeight - 1, 1));
    Check(pixelAt(0, 0, 0) == 0, "左上角 R 通道 = 0");

    // 中点应接近 128
    const int midR = pixelAt(kWidth / 2, 0, 0);
    Check(midR > 120 && midR < 136, "水平中点 R = %d（应接近 128）", midR);

    munmap(base, size);
    close(frameFd);
}

void TestTapRoundTrip(int cfd, int readFd) {
    printf("\n\033[1;34m[3] Tap 全链路\033[0m  客户端 → socket → dispatch → 注入 → 内核\n");

    DrainEvents(readFd);

    Request req = MakeRequest(Cmd::Tap);
    req.x = 321;
    req.y = 654;
    req.durationMs = 20;

    Reply reply{};
    Check(Transact(cfd, req, &reply), "socket 往返成功");
    Check(reply.status == kOk, "服务端返回 ok");

    // 轮询等到事件齐，而不是死等固定时长 ——
    // 构建占满 CPU 时事件到达会被推迟，固定睡眠会造成偶发失败。
    const auto evs = WaitEvents(readFd, 11, 3000);

    Check(!evs.empty(), "内核收到了 %zu 个事件", evs.size());
    if (evs.empty()) return;

    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 1) == 1, "BTN_TOUCH 按下");
    Check(CountOf(evs, EV_KEY, BTN_TOUCH, 0) == 1, "BTN_TOUCH 抬起");

    int32_t x = -1;
    const bool gotX = LastValueOf(evs, EV_ABS, ABS_MT_POSITION_X, &x);
    Check(gotX && x == 321, "坐标穿过整条链路未被破坏: X = %d", x);

    int32_t y = -1;
    const bool gotY = LastValueOf(evs, EV_ABS, ABS_MT_POSITION_Y, &y);
    Check(gotY && y == 654, "Y = %d", y);
}

void TestSwipeRoundTrip(int cfd, int readFd) {
    printf("\n\033[1;34m[4] Swipe 全链路\033[0m\n");

    DrainEvents(readFd);

    Request req = MakeRequest(Cmd::Swipe);
    req.x  = 100; req.y  = 1700;
    req.x2 = 900; req.y2 = 200;
    req.durationMs = 120;

    Reply reply{};
    Check(Transact(cfd, req, &reply), "socket 往返成功");
    Check(reply.status == kOk, "服务端返回 ok");

    // 先等到第一个事件批出现，再补齐剩下的
    auto evs = WaitEvents(readFd, 1, 3000);
    for (int i = 0; i < 20 && CountSyn(evs) < 4; ++i) {
        usleep(50000);
        auto more = DrainEvents(readFd);
        evs.insert(evs.end(), more.begin(), more.end());
    }

    Check(CountSyn(evs) > 3, "产生了 %d 个事件批（DOWN + MOVE* + UP）",
          CountSyn(evs));

    int32_t lastX = -1;
    const bool gotX = LastValueOf(evs, EV_ABS, ABS_MT_POSITION_X, &lastX);
    Check(gotX && lastX == 900, "终点 X 到达 900，实际 %d", lastX);
}

void TestProtocolRobustness(int cfd) {
    printf("\n\033[1;34m[5] 协议健壮性\033[0m\n");

    {
        Request req = MakeRequest(Cmd::Tap);
        req.magic = 0xDEADBEEF;
        req.x = 1; req.y = 1;

        Reply reply{};
        Check(Transact(cfd, req, &reply), "错误 magic：仍能收到应答（连接未断）");
        Check(reply.status == kErrBadMagic, "返回 kErrBadMagic (0x%x)",
              reply.status);
    }

    {
        Request req = MakeRequest(Cmd::Tap);
        req.cmd = 9999;

        Reply reply{};
        Check(Transact(cfd, req, &reply), "未知命令：收到应答");
        Check(reply.status == kErrBadCmd, "返回 kErrBadCmd (0x%x)", reply.status);
    }

    {
        Reply reply{};
        Check(Transact(cfd, MakeRequest(Cmd::Info), &reply) &&
                      reply.status == kOk,
              "经过上述异常后，正常请求仍可处理");
    }
}

// 数当前进程打开的 fd 数量
//
// 集成测试里服务端与客户端在同一个进程，所以这个数字能反映
// 服务端有没有漏关 fd —— 对常驻服务来说这是最要紧的一类问题：
// 一次抓帧会经过 pipe/memfd/GraphicBuffer 好几个 fd，
// 漏一个就意味着跑几小时后必然 EMFILE。
int CountOpenFds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d) != nullptr) ++n;
    closedir(d);
    return n - 2;   // 减掉 "." 和 ".."
}

// 验证空闲连接不会永久占用服务
void TestIdleTimeout() {
    printf("\n\033[1;34m[6] 连接空闲超时\033[0m  "
           "连上但不发数据，服务不应被卡死\n");

    // 连上但不发任何数据
    const int idleFd = ConnectClient();
    Check(idleFd >= 0, "建立了一个不发数据的连接");
    if (idleFd < 0) return;

    // 服务端超时设的是 1 秒（main 里通过环境变量设的），等 1.6 秒
    usleep(1600 * 1000);

    // 服务端应该已经主动断开 —— 读会返回 0（EOF）
    char buf[1];
    ssize_t n;
    do {
        n = recv(idleFd, buf, 1, 0);
    } while (n < 0 && errno == EINTR);

    Check(n == 0, "空闲连接已被服务端断开（recv 返回 %zd，期望 0）", n);
    close(idleFd);

    // 关键：服务本身必须还活着
    const int cfd = ConnectClient();
    Check(cfd >= 0, "超时之后仍能建立新连接（服务未被卡死）");
    if (cfd < 0) return;

    Reply reply{};
    const bool ok = Transact(cfd, MakeRequest(Cmd::Info), &reply);
    Check(ok && reply.status == kOk, "新连接能正常处理请求");
    close(cfd);
}

// 长驻服务的稳定性：反复抓帧不能累积 fd
void TestNoFdLeak() {
    printf("\n\033[1;34m[7] fd 泄漏\033[0m  反复抓帧不应累积文件描述符\n");

    constexpr int kIterations = 200;

    // 先跑几轮热身，让各种惰性分配都发生
    for (int i = 0; i < 5; ++i) {
        const int fd = ConnectClient();
        if (fd < 0) { Check(false, "热身心跳连接失败"); return; }
        Reply reply{};
        int frameFd = -1;
        Transact(fd, MakeRequest(Cmd::Capture), &reply, &frameFd);
        if (frameFd >= 0) close(frameFd);
        close(fd);
    }

    const int before = CountOpenFds();
    Check(before > 0, "初始 fd 数量: %d", before);

    int failures = 0;
    for (int i = 0; i < kIterations; ++i) {
        const int fd = ConnectClient();
        if (fd < 0) { ++failures; continue; }

        Reply reply{};
        int frameFd = -1;
        if (!Transact(fd, MakeRequest(Cmd::Capture), &reply, &frameFd)) {
            ++failures;
        }
        // 客户端这一侧的 fd 必须自己关 —— 否则测的是客户端的泄漏
        if (frameFd >= 0) close(frameFd);
        close(fd);
    }

    const int after = CountOpenFds();

    Check(failures == 0, "%d 次抓帧全部成功（失败 %d 次）", kIterations, failures);
    Check(after <= before, "fd 数量没有增长：%d → %d（%+d）",
          before, after, after - before);

    if (after > before) {
        Info("泄漏量 %d 个 —— 一次抓帧经过 pipe/memfd/连接三条路径，"
             "漏一个就意味着跑几小时后 EMFILE", after - before);
    }
}

}  // namespace

// ---------------------------------------------------------------------------

int main() {
    // 把空闲超时压到 1 秒，否则 [6] 用例要等 30 秒
    setenv("AUTOD_IDLE_TIMEOUT_SEC", "1", 1);

    printf("\033[1m=== autod 端到端集成测试 ===\033[0m\n");
    printf("使用真实 Dispatcher / SocketServer / Injector，截图后端为桩。\n");

    if (geteuid() != 0) {
        fprintf(stderr, "\n\033[1;31m需要 root\033[0m（/dev/uinput 默认 0600）\n");
        return 2;
    }

    // ---- 三个组件 ----
    std::string err;

    Capture capture;
    if (!capture.Init(&err)) {
        fprintf(stderr, "Capture::Init 失败: %s\n", err.c_str());
        return 1;
    }

    InjectorConfig cfg;
    cfg.touchWidth  = kWidth;
    cfg.touchHeight = kHeight;
    cfg.deviceName  = kDeviceName;

    Injector injector;
    if (!injector.Init(cfg, &err)) {
        fprintf(stderr, "Injector::Init 失败: %s\n", err.c_str());
        return 1;
    }

    Dispatcher dispatcher(&capture, &injector);

    printf("\n截图后端: %s\n触控后端: %s\n",
           Capture::BackendName(), injector.BackendName());

    // ---- 读回用的 uinput 设备节点 ----
    const std::string node = FindEventNode(kDeviceName);
    if (node.empty()) {
        fprintf(stderr, "找不到设备节点 %s\n", kDeviceName);
        return 1;
    }
    const int readFd = open(node.c_str(), O_RDONLY | O_NONBLOCK);
    if (readFd < 0) {
        fprintf(stderr, "打开 %s 失败: %s\n", node.c_str(), strerror(errno));
        return 1;
    }
    printf("读回节点: %s\n", node.c_str());

    // ---- socket 服务端（后台线程），用真实 Dispatcher ----
    unlink(kSocketPath);
    auto server = SocketServer::FromPath(kSocketPath);
    if (!server.Start(&err)) {
        fprintf(stderr, "SocketServer::Start 失败: %s\n", err.c_str());
        return 1;
    }

    std::thread serverThread([&] {
        server.Run([&](const Request& req, const std::string& payload,
                       int reqFd, int peerUid) {
            return dispatcher.Handle(req, payload, reqFd, peerUid);
        });
    });

    usleep(100000);

    // ---- 客户端 ----
    const int cfd = ConnectClient();
    if (cfd < 0) {
        fprintf(stderr, "连接 %s 失败: %s\n", kSocketPath, strerror(errno));
        server.Stop();
        serverThread.join();
        return 1;
    }

    TestInfo(cfd);
    TestCaptureFrame(cfd);
    TestTapRoundTrip(cfd, readFd);
    TestSwipeRoundTrip(cfd, readFd);
    TestProtocolRobustness(cfd);
    TestIdleTimeout();
    TestNoFdLeak();

    close(cfd);
    close(readFd);

    server.Stop();
    serverThread.join();
    unlink(kSocketPath);

    return Summary("结果");
}
