// socket_server.cpp

#include "socket_server.h"

#include <string.h>

#include "thread_util.h"

#include <vector>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include "remote_control_log.h"
#include "remote_control_platform.h"

#if REMOTE_CONTROL_HAS_INIT_SOCKET
#include <cutils/sockets.h>
#endif

namespace remote_control {
namespace {

constexpr size_t kMaxFdsPerMessage = 1;
constexpr int    kBacklog          = 8;

// 连接空闲超时。
//
// 为什么必须有：ServeConnection 是在 accept 循环里**串行**调用的。
// 没有超时的话，一个连上来就不发数据的客户端会让 recvmsg 永久阻塞，
// 整个服务随之卡死（症状："服务还在，但谁来都没反应"）。
//
// 为什么操作仍要串行：Injector 是有状态的（downTime、触控槽位映射），
// 并发注入会互相破坏手势状态。连接可以并发，操作不行。
constexpr int kDefaultIdleTimeoutSec = 30;

// Stop() may be called by a request handler running in one of the detached
// workers (Shutdown/Restart).  That worker must not wait for itself while the
// other workers are being kicked.  Keep the current server/fd in TLS so Stop
// can exclude exactly that connection from its wait.
thread_local SocketServer* gActiveSocketServer = nullptr;
thread_local int gActiveSocketFd = -1;

// 允许用环境变量覆盖，方便测试（不然一个用例要等 30 秒）
// 和运维调优。非法值忽略，回退到默认。
int IdleTimeoutSec() {
    const char* env = getenv("REMOTE_CONTROL_IDLE_TIMEOUT_SEC");
    if (env && *env) {
        const int v = atoi(env);
        if (v > 0 && v <= 3600) return v;
    }
    return kDefaultIdleTimeoutSec;
}

std::string ErrnoString(int e) {
    return std::string(strerror(e)) + " (errno=" + std::to_string(e) + ")";
}

}  // namespace

SocketServer SocketServer::FromInitSocket(const std::string& name) {
    SocketServer s;
    s.initSocketName_ = name;
    return s;
}

SocketServer SocketServer::FromPath(const std::string& path) {
    SocketServer s;
    s.path_ = path;
    return s;
}

SocketServer::SocketServer(SocketServer&& other) noexcept
      : path_(std::move(other.path_)),
        initSocketName_(std::move(other.initSocketName_)),
        listenFd_(other.listenFd_),
        wakeReadFd_(other.wakeReadFd_),
        wakeWriteFd_(other.wakeWriteFd_.load()),
        stop_(other.stop_.load()),
        ownsPath_(other.ownsPath_) {
    // 转移所有权，避免 other 析构时关掉我们正在用的 fd / unlink 路径
    other.listenFd_ = -1;
    other.wakeReadFd_ = -1;
    other.wakeWriteFd_.store(-1);
    other.ownsPath_ = false;
}

SocketServer& SocketServer::operator=(SocketServer&& other) noexcept {
    if (this != &other) {
        Stop();
        if (listenFd_ >= 0) close(listenFd_);
        if (wakeReadFd_ >= 0) close(wakeReadFd_);
        const int oldWakeWrite = wakeWriteFd_.exchange(-1);
        if (oldWakeWrite >= 0) close(oldWakeWrite);
        if (ownsPath_ && !path_.empty()) unlink(path_.c_str());

        path_           = std::move(other.path_);
        initSocketName_ = std::move(other.initSocketName_);
        listenFd_       = other.listenFd_;
        wakeReadFd_     = other.wakeReadFd_;
        wakeWriteFd_.store(other.wakeWriteFd_.load());
        stop_.store(other.stop_.load());
        ownsPath_       = other.ownsPath_;

        other.listenFd_ = -1;
        other.wakeReadFd_ = -1;
        other.wakeWriteFd_.store(-1);
        other.ownsPath_ = false;
    }
    return *this;
}

SocketServer::~SocketServer() {
    Stop();
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    if (wakeReadFd_ >= 0) {
        close(wakeReadFd_);
        wakeReadFd_ = -1;
    }
    const int wakeWrite = wakeWriteFd_.exchange(-1);
    if (wakeWrite >= 0) close(wakeWrite);
    if (ownsPath_ && !path_.empty()) {
        unlink(path_.c_str());
    }
}

bool SocketServer::Start(std::string* error) {
    // --- 模式 1：init 已创建好 socket，通过环境变量传 fd ---
    if (!initSocketName_.empty()) {
#if REMOTE_CONTROL_HAS_INIT_SOCKET
        // android_get_control_socket 读 ANDROID_SOCKET_<name>
        listenFd_ = android_get_control_socket(initSocketName_.c_str());
        if (listenFd_ < 0) {
            if (error) {
                *error = "android_get_control_socket(" + initSocketName_ +
                         ") failed; 检查 remote-control.rc 里是否有 'socket " +
                         initSocketName_ + " ...'";
            }
            return false;
        }
        // init 已经 listen 过了，这里不要再 listen
        path_ = std::string("/dev/socket/") + initSocketName_;
        ALOGI("remote-control: 接管 init socket fd=%d (%s)", listenFd_, path_.c_str());
        if (!CreateWakePipe(error)) {
            close(listenFd_);
            listenFd_ = -1;
            return false;
        }
        return true;
#else
        // init socket activation 是 Android 专有机制（libcutils）。
        // 主机上（单元测试）只能用 FromPath 模式。
        if (error) {
            *error = "init socket activation 仅在 Android 上可用，"
                     "主机测试请用 FromPath()";
        }
        return false;
#endif
    }

    // --- 模式 2：自己 bind 一个路径 ---
    if (path_.empty()) {
        if (error) *error = "既没有 init socket 名，也没有 socket 路径";
        return false;
    }
    if (path_.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error) *error = "socket 路径过长: " + path_;
        return false;
    }

    listenFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        if (error) *error = "socket() 失败: " + ErrnoString(errno);
        return false;
    }

    // 路径可能残留自上次异常退出
    unlink(path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path_.c_str(), path_.size());

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error) *error = "bind(" + path_ + ") 失败: " + ErrnoString(errno);
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 只允许属主和同组访问。调用方 UID 还会在 ServeConnection 里二次校验。
    // 默认 0660：属主与同组可访问。要放宽用 --socket-mode（例如让上位应用
    // 以自己的 UID 连进来）。放宽的代价是同一台设备上任何进程都能控制服务，
    // 所以默认保守，由部署方显式决定。
    if (chmod(path_.c_str(), socketMode_) < 0) {
        ALOGW("remote-control: chmod(%s, %04o) 失败: %s", path_.c_str(),
              static_cast<unsigned>(socketMode_), strerror(errno));
    }

    if (listen(listenFd_, kBacklog) < 0) {
        if (error) *error = "listen() 失败: " + ErrnoString(errno);
        close(listenFd_);
        listenFd_ = -1;
        unlink(path_.c_str());
        return false;
    }

    ownsPath_ = true;
    ALOGI("remote-control: 监听 %s (fd=%d)", path_.c_str(), listenFd_);
    if (!CreateWakePipe(error)) {
        close(listenFd_);
        listenFd_ = -1;
        unlink(path_.c_str());
        ownsPath_ = false;
        return false;
    }
    return true;
}

bool SocketServer::CreateWakePipe(std::string* error) {
    int fds[2] = {-1, -1};
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        if (error) *error = std::string("创建停止唤醒管道失败: ") + strerror(errno);
        return false;
    }
    wakeReadFd_ = fds[0];
    wakeWriteFd_.store(fds[1]);
    return true;
}

void SocketServer::SignalStop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    const int fd = wakeWriteFd_.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const char byte = 1;
        // write() is async-signal-safe; a full non-blocking pipe already
        // contains a wakeup, so EAGAIN can be ignored.
        (void)write(fd, &byte, sizeof(byte));
    }
}

void SocketServer::Stop() {
    SignalStop();
    const int callerFd = gActiveSocketServer == this ? gActiveSocketFd : -1;

    // 先关监听，再 shutdown 所有活跃连接。仅关监听只能让 accept 返回，
    // detached worker 仍可能阻塞在 recvmsg；主线程若随后析构 Dispatcher，
    // worker 就会继续使用悬空的 handler/stop_。
    std::vector<int> peers;
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        if (listenFd_ >= 0) {
            // shutdown 让阻塞中的 accept 返回，避免依赖超时轮询。
            shutdown(listenFd_, SHUT_RDWR);
            close(listenFd_);
            listenFd_ = -1;
        }
        peers.reserve(connFds_.size());
        for (int fd : connFds_) {
            if (fd != callerFd) peers.push_back(fd);
        }
    }
    for (int fd : peers) shutdown(fd, SHUT_RDWR);

    // 如果 Stop 是由某个 worker 的 handler 调用，不能在这里等待其它
    // worker：两个并发的 Shutdown 请求会各自等待对方而死锁。调用方
    // 返回 handler 后由主线程再次 Stop()，统一等待所有连接退出。
    if (callerFd >= 0) return;

    // 非 worker 调用方（通常是 Run() 返回后的主线程）等待所有 worker
    // 注销连接，确保随后销毁 handler/Dispatcher 时没有悬空访问。
    std::unique_lock<std::mutex> lk(connMutex_);
    connCv_.wait(lk, [this] { return connFds_.empty(); });
}

void SocketServer::Run(RequestHandler handler) {
    if (listenFd_ < 0) {
        ALOGE("remote-control: Run() 在 Start() 之前被调用");
        return;
    }

    // 连续失败计数：accept 失败如果**立刻**再来一次，就是一个 100% CPU 的
    // 死循环 —— 实测踩过：接管 init 的 socket 时 accept 一直返回 EINVAL，
    // 刷屏 + 把 HTTP 线程一起饿死，表现为"服务在跑但控制台打不开"，
    // 而且日志刷得太快，真正的原因反而看不见。
    //
    // 所以：同一种错误连续出现就退避，并只报一次"这条路走不通"，
    // 让失败**显式**而不是变成饥饿。
    int consecutiveErrors = 0;
    while (!stop_.load()) {
        pollfd fds[2]{};
        fds[0].fd = listenFd_;
        fds[0].events = POLLIN;
        fds[1].fd = wakeReadFd_;
        fds[1].events = POLLIN;
        int pollRc;
        do {
            pollRc = poll(fds, 2, -1);
        } while (pollRc < 0 && errno == EINTR);
        if (pollRc < 0 || stop_.load()) break;
        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(wakeReadFd_, drain, sizeof(drain)) > 0) {}
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            continue;
        }

        int connFd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (connFd < 0) {
            if (errno == EINTR) continue;
            if (stop_.load()) break;
            if (++consecutiveErrors <= 3) {
                ALOGE("remote-control: accept 失败: %s", strerror(errno));
            } else if (consecutiveErrors == 4) {
                ALOGE("remote-control: accept 持续失败（%s），退避等待；"
                      "这个监听 fd 用不了 —— 若用的是 --init-socket，"
                      "见 remote-control.rc 里为什么改成自己 bind",
                      strerror(errno));
            }
            // 退避：连续失败时不要空转
            usleep(200 * 1000);
            continue;
        }
        consecutiveErrors = 0;

        // 在 Stop() 和连接 worker 之间建立先后关系：Stop 取得锁后会等待
        // connFds_ 为空，所以必须先登记再启动线程；否则 Stop 可能已经返回，
        // 主线程随即析构服务对象，而这个刚接收的连接才开始访问 this。
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            if (!stop_.load()) {
                connFds_.insert(connFd);
                accepted = true;
            }
        }
        if (!accepted) {
            close(connFd);
            break;
        }

        // 给连接设个空闲超时 —— 防止连上来就不说话的客户端永久占着线程。
        timeval tv{};
        tv.tv_sec  = IdleTimeoutSec();
        tv.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 每个连接一个线程。
        //
        // ⚠️ 这里曾经是串行调用（ServeConnection 直接在 accept 循环里跑），
        //    后果是**一个长连接的客户端会把整个服务占住**：
        //      - 上位应用连上后处于空闲，其它客户端连不进来
        //      - 空闲超时一到，服务端把应用那条连接关掉，
        //        而应用并不知道，下次写入直接 Broken pipe
        //    实测就是这么暴露的。
        //
        //    真正的互斥放在 handler 里（见 main.cpp）：连接可以并发，
        //    但 Injector 是有状态的（按下/抬起、槽位映射），
        //    并发注入会互相破坏手势，所以**操作**必须串行。
        // 线程起不来就退回串行，至少不丢连接。
        // SpawnDetached 用 pthread_create，失败返回 false 而不是抛异常 ——
        // AOSP 是 -fno-exceptions，std::thread 抛出来就是整个进程 terminate。
        auto serve = [this, connFd, handler]() mutable {
            SocketServer* previousServer = gActiveSocketServer;
            const int previousFd = gActiveSocketFd;
            gActiveSocketServer = this;
            gActiveSocketFd = connFd;
            ServeConnection(connFd, handler);
            gActiveSocketServer = previousServer;
            gActiveSocketFd = previousFd;
            close(connFd);
            {
                std::lock_guard<std::mutex> lk(connMutex_);
                connFds_.erase(connFd);
                connCv_.notify_all();
            }
        };
        // Keep the local callable intact so the serial fallback below still
        // has a valid handler when thread creation fails.
        if (!SpawnDetached(serve)) {
            ALOGW("remote-control: 起线程失败，本连接串行处理");
            // 线程创建失败时仍然走同一个 wrapper，保证连接从集合中
            // 注销并唤醒 Stop() 的等待者。
            serve();
        }
    }
    ALOGI("remote-control: accept 循环退出");
}

void SocketServer::ServeConnection(int connFd, const RequestHandler& handler) {
    // --- 校验调用方 UID ---
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        ALOGI("remote-control: 新连接 pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
        // TODO(鉴权): 在这里按 cred.uid / cred.pid 做白名单。
        //   当前仅依赖 socket 文件权限 (0660) 做粗粒度控制。
        //   生产环境建议：
        //     - 建一个专用的 AID（如 AID_REMOTE_CONTROL），客户端进程加入该组
        //     - 或校验 cred.uid 是否在允许列表内，否则直接拒绝
    }

    while (!stop_.load()) {
        Request req{};
        std::string payload;
        int reqFd = -1;
        const int rc = RecvRequest(connFd, &req, &payload, &reqFd);
        if (rc < 0) break;          // 对端正常关闭
        if (rc > 0) {
            ALOGW("remote-control: 读请求失败: %s", strerror(rc));
            break;
        }

        ReplyPacket packet = handler(req, payload, reqFd, cred.uid);
        if (reqFd >= 0) close(reqFd);   // handler 若需要保留会自己 dup

        if (!SendReply(connFd, packet)) {
            ALOGW("remote-control: 发应答失败: %s", strerror(errno));
            if (packet.fd >= 0) close(packet.fd);
            break;
        }
        if (packet.fd >= 0) {
            close(packet.fd);   // SCM_RIGHTS 已经让内核复制了一份
        }
    }
}

int SocketServer::RecvRequest(int connFd, Request* out, std::string* payload,
                              int* outFd) {
    payload->clear();
    *outFd = -1;

    // 一次读「头 + payload」。SEQPACKET 有消息边界，多读的部分就是 payload，
    // 不需要长度前缀。缓冲区按上限一次备好，超长消息会被内核截断并设置
    // MSG_TRUNC；下面在发现该标志时拒绝整条消息。
    std::vector<char> buf(sizeof(Request) + kMaxRequestPayload);
    iovec iov{};
    iov.iov_base = buf.data();
    iov.iov_len  = buf.size();

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    // 客户端可能传 fd（InstallApp 用 memfd 送 APK），所以这里要能收。
    // 其余命令收到 fd 一律关闭，防止泄漏。
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)]{};
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(connFd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    int receivedFd = -1;
    if (n >= 0) {
        // 收到的 fd 必须先接管，再处理短包/控制数据截断等错误返回；
        // 否则内核已安装到本进程的 SCM_RIGHTS fd 会泄漏。
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS ||
                cmsg->cmsg_len < CMSG_LEN(0)) {
                continue;
            }
            const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < count; ++i) {
                if (receivedFd < 0) {
                    receivedFd = fds[i];
                } else {
                    ALOGW("remote-control: 一次只接受一个 fd，丢弃 fd=%d", fds[i]);
                    close(fds[i]);
                }
            }
        }
    }
    struct ReceivedFdGuard {
        int* fd;
        ~ReceivedFdGuard() { if (*fd >= 0) close(*fd); }
    } receivedFdGuard{&receivedFd};

    if (n == 0) return -1;                       // 对端正常关闭

    // SO_RCVTIMEO 到期会返回 EAGAIN/EWOULDBLOCK —— 对端连上但不发数据。
    // 单独报出来，否则日志里只会看到一句含糊的"读请求失败"。
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ALOGW("remote-control: 连接空闲超过 %d 秒，主动断开", IdleTimeoutSec());
        return -1;
    }

    if (n < 0) return errno;
    // SOCK_SEQPACKET 在消息大于接收缓冲区时会保留消息边界，但只把前缀
    // 拷进 buf，并通过 MSG_TRUNC 标记丢弃了剩余内容。不能把这个前缀当成
    // 合法请求交给 handler，否则超长 payload 会被静默截断，既可能绕过
    // 参数校验，也会让客户端误以为整条请求已处理。
    if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) return EMSGSIZE;
    if (static_cast<size_t>(n) < sizeof(Request)) return EMSGSIZE;

    memcpy(out, buf.data(), sizeof(Request));
    const size_t payloadLen = static_cast<size_t>(n) - sizeof(Request);
    if (payloadLen > 0) payload->assign(buf.data() + sizeof(Request), payloadLen);
    *outFd = receivedFd;
    receivedFd = -1;
    return 0;
}

bool SocketServer::SendReply(int connFd, const ReplyPacket& packet) {
    Reply reply = packet.reply;

    iovec iov{};
    iov.iov_base = &reply;
    iov.iov_len  = sizeof(Reply);

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    if (packet.fd >= 0) {
        msg.msg_control    = control;
        msg.msg_controllen = sizeof(control);

        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &packet.fd, sizeof(int));
    }

    ssize_t n;
    do {
        n = sendmsg(connFd, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    return n == static_cast<ssize_t>(sizeof(Reply));
}

}  // namespace remote_control
