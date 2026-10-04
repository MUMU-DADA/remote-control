// test_transport.cpp — HTTP / Unix socket transport lifecycle regressions

#include "http_server.h"
#include "socket_server.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace remote_control;
using remote_control_test::Check;

namespace {

int ConnectHttp(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

std::string ReadToClose(int fd) {
    std::string result;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

bool SendAll(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

size_t CountOpenFds() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return 0;
    size_t count = 0;
    while (readdir(dir) != nullptr) ++count;
    closedir(dir);
    // readdir includes "." and "..".
    return count >= 2 ? count - 2 : 0;
}

void RemoveDirectoryContents(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string child = path + "/" + entry->d_name;
        unlink(child.c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}

void TestHttpConnectionReservationAndStop() {
    printf("\n\033[1;34m[1] HTTP 连接上限与 Stop\033[0m\n");
    HttpServer server;
    HttpServer::Options options;
    options.port = 0;
    options.maxConns = 1;
    std::string error;
    if (!server.Start(options, &error)) {
        Check(false, "启动 HTTP server: %s", error.c_str());
        return;
    }
    HttpHandler handler = [](const HttpRequest&) {
        return HttpResponse::Text(200, "unexpected");
    };
    std::thread runner([&]() { server.Run(handler); });

    const int first = ConnectHttp(server.port());
    const int second = ConnectHttp(server.port());
    if (first < 0 || second < 0) {
        Check(false, "连接到本地 HTTP server");
        if (first >= 0) close(first);
        if (second >= 0) close(second);
        server.Stop();
        runner.join();
        return;
    }

    const std::string rejected = ReadToClose(second);
    Check(rejected.find("503 Service Unavailable") != std::string::npos,
          "未发送请求头的连接计入 maxConns");

    server.Stop();
    char byte = 0;
    const ssize_t firstRead = read(first, &byte, 1);
    Check(firstRead <= 0, "Stop 关闭尚未发送请求头的连接");
    close(first);
    close(second);
    runner.join();
}

void TestHttpStopWaitsForHandler() {
    printf("\n\033[1;34m[2] HTTP Stop 等待正在运行的处理器\033[0m\n");
    HttpServer server;
    HttpServer::Options options;
    options.port = 0;
    std::string error;
    if (!server.Start(options, &error)) {
        Check(false, "启动 HTTP server: %s", error.c_str());
        return;
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    HttpHandler handler = [&](const HttpRequest&) {
        entered.store(true);
        usleep(800 * 1000);
        finished.store(true);
        return HttpResponse::Text(200, "done");
    };
    std::thread runner([&]() { server.Run(handler); });
    const int fd = ConnectHttp(server.port());
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const bool sent = fd >= 0 && SendAll(fd, request, sizeof(request) - 1);
    for (int i = 0; sent && i < 100 && !entered.load(); ++i) usleep(1000);
    Check(sent && entered.load(), "请求已进入处理器");
    server.Stop();
    Check(finished.load(), "Stop 返回前等待处理器完成");
    if (fd >= 0) close(fd);
    runner.join();
}

void TestHttpStopFromHandler() {
    printf("\n\033[1;34m[3] 从 handler 内调用 Stop\033[0m\n");
    HttpServer server;
    HttpServer::Options options;
    options.port = 0;
    std::string error;
    if (!server.Start(options, &error)) {
        Check(false, "启动 HTTP server: %s", error.c_str());
        return;
    }

    std::atomic<bool> stopReturned{false};
    HttpHandler handler = [&](const HttpRequest&) {
        server.Stop();
        stopReturned.store(true);
        return HttpResponse::Text(200, "stopped");
    };
    std::thread runner([&]() { server.Run(handler); });
    const int fd = ConnectHttp(server.port());
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const bool sent = fd >= 0 && SendAll(fd, request, sizeof(request) - 1);
    for (int i = 0; sent && i < 2000 && !stopReturned.load(); ++i) usleep(1000);
    Check(sent && stopReturned.load(), "handler 调用 Stop 后能继续返回");
    if (!stopReturned.load()) _Exit(1);

    const std::string response = ReadToClose(fd);
    Check(response.find("HTTP/1.1 200 OK") != std::string::npos &&
                  response.find("stopped") != std::string::npos,
          "Stop 后当前请求仍完成响应");
    if (fd >= 0) close(fd);
    runner.join();
}

void TestHttpSpoolCleanup() {
    printf("\n\033[1;34m[4] HTTP spool 生命周期\033[0m\n");
    char dirTemplate[] = "/tmp/remote-control-transport-XXXXXX";
    char* dirName = mkdtemp(dirTemplate);
    if (!dirName) {
        Check(false, "创建 spool 临时目录: %s", strerror(errno));
        return;
    }
    const std::string spoolDir(dirName);

    HttpServer server;
    HttpServer::Options options;
    options.port = 0;
    options.maxBodyBytes = 1024 * 1024;
    options.spoolThresholdBytes = 32;
    options.spoolDir = spoolDir;
    std::string error;
    if (!server.Start(options, &error)) {
        Check(false, "启动 HTTP server: %s", error.c_str());
        RemoveDirectoryContents(spoolDir);
        return;
    }

    HttpHandler handler = [](const HttpRequest& req) {
        struct stat st{};
        const bool stored = !req.bodyFile.empty() &&
                            stat(req.bodyFile.c_str(), &st) == 0 &&
                            static_cast<size_t>(st.st_size) == req.bodySize &&
                            req.body.empty();
        return HttpResponse::Text(stored ? 200 : 500,
                                  stored ? "spooled" : "missing");
    };
    std::thread runner([&]() { server.Run(handler); });

    const int fd = ConnectHttp(server.port());
    if (fd < 0) {
        Check(false, "连接到 spool 测试 HTTP server");
        server.Stop();
        runner.join();
        RemoveDirectoryContents(spoolDir);
        return;
    }

    const std::string body(128 * 1024, 'x');
    const std::string request = std::string(
            "POST /upload HTTP/1.1\r\nHost: localhost\r\n") +
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    // 把 Content-Length 之外的字节也放在同一次写入里，覆盖头部 read
    // 一并读到流水线数据时的落盘边界：多出来的字节不能进入 bodyFile。
    const std::string extra = "EXTRA_PIPELINED_BYTES";
    const std::string wire = request + body + extra;
    const bool requestSent = SendAll(fd, wire.data(), wire.size());
    const std::string response = ReadToClose(fd);
    close(fd);

    Check(requestSent, "发送大请求体");
    Check(response.find("200 OK") != std::string::npos &&
                  response.find("spooled") != std::string::npos,
          "handler 可读取已落盘请求体");
    DIR* dir = opendir(spoolDir.c_str());
    size_t entries = 0;
    if (dir) {
        while (dirent* entry = readdir(dir)) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                ++entries;
            }
        }
        closedir(dir);
    }
    Check(dir != nullptr && entries == 0,
          "请求处理完成后删除 spool 文件");

    server.Stop();
    runner.join();
    RemoveDirectoryContents(spoolDir);
}

void TestHttpContentLengthValidation() {
    printf("\n\033[1;34m[5] HTTP Content-Length 校验\033[0m\n");
    HttpServer server;
    HttpServer::Options options;
    options.port = 0;
    options.maxBodyBytes = 1024;
    std::string error;
    if (!server.Start(options, &error)) {
        Check(false, "启动 HTTP server: %s", error.c_str());
        return;
    }
    std::thread runner([&]() { server.Run([](const HttpRequest&) {
        return HttpResponse::Text(200, "handled");
    }); });

    const std::string overflow =
            std::to_string(std::numeric_limits<size_t>::max()) + "0";
    const std::vector<std::string> invalid = {
        "Content-Length: 1x\r\n",
        "Content-Length: +1\r\n",
        "Content-Length: " + overflow + "\r\n",
        "Content-Length: 0\r\nContent-Length: 0\r\n",
    };
    for (const std::string& header : invalid) {
        const int fd = ConnectHttp(server.port());
        if (fd < 0) {
            Check(false, "连接到 Content-Length 校验 server");
            continue;
        }
        const std::string request = "POST / HTTP/1.1\r\nHost: localhost\r\n" +
                                    header + "\r\n";
        const bool sent = SendAll(fd, request.data(), request.size());
        const std::string response = ReadToClose(fd);
        close(fd);
        Check(sent && response.find("400 Bad Request") != std::string::npos,
              "拒绝非法或重复 Content-Length");
    }

    server.Stop();
    runner.join();
}

void TestUnixShortPacketFdCleanup() {
    printf("\n\033[1;34m[6] Unix socket 短包 fd 清理\033[0m\n");
    char pathTemplate[] = "/tmp/remote-control-transport-sock-XXXXXX";
    const int tempFd = mkstemp(pathTemplate);
    if (tempFd < 0) {
        Check(false, "创建 socket 路径: %s", strerror(errno));
        return;
    }
    close(tempFd);
    unlink(pathTemplate);
    const std::string path(pathTemplate);

    SocketServer server = SocketServer::FromPath(path);
    std::string error;
    if (!server.Start(&error)) {
        Check(false, "启动 Unix socket server: %s", error.c_str());
        unlink(path.c_str());
        return;
    }
    std::atomic<int> handled{0};
    RequestHandler handler = [&](const Request&, const std::string&, int, int) {
        ++handled;
        return ReplyPacket{};
    };
    std::thread runner([&]() { server.Run(handler); });

    const int peer = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (peer < 0 || connect(peer, reinterpret_cast<sockaddr*>(&addr),
                            sizeof(addr)) != 0) {
        Check(false, "连接 Unix socket server");
        if (peer >= 0) close(peer);
        server.Stop();
        runner.join();
        return;
    }
    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    Request validRequest{};
    const ssize_t validSent = send(peer, &validRequest, sizeof(validRequest), 0);
    Reply validReply{};
    size_t replyBytes = 0;
    while (replyBytes < sizeof(validReply)) {
        const ssize_t n = read(peer,
                               reinterpret_cast<char*>(&validReply) + replyBytes,
                               sizeof(validReply) - replyBytes);
        if (n <= 0) break;
        replyBytes += static_cast<size_t>(n);
    }
    Check(validSent == static_cast<ssize_t>(sizeof(validRequest)) &&
                  replyBytes == sizeof(validReply),
          "有效请求保持连接并返回应答");

    // SOCK_SEQPACKET 对超出接收缓冲区的消息返回前缀并设置 MSG_TRUNC；
    // 服务端必须拒绝整条消息，不能把截断 payload 交给 handler。
    const int oversizedPeer = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    bool oversizedConnected = oversizedPeer >= 0 &&
            connect(oversizedPeer, reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)) == 0;
    if (!oversizedConnected) {
        Check(false, "连接 Unix socket server（超长请求）");
        if (oversizedPeer >= 0) close(oversizedPeer);
    } else {
        timeval oversizedTimeout{};
        oversizedTimeout.tv_sec = 3;
        setsockopt(oversizedPeer, SOL_SOCKET, SO_RCVTIMEO,
                   &oversizedTimeout, sizeof(oversizedTimeout));
        std::vector<char> oversized(sizeof(Request) + kMaxRequestPayload + 1);
        memcpy(oversized.data(), &validRequest, sizeof(validRequest));
        const ssize_t oversizedSent = send(oversizedPeer, oversized.data(),
                                           oversized.size(), 0);
        char byte = 0;
        const ssize_t oversizedRead = read(oversizedPeer, &byte, 1);
        Check(oversizedSent == static_cast<ssize_t>(oversized.size()),
              "发送超长请求");
        Check(oversizedRead == 0, "服务端拒绝带 MSG_TRUNC 的请求");
        Check(handled.load() == 1, "超长请求不会进入请求处理器");
        close(oversizedPeer);
    }

    int pipeFds[2] = {-1, -1};
    if (pipe(pipeFds) != 0) {
        Check(false, "创建用于传递的 pipe fd");
        close(peer);
        server.Stop();
        runner.join();
        return;
    }
    const size_t baseline = CountOpenFds();
    const char payload[] = "bad";
    iovec iov{};
    iov.iov_base = const_cast<char*>(payload);
    iov.iov_len = sizeof(payload);
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &pipeFds[0], sizeof(int));
    const ssize_t sent = sendmsg(peer, &msg, 0);
    close(pipeFds[0]);
    close(pipeFds[1]);

    char byte = 0;
    const ssize_t peerRead = read(peer, &byte, 1);
    // EOF means the server has completed handling this packet and closed its
    // accepted socket; the local pipe endpoints account for two baseline fds.
    const size_t after = CountOpenFds();
    Check(sent == static_cast<ssize_t>(sizeof(payload)),
          "发送带 SCM_RIGHTS 的短包");
    Check(peerRead == 0, "服务端拒绝短包并关闭连接");
    Check(baseline >= 3 && after == baseline - 3,
          "短包中的 SCM_RIGHTS fd 未泄漏（基线 %zu，结束 %zu）",
          baseline, after);
    Check(handled.load() == 1, "短包不会进入请求处理器");

    close(peer);
    server.Stop();
    runner.join();
}

}  // namespace

int main() {
    printf("\033[1m=== Transport lifecycle regression tests ===\033[0m\n");
    TestHttpConnectionReservationAndStop();
    TestHttpStopWaitsForHandler();
    TestHttpStopFromHandler();
    TestHttpSpoolCleanup();
    TestHttpContentLengthValidation();
    TestUnixShortPacketFdCleanup();
    return remote_control_test::Summary("传输层生命周期");
}
