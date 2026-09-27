// autodctl.cpp — autod 的参考客户端（在设备上运行）
//
// 为什么必须有 C++ 版本：autod 用 SOCK_SEQPACKET + SCM_RIGHTS 传帧，
// 这两样都过不了 `adb forward`（TCP）。所以客户端必须在设备上跑。
//
// 用法：
//   autodctl --socket /data/local/tmp/autod.sock info
//   autodctl --socket /data/local/tmp/autod.sock capture -o /data/local/tmp/shot.png
//   autodctl --socket /data/local/tmp/autod.sock tap 540 1200
//   autodctl --socket /data/local/tmp/autod.sock swipe 540 1600 540 400

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <android/bitmap.h>
#include <android/data_space.h>

#include "../daemon/protocol.h"

using namespace autod;

namespace {

int Connect(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() 失败: %s\n", strerror(errno));
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket 路径过长\n");
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path.c_str(), path.size());

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) 失败: %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// 发请求，收应答。收到 fd 时写入 *outFd。
bool Transact(int sockFd, const Request& req, Reply* reply, int* outFd) {
    if (send(sockFd, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req)) {
        fprintf(stderr, "发送请求失败: %s\n", strerror(errno));
        return false;
    }

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
        n = recvmsg(sockFd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n != static_cast<ssize_t>(sizeof(Reply))) {
        fprintf(stderr, "接收应答失败: %s\n", n < 0 ? strerror(errno) : "长度不符");
        return false;
    }

    *outFd = -1;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
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

bool CheckReply(const Reply& reply) {
    if (reply.magic != kMagic) {
        fprintf(stderr, "应答 magic 不匹配\n");
        return false;
    }
    if (reply.status != kOk) {
        fprintf(stderr, "服务端返回错误: %s (0x%x)\n",
                StatusName(reply.status), reply.status);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 子命令
// ---------------------------------------------------------------------------

int CmdInfo(int sockFd) {
    Reply reply{};
    int frameFd = -1;
    if (!Transact(sockFd, MakeRequest(Cmd::Info), &reply, &frameFd)) return 1;
    if (!CheckReply(reply)) return 1;

    printf("显示数量: 1\n");
    printf("分辨率:   %u x %u\n", reply.width, reply.height);
    printf("format:   0x%x\n", reply.format);
    return 0;
}

int CmdCapture(int sockFd, const char* outPath, bool raw) {
    Request req = MakeRequest(Cmd::Capture);
    req.flags  = raw ? kFlagRawRGBA : kFlagNone;

    Reply reply{};
    int frameFd = -1;
    if (!Transact(sockFd, req, &reply, &frameFd)) return 1;
    if (!CheckReply(reply)) {
        if (frameFd >= 0) close(frameFd);
        return 1;
    }
    if (frameFd < 0) {
        fprintf(stderr, "服务端没有返回帧 fd\n");
        return 1;
    }

    void* base = mmap(nullptr, reply.dataSize, PROT_READ, MAP_SHARED, frameFd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "mmap 失败: %s\n", strerror(errno));
        close(frameFd);
        return 1;
    }

    fprintf(stderr, "截图 %ux%u stride=%u format=0x%x size=%llu\n",
            reply.width, reply.height, reply.stride, reply.format,
            static_cast<unsigned long long>(reply.dataSize));

    int rc = 0;
    if (raw) {
        const int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "打开 %s 失败: %s\n", outPath, strerror(errno));
            rc = 1;
        } else {
            // 写一个 PPM 头方便直接用图片查看器打开
            char header[64];
            const int headerLen = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                                           reply.width, reply.height);
            write(fd, header, headerLen);
            // 丢弃 alpha 通道，PPM 是 3 字节/像素
            const uint8_t* src = static_cast<const uint8_t*>(base);
            for (uint32_t y = 0; y < reply.height; ++y) {
                for (uint32_t x = 0; x < reply.width; ++x) {
                    write(fd, src + (y * reply.width + x) * 4, 3);
                }
            }
            close(fd);
            fprintf(stderr, "已写入 %s (PPM)\n", outPath);
        }
    } else {
        // 用 AndroidBitmap_compress 编码成 PNG，和 AOSP 的 screencap 同一条路
        AndroidBitmapInfo info;
        memset(&info, 0, sizeof(info));
        info.width  = reply.width;
        info.height = reply.height;
        info.stride = reply.stride * 4;
        info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
        info.flags  = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;

        const int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "打开 %s 失败: %s\n", outPath, strerror(errno));
            rc = 1;
        } else {
            const int result = AndroidBitmap_compress(
                    &info, ADATASPACE_SRGB, base,
                    ANDROID_BITMAP_COMPRESS_FORMAT_PNG, 100, &fd,
                    [](void* fdPtr, const void* data, size_t size) -> bool {
                        return write(*static_cast<int*>(fdPtr), data, size) ==
                               static_cast<ssize_t>(size);
                    });
            close(fd);
            if (result != ANDROID_BITMAP_RESULT_SUCCESS) {
                fprintf(stderr, "PNG 编码失败: %d\n", result);
                rc = 1;
            } else {
                fprintf(stderr, "已写入 %s (PNG)\n", outPath);
            }
        }
    }

    munmap(base, reply.dataSize);
    close(frameFd);
    return rc;
}

int CmdTap(int sockFd, int x, int y, uint32_t durationMs) {
    Request req = MakeRequest(Cmd::Tap);
    req.x = x;
    req.y = y;
    req.durationMs = durationMs;

    Reply reply{};
    int frameFd = -1;
    if (!Transact(sockFd, req, &reply, &frameFd)) return 1;
    if (!CheckReply(reply)) return 1;

    printf("已点击 (%d, %d)\n", x, y);
    return 0;
}

int CmdSwipe(int sockFd, int x1, int y1, int x2, int y2, uint32_t durationMs) {
    Request req = MakeRequest(Cmd::Swipe);
    req.x  = x1;
    req.y  = y1;
    req.x2 = x2;
    req.y2 = y2;
    req.durationMs = durationMs;

    Reply reply{};
    int frameFd = -1;
    if (!Transact(sockFd, req, &reply, &frameFd)) return 1;
    if (!CheckReply(reply)) return 1;

    printf("已滑动 (%d,%d) -> (%d,%d)\n", x1, y1, x2, y2);
    return 0;
}

void Usage(const char* argv0) {
    fprintf(stderr, R"(autodctl — autod 客户端

用法: %s --socket <路径> <子命令> [参数]

子命令:
  info                              查询显示参数
  capture [-o 文件] [--raw]         截图（默认 PNG，--raw 输出 PPM）
  tap <x> <y> [--ms N]              单击
  swipe <x1> <y1> <x2> <y2> [--ms N] 滑动

选项:
  --socket <路径>   autod 的 Unix socket 路径（必填）
)", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string socketPath;
    std::string outPath = "/data/local/tmp/shot.png";
    bool raw = false;
    uint32_t ms = 0;

    enum LongOpt { kOptSocket = 1000, kOptOut, kOptRaw, kOptMs };
    static const option kLong[] = {
        {"socket", required_argument, nullptr, kOptSocket},
        {"output", required_argument, nullptr, kOptOut},
        {"raw",    no_argument,       nullptr, kOptRaw},
        {"ms",     required_argument, nullptr, kOptMs},
        {nullptr, 0, nullptr, 0},
    };

    // 先手工扫一遍找 --socket，再解析其余
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "o:h", kLong, nullptr)) != -1) {
        switch (c) {
            case kOptSocket: socketPath = optarg; break;
            case kOptOut:    outPath = optarg;    break;
            case kOptRaw:    raw = true;          break;
            case kOptMs:     ms = strtoul(optarg, nullptr, 10); break;
            default:         break;
        }
    }

    const int remaining = argc - optind;
    if (socketPath.empty() || remaining < 1) {
        Usage(argv[0]);
        return 1;
    }

    const std::string cmd = argv[optind];
    char** args = argv + optind + 1;

    const int sockFd = Connect(socketPath);
    if (sockFd < 0) return 1;

    int rc = 0;
    if (cmd == "info") {
        rc = CmdInfo(sockFd);
    } else if (cmd == "capture") {
        rc = CmdCapture(sockFd, outPath.c_str(), raw);
    } else if (cmd == "tap") {
        if (remaining < 3) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdTap(sockFd, atoi(args[0]), atoi(args[1]), ms);
    } else if (cmd == "swipe") {
        if (remaining < 5) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdSwipe(sockFd, atoi(args[0]), atoi(args[1]),
                      atoi(args[2]), atoi(args[3]), ms);
    } else {
        fprintf(stderr, "未知子命令: %s\n", cmd.c_str());
        Usage(argv[0]);
        rc = 1;
    }

    close(sockFd);
    return rc;
}
