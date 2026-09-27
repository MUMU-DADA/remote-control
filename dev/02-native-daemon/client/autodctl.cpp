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

// AndroidBitmap_compress 是 Android 专有 API。
// 主机上（编译期自检、联调）退回到写 PPM。
#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/data_space.h>
#endif

#include <string>
#include <vector>

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
        // 不能是 const —— AndroidBitmap_compress 的第 6 个参数是 void*，
        // 传 &fd 时 const int* 无法隐式转换（NDK 构建实测踩到）。
        int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
#ifdef __ANDROID__
        // 用 AndroidBitmap_compress 编码成 PNG，和 AOSP 的 screencap 同一条路
        AndroidBitmapInfo info;
        memset(&info, 0, sizeof(info));
        info.width  = reply.width;
        info.height = reply.height;
        info.stride = reply.stride * 4;
        info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
        info.flags  = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;

        // ⚠️ 不能是 const —— AndroidBitmap_compress 的第 6 个参数是 void*，
        //    传 &fd（const int*）无法隐式转换。NDK 构建实测踩到，
        //    AOSP 构建同样会失败。
        int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
#else
        // 主机上没有 AndroidBitmap_compress，退回到 PPM。
        // 这条分支只为让本文件在开发机上能编译自检 —— 设备上永远走上面的 PNG 路径。
        fprintf(stderr,
                "提示: 主机构建无 PNG 编码，改为输出 PPM\n");
        const int fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "打开 %s 失败: %s\n", outPath, strerror(errno));
            rc = 1;
        } else {
            char header[64];
            const int headerLen = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                                           reply.width, reply.height);
            if (write(fd, header, static_cast<size_t>(headerLen)) < 0) rc = 1;
            const auto* src = static_cast<const uint8_t*>(base);
            std::vector<uint8_t> row(static_cast<size_t>(reply.width) * 3);
            for (uint32_t yy = 0; yy < reply.height && rc == 0; ++yy) {
                for (uint32_t xx = 0; xx < reply.width; ++xx) {
                    memcpy(&row[xx * 3], src + (static_cast<size_t>(yy) * reply.width + xx) * 4, 3);
                }
                if (write(fd, row.data(), row.size()) < 0) rc = 1;
            }
            close(fd);
            if (rc == 0) fprintf(stderr, "已写入 %s (PPM)\n", outPath);
        }
#endif
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

应用管理:
  list-apps [--system] [--meta] [--names]   列出应用
  app-info <包名>                           详情（含权限与组件清单）
  launch <包名> [Activity]                  启动
  kill <包名>                               强制停止
  foreground                                当前前台应用
  install <apk> [--no-replace]              安装（APK 内容经 memfd 传输）

文件与下载（路径均相对下载目录，越界会被拒绝）:
  download <url> [文件名] [子目录]          下载到下载目录
  ls [路径]                                 列出目录
  stat <路径>                               文件信息
  mkdir [-p] <路径>                         创建目录
  rm [-r] <路径>                            删除
  mv <源> <目标>                            重命名/移动

服务自身（v3）:
  describe                                  能力清单：有哪些命令、哪些可用
  config                                    当前配置与运行时状态
  set-config <key> <value> [...]            热改配置
  selftest                                  环境自检（有副作用：会抓帧、建设备）
  stats                                     运行统计
  log [sinceSeq]                            取最近日志（增量拉取）
  shutdown                                  优雅退出
  restart                                   退出并由 init 重启（退出码 1）

选项:
  --socket <路径>   autod 的 Unix socket 路径（必填）
)", argv0);
}

}  // namespace


// ---------------------------------------------------------------------------
// v2：应用与文件管理
// ---------------------------------------------------------------------------

// 把参数拼成 NUL 分隔的 payload（协议约定，见 protocol.h）
// 取第一个不以 '-' 开头的参数。
//
// 子命令的参数是"选项 + 位置参数"混排的（`mkdir -p <路径>`），
// 不能直接拿 args[0] —— 那样 `-p` 会被当成路径，
// 实测真的建出了一个名叫 "-p" 的目录。
const char* FirstPositional(char** args, int count) {
    for (int i = 0; i < count; ++i) {
        if (args[i] != nullptr && args[i][0] != '-') return args[i];
    }
    return nullptr;
}

std::string BuildPayload(std::initializer_list<const char*> parts) {
    std::string out;
    bool first = true;
    for (const char* p : parts) {
        if (p == nullptr) continue;          // 跳过可选的尾参数
        if (!first) out.push_back('\0');
        out += p;
        first = false;
    }
    return out;
}

// 发一条 v2 命令，收 JSON 应答。
//
// 应答的 JSON 走 memfd + SCM_RIGHTS（和截图同一个通道）—— 这样没有大小限制，
// 应用列表几十 KB 也能回。
bool TransactV2(int sockFd, Cmd cmd, const std::string& payload, uint32_t flags,
                int passFd, std::string* jsonOut, Reply* replyOut) {
    std::vector<char> msg(sizeof(Request) + payload.size());
    Request req = MakeRequest(cmd);
    req.flags = flags;
    memcpy(msg.data(), &req, sizeof(Request));
    if (!payload.empty()) {
        memcpy(msg.data() + sizeof(Request), payload.data(), payload.size());
    }

    ssize_t sent;
    if (passFd >= 0) {
        iovec iov{msg.data(), msg.size()};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr m{};
        m.msg_iov        = &iov;
        m.msg_iovlen     = 1;
        m.msg_control    = control;
        m.msg_controllen = sizeof(control);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&m);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &passFd, sizeof(int));
        m.msg_controllen = cmsg->cmsg_len;
        sent = sendmsg(sockFd, &m, MSG_NOSIGNAL);
    } else {
        sent = send(sockFd, msg.data(), msg.size(), MSG_NOSIGNAL);
    }
    if (sent != static_cast<ssize_t>(msg.size())) {
        fprintf(stderr, "发送失败: %s\n", strerror(errno));
        return false;
    }

    iovec iov{};
    iov.iov_base = replyOut;
    iov.iov_len  = sizeof(Reply);
    msghdr m{};
    m.msg_iov    = &iov;
    m.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    m.msg_control    = control;
    m.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(sockFd, &m, 0);
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(sizeof(Reply))) {
        fprintf(stderr, "接收应答失败: %s\n", n < 0 ? strerror(errno) : "长度不符");
        return false;
    }

    jsonOut->clear();
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&m); cmsg; cmsg = CMSG_NXTHDR(&m, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        int fd = -1;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        if (fd < 0) continue;

        // 顺序读，不依赖 st_size —— 服务端可能用管道
        char buf[16384];
        ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0) jsonOut->append(buf, r);
        close(fd);
    }
    return true;
}

// 简易 JSON 缩进。没有引库，就自己按字符走一遍：
// 只在字符串之外对 { } [ ] , 做换行缩进。
std::string PrettyJson(const std::string& in, int indentStep = 2) {
    std::string out;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    auto newline = [&](int d) {
        out.push_back('\n');
        out.append(static_cast<size_t>(d * indentStep), ' ');
    };
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (inStr) {
            out.push_back(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        switch (c) {
            case '"': inStr = true; out.push_back(c); break;
            case '{': case '[':
                out.push_back(c);
                ++depth;
                if (i + 1 < in.size() && in[i + 1] != '}' && in[i + 1] != ']') newline(depth);
                break;
            case '}': case ']':
                --depth;
                if (i > 0 && in[i - 1] != '{' && in[i - 1] != '[') newline(depth);
                out.push_back(c);
                break;
            case ',':
                out.push_back(c);
                newline(depth);
                break;
            case ':':
                out += ": ";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

// 通用：发一条 v2 命令并把 JSON 打出来
int CmdV2(int sockFd, Cmd cmd, const std::string& payload, uint32_t flags,
          bool compact = false) {
    Reply reply{};
    std::string json;
    if (!TransactV2(sockFd, cmd, payload, flags, -1, &json, &reply)) return 1;

    bool ok = reply.status == kOk;
    if (!json.empty()) {
        printf("%s\n", compact ? json.c_str() : PrettyJson(json).c_str());
    }
    if (!ok) {
        // v2 应答即使失败也带 JSON（里面有服务端给的原因），已经打出来了
        fprintf(stderr, "服务端返回: %s (0x%x)\n",
                StatusName(reply.status), reply.status);
        return 1;
    }
    return 0;
}

// install：把 APK 读进 memfd 送过去
int CmdInstall(int sockFd, const char* apkPath, bool replace) {
    const int fileFd = open(apkPath, O_RDONLY | O_CLOEXEC);
    if (fileFd < 0) {
        fprintf(stderr, "打不开 %s: %s\n", apkPath, strerror(errno));
        return 1;
    }

    const int memFd = memfd_create("apk", MFD_CLOEXEC);
    if (memFd < 0) {
        fprintf(stderr, "memfd_create 失败: %s\n", strerror(errno));
        close(fileFd);
        return 1;
    }

    char buf[256 * 1024];
    for (;;) {
        const ssize_t n = read(fileFd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "读取失败: %s\n", strerror(errno));
            close(fileFd); close(memFd);
            return 1;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = write(memFd, buf + off, static_cast<size_t>(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "写入 memfd 失败: %s\n", strerror(errno));
                close(fileFd); close(memFd);
                return 1;
            }
            off += w;
        }
    }
    close(fileFd);
    lseek(memFd, 0, SEEK_SET);

    Reply reply{};
    std::string json;
    const uint32_t flags = replace ? static_cast<uint32_t>(kFlagReplace) : 0u;
    const bool sent = TransactV2(sockFd, Cmd::InstallApp, {}, flags, memFd,
                                 &json, &reply);
    close(memFd);
    if (!sent) return 1;

    if (!json.empty()) printf("%s\n", PrettyJson(json).c_str());
    if (reply.status != kOk) {
        fprintf(stderr, "服务端返回: %s (0x%x)\n",
                StatusName(reply.status), reply.status);
        return 1;
    }
    return 0;
}

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
    // ⚠️ optstring 开头的 "+" 不能省。
    //    GNU getopt 默认会**重排参数**，把子命令后面的选项也当顶层选项解析，
    //    于是 `list-apps --system` 里的 --system 会被当成未知的顶层选项报错。
    //    "+" 让它遇到第一个非选项（也就是子命令）就停下，
    //    后面的参数原样留给子命令自己处理。
    while ((c = getopt_long(argc, argv, "+o:h", kLong, nullptr)) != -1) {
        switch (c) {
            case kOptSocket: socketPath = optarg; break;
            case kOptOut:    outPath = optarg;    break;
            case kOptRaw:    raw = true;          break;
            case kOptMs:     ms = static_cast<uint32_t>(strtoul(optarg, nullptr, 10)); break;

            // ⚠️ 短选项 -o 走的是 'o'，不是 kOptOut。
            //    少了这一行，`-o 路径` 会被 default 静默吞掉，
            //    输出永远落到默认的 /data/local/tmp/shot.png。
            case 'o':        outPath = optarg;    break;

            case 'h':
                Usage(argv[0]);
                return 0;

            default:
                Usage(argv[0]);
                return 1;
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
    // ⚠️ 子命令自己的选项必须在这里解析。
    //    optstring 加了 "+" 之后 getopt 遇到第一个非选项（也就是子命令）就停，
    //    `capture -o /path` 里的 -o 不会再被顶层解析到 ——
    //    结果是输出悄悄落到默认路径，看起来像"选项没生效"。
    for (int i = 0; i < remaining - 1; ++i) {
        if ((strcmp(args[i], "-o") == 0 || strcmp(args[i], "--output") == 0) &&
            i + 1 < remaining - 1) {
            outPath = args[++i];
        } else if (strcmp(args[i], "--raw") == 0) {
            raw = true;
        }
    }

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
    }
    // ── v2：应用与文件管理 ──
    // 约定：payload 为 NUL 分隔字符串，应答为 JSON
    else if (cmd == "list-apps") {
        uint32_t flags = 0;
        bool namesOnly = false;
        for (int i = 0; i < remaining - 1; ++i) {
            if (strcmp(args[i], "--system") == 0) flags |= kFlagIncludeSystem;
            if (strcmp(args[i], "--meta") == 0)   flags |= kFlagWithMetadata;
            if (strcmp(args[i], "--names") == 0)  namesOnly = true;
        }
        rc = CmdV2(sockFd, Cmd::ListApps, {}, flags, /*compact=*/namesOnly);
    } else if (cmd == "app-info") {
        if (remaining < 2) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::AppInfo, BuildPayload({args[0]}), 0);
    } else if (cmd == "launch") {
        if (remaining < 2) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::LaunchApp,
                   BuildPayload({args[0], remaining >= 3 ? args[1] : nullptr}), 0);
    } else if (cmd == "kill") {
        if (remaining < 2) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::KillApp, BuildPayload({args[0]}), 0);
    } else if (cmd == "foreground") {
        rc = CmdV2(sockFd, Cmd::ForegroundApp, {}, 0);
    } else if (cmd == "install") {
        if (remaining < 2) { Usage(argv[0]); close(sockFd); return 1; }
        bool replace = true;
        for (int i = 1; i < remaining - 1; ++i) {
            if (strcmp(args[i], "--no-replace") == 0) replace = false;
        }
        rc = CmdInstall(sockFd, args[0], replace);
    } else if (cmd == "download") {
        if (remaining < 2) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::Download,
                   BuildPayload({args[0],
                                 remaining >= 3 ? args[1] : nullptr,
                                 remaining >= 4 ? args[2] : nullptr}), 0);
    } else if (cmd == "ls" || cmd == "stat") {
        const char* path = FirstPositional(args, remaining - 1);
        rc = CmdV2(sockFd, Cmd::FileOp,
                   BuildPayload({cmd == "ls" ? "list" : "stat",
                                 path != nullptr ? path : ""}), 0);
    } else if (cmd == "mkdir") {
        uint32_t flags = 0;
        for (int i = 0; i < remaining - 1; ++i) {
            if (strcmp(args[i], "-p") == 0 || strcmp(args[i], "--parents") == 0) {
                flags |= kFlagRecursive;
            }
        }
        const char* path = FirstPositional(args, remaining - 1);
        if (path == nullptr) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::FileOp, BuildPayload({"mkdir", path}), flags);
    } else if (cmd == "rm") {
        uint32_t flags = 0;
        for (int i = 0; i < remaining - 1; ++i) {
            if (strcmp(args[i], "-r") == 0 || strcmp(args[i], "-rf") == 0 ||
                strcmp(args[i], "--recursive") == 0) {
                flags |= kFlagRecursive;
            }
        }
        const char* path = FirstPositional(args, remaining - 1);
        if (path == nullptr) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::FileOp, BuildPayload({"delete", path}), flags);
    } else if (cmd == "mv") {
        const char* a = FirstPositional(args, remaining - 1);
        const char* b = nullptr;
        if (a != nullptr) {
            for (char** q = args; *q != nullptr; ++q) {
                if (*q == a) { b = FirstPositional(q + 1, remaining - 1 - (int)(q - args)); break; }
            }
        }
        if (a == nullptr || b == nullptr) { Usage(argv[0]); close(sockFd); return 1; }
        rc = CmdV2(sockFd, Cmd::FileOp, BuildPayload({"rename", a, b}), 0);
    // ── v3：服务自身 ──
    } else if (cmd == "describe") {
        rc = CmdV2(sockFd, Cmd::Describe, {}, 0);
    } else if (cmd == "config") {
        rc = CmdV2(sockFd, Cmd::GetConfig, {}, 0);
    } else if (cmd == "set-config") {
        // 参数成对：key value key value…
        std::string payload;
        int pairs = 0;
        for (int i = 0; i + 1 < remaining - 1; i += 2) {
            if (pairs++ > 0) payload.push_back('\0');
            payload += args[i];
            payload.push_back('\0');
            payload += args[i + 1];
        }
        if (pairs == 0) {
            fprintf(stderr, "用法: set-config <key> <value> [<key> <value> ...]\n");
            fprintf(stderr, "可改: verbose, log-level, display, socket-mode, touch-range\n");
            fprintf(stderr, "需重启: socket, init-socket, uid, gid\n");
            close(sockFd);
            return 1;
        }
        rc = CmdV2(sockFd, Cmd::SetConfig, payload, 0);
    } else if (cmd == "selftest") {
        rc = CmdV2(sockFd, Cmd::SelfTest, {}, 0);
    } else if (cmd == "stats") {
        rc = CmdV2(sockFd, Cmd::Stats, {}, 0);
    } else if (cmd == "log") {
        rc = CmdV2(sockFd, Cmd::Log,
                   BuildPayload({remaining >= 2 ? args[0] : ""}), 0);
    } else if (cmd == "shutdown") {
        rc = CmdV2(sockFd, Cmd::Shutdown, {}, 0);
    } else if (cmd == "restart") {
        rc = CmdV2(sockFd, Cmd::Restart, {}, 0);
    } else {
        fprintf(stderr, "未知子命令: %s\n", cmd.c_str());
        Usage(argv[0]);
        rc = 1;
    }

    close(sockFd);
    return rc;
}
