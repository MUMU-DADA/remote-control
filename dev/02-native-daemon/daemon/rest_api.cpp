// rest_api.cpp — REST 适配器实现

#include "rest_api.h"

#include <errno.h>
#include <fcntl.h>
#include <cmath>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include <iterator>
#include <vector>

#include "remote_control_log.h"
#include "dispatch.h"
#include "json_parser.h"
#include "json_writer.h"
#include "encode_pool.h"
#include "frame_hub.h"
#include "image_encoder.h"
#include "peer_util.h"
#include "png_encoder.h"
#include "protocol.h"
#include "service_state.h"
#include "sha256.h"
#include "websocket.h"
#include "webui.h"

namespace remote_control {
namespace {

// 从 memfd 读回内容。fd 的所有权在本函数内结束（读完就关）。
std::string ReadFd(int fd, uint64_t expected) {
    if (fd < 0) return {};
    std::string out;
    if (expected > 0 && expected < (64u << 20)) out.reserve(expected);
    char buf[64 * 1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 路径分段，用于 /api/v1/apps/{pkg}/launch 这类路由
std::vector<std::string> Segments(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        out.push_back(path.substr(i, j - i));
        i = j;
    }
    return out;
}

// 把参数拼成 NUL 分隔的 payload。
//
// **只省略尾部空参数** —— 协议的约定是"没给的参数就是没给"，
// 而不是"给了一个空字符串"。中间空参数必须保留位置，比如
// Download 的 filename 为空、但 subdir 不为空时，不能把 subdir
// 错当成 filename。
std::string PackArgs(std::initializer_list<std::string> args) {
    // 只省略尾部空参数；中间空参数要保留位置。
    size_t count = args.size();
    while (count > 0) {
        auto it = args.begin();
        std::advance(it, static_cast<ptrdiff_t>(count - 1));
        if (!it->empty()) break;
        --count;
    }

    std::string out;
    size_t i = 0;
    for (const auto& a : args) {
        if (i >= count) break;
        if (i > 0) out.push_back('\0');
        out += a;
        ++i;
    }
    return out;
}

// 读取外部 JSON 手势时不能直接把 double 转成 uint32_t：负数、无穷大和
// 超范围转换的结果不可依赖，还可能让 Injector 长时间阻塞 worker。
bool ParseGestureMs(const json::Value& body, const char* key,
                   uint32_t defaultValue, uint32_t* out,
                   HttpResponse* error) {
    if (!body.has(key)) {
        *out = defaultValue;
        return true;
    }
    const json::Value& v = body[key];
    const double raw = v.asDouble(std::numeric_limits<double>::quiet_NaN());
    if (!v.isNumber() || !std::isfinite(raw) || raw < 0.0 ||
        raw > static_cast<double>(kMaxGestureMs) || std::floor(raw) != raw) {
        *error = HttpResponse::Error(
                400, std::string(key) + " 必须是 0 到 " +
                             std::to_string(kMaxGestureMs) + " 的整数毫秒值");
        return false;
    }
    *out = static_cast<uint32_t>(raw);
    return true;
}

bool IsPathUnder(const std::string& path, const std::string& base) {
    if (path == base) return true;
    return path.size() > base.size() &&
           path.compare(0, base.size(), base) == 0 &&
           path[base.size()] == '/';
}

// Walk an already canonical absolute path without following any symlink.
// The returned parent fd remains valid even if another process renames a
// directory in the meantime; callers can therefore unlinkat() the same entry
// safely after pm has consumed the opened fd.
bool OpenAbsoluteParentNoFollow(const std::string& path, int* parentFd,
                                std::string* leaf, std::string* error) {
    if (path.empty() || path[0] != '/') return false;
    std::vector<std::string> parts;
    size_t i = 1;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        if (slash > i) parts.emplace_back(path, i, slash - i);
        i = slash + 1;
    }
    if (parts.empty()) return false;
    *leaf = parts.back();
    parts.pop_back();
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (error) *error = "无法打开文件父目录: " + std::string(strerror(errno));
        return false;
    }
    for (const std::string& part : parts) {
        const int next = openat(fd, part.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = "无法打开文件父目录: " + std::string(strerror(savedErrno));
            return false;
        }
        close(fd);
        fd = next;
    }
    *parentFd = fd;
    return true;
}

// ?path= 只接受共享存储中的已有普通文件。realpath() 只用于确定允许
// 边界，真正打开和后续删除都通过 canonical path 的目录 fd 完成，避免
// "检查后父目录被换成软链接" 的 TOCTOU。
bool OpenSharedStorageFile(const std::string& path, int* outFd,
                           std::string* canonicalPath, std::string* error) {
    char canonical[PATH_MAX];
    if (realpath(path.c_str(), canonical) == nullptr) {
        if (error) *error = "文件不存在或无法解析: " + path;
        return false;
    }
    const char* roots[] = {
        "/storage/emulated/0",
        "/sdcard",
        "/data/media/0",
        nullptr,
    };
    bool allowed = false;
    for (const char** root = roots; *root != nullptr; ++root) {
        char canonicalRoot[PATH_MAX];
        if (realpath(*root, canonicalRoot) != nullptr &&
            IsPathUnder(canonical, canonicalRoot)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        if (error) *error = "path 必须指向共享存储中的已有文件";
        return false;
    }
    int parentFd = -1;
    std::string leaf;
    if (!OpenAbsoluteParentNoFollow(canonical, &parentFd, &leaf, error)) return false;
    const int fd = openat(parentFd, leaf.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        const int savedErrno = errno;
        close(parentFd);
        if (error) *error = "无法打开 " + path + ": " + strerror(savedErrno);
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        const int savedErrno = errno;
        close(fd);
        close(parentFd);
        if (error) *error = "APK 路径不是普通文件: " + path +
                             (savedErrno ? (" (" + std::string(strerror(savedErrno)) + ")") : "");
        return false;
    }
    close(parentFd);
    *outFd = fd;
    *canonicalPath = canonical;
    return true;
}

bool UnlinkAbsoluteNoFollow(const std::string& path) {
    int parentFd = -1;
    std::string leaf;
    if (!OpenAbsoluteParentNoFollow(path, &parentFd, &leaf, nullptr)) return false;
    const bool ok = unlinkat(parentFd, leaf.c_str(), 0) == 0;
    close(parentFd);
    return ok;
}

bool ParseJsonBody(const HttpRequest& req, json::Value* out, HttpResponse* err) {
    if (req.body.empty()) {
        out->SetObject();     // 空体当空对象，允许 POST 无参数
        return true;
    }
    std::string perr;
    if (!json::Parse(req.body, out, &perr)) {
        *err = HttpResponse::Error(400, "请求体不是合法 JSON: " + perr);
        return false;
    }
    if (!out->isObject()) {
        *err = HttpResponse::Error(400, "请求体必须是 JSON 对象");
        return false;
    }
    return true;
}

}  // namespace

// ── 通用转发 ────────────────────────────────────────────────────────────────
HttpResponse RestApi::Call(Cmd cmd, const std::string& payload, uint32_t flags,
                           int reqFd, bool binaryReply,
                           const char* binaryContentType) {
    const uint32_t cmdId = static_cast<uint32_t>(cmd);
    Request request{};
    request.magic = kMagic;
    request.cmd   = cmdId;
    request.flags = flags;

    ReplyPacket packet = dispatcher_->Handle(request, payload, reqFd, /*peerUid=*/0);
    ServiceState::Instance().CountRequest(cmdId, packet.reply.status);

    std::string body = ReadFd(packet.fd, packet.reply.dataSize);

    if (!binaryReply) {
        // 非二进制命令的应答体一律是 JSON。
        // 服务端已经把错误原因写在 JSON 里了，所以即使 status != OK
        // 也原样返回，让调用方看到的是**服务端的原话**而不是一个码。
        int httpStatus = 200;
        switch (packet.reply.status) {
            case kOk:           httpStatus = 200; break;
            case kErrBadMagic:
            case kErrBadCmd:
            case kErrBadArg:
            case kErrPayload:   httpStatus = 400; break;
            case kErrNotFound:  httpStatus = 404; break;
            case kErrPermission:httpStatus = 403; break;
            case kErrUnsupported: httpStatus = 501; break;
            case kErrTimeout:   httpStatus = 504; break;
            default:            httpStatus = 500; break;
        }
        if (body.empty()) {
            // 理论上不会发生（失败也带 JSON），兜底别返回空体
            json::Writer w;
            w.Obj().Field("ok", packet.reply.status == kOk)
                   .Field("status", static_cast<uint64_t>(packet.reply.status))
                   .Field("error", StatusName(packet.reply.status))
             .EndObj();
            body = w.str();
        }
        return HttpResponse::Json(httpStatus, body);
    }

    if (packet.reply.status != kOk) {
        return HttpResponse::Error(500, StatusName(packet.reply.status));
    }
    HttpResponse r;
    r.status = 200;
    r.contentType = binaryContentType;
    r.body = std::move(body);
    return r;
}

// ── 截图 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleCapture(const HttpRequest& req) {
    // ── 格式与质量 ──
    //
    // **和画面流用同一套解析**，不要各写一遍。早先这里是
    //   `format == "raw" ? raw : 一律 PNG`
    // 于是：
    //   ?format=jpeg   静默返回 PNG
    //   ?format=bogus  也静默返回 PNG
    // 调用方拿到 PNG 却以为要到了 JPEG，而且没有任何提示。
    ImageFormat codec = ImageFormat::kAuto;
    {
        const std::string f = req.queryParam("format", "auto");
        if (!ImageEncoder::ParseFormat(f, &codec)) {
            return HttpResponse::Error(
                    400, "未知格式: " + f + "（可用 auto|png|jpeg|webp|raw）");
        }
    }
    // auto 的含义**随场景变**：
    //   单次截图 → PNG（无损。一次调用，大小不重要，清晰更重要）
    //   画面流   → JPEG（一路视频，带宽和编码耗时都重要）
    // 两个默认值不同是有意的，不是不一致。
    if (codec == ImageFormat::kAuto) codec = ImageFormat::kPng;

    int quality = 0;
    {
        const std::string q = req.queryParam("quality", "");
        if (!q.empty()) {
            char* end = nullptr;
            const long v = strtol(q.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && v >= 1 && v <= 100) {
                quality = static_cast<int>(v);
            }
        }
    }
    if (quality == 0) {
        quality = (codec == ImageFormat::kPng) ? 6
                : (codec == ImageFormat::kWebp) ? 90 : 90;
    }

    Request request{};
    request.magic = kMagic;
    request.cmd   = static_cast<uint32_t>(Cmd::Capture);
    request.flags = 0;

    ReplyPacket packet = dispatcher_->Handle(request, "", -1, 0);
    ServiceState::Instance().CountRequest(static_cast<uint32_t>(Cmd::Capture),
                                          packet.reply.status);

    if (packet.reply.status != kOk) {
        if (packet.fd >= 0) close(packet.fd);
        return HttpResponse::Error(500, StatusName(packet.reply.status));
    }

    const uint32_t w = packet.reply.width;
    const uint32_t h = packet.reply.height;
    const uint32_t fmt = packet.reply.format;
    const uint64_t size = packet.reply.dataSize;

    if (packet.fd < 0 || size == 0) {
        if (packet.fd >= 0) close(packet.fd);
        return HttpResponse::Error(500, "截图返回空数据");
    }

    // mmap 而不是 read：大帧少一次拷贝，而且编码要按行随机访问
    void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, packet.fd, 0);
    if (base == MAP_FAILED) {
        close(packet.fd);
        return HttpResponse::Error(500, std::string("mmap 失败: ") +
                                            strerror(errno));
    }

    // 统一收尾。原来每条分支都写一遍 munmap+close，漏一条就是 fd 泄漏 ——
    // 这个函数现在分支更多了，用 RAII 才靠得住。
    struct Guard {
        void* p; uint64_t n; int fd;
        ~Guard() { munmap(p, n); close(fd); }
    } guard{base, size, packet.fd};

    HttpResponse resp;
    resp.extraHeaders.push_back({"X-RemoteControl-Width", std::to_string(w)});
    resp.extraHeaders.push_back({"X-RemoteControl-Height", std::to_string(h)});
    resp.extraHeaders.push_back({"X-RemoteControl-PixelFormat", std::to_string(fmt)});

    if (codec == ImageFormat::kRaw) {
        resp.status = 200;
        resp.contentType = "application/octet-stream";
        resp.extraHeaders.push_back({"X-RemoteControl-Stride",
                                     std::to_string(packet.reply.stride)});
        resp.body.assign(static_cast<const char*>(base), size);
        return resp;
    }

    // 只处理 4 字节/像素的格式。别的（RGB_565 等）在传输层转没意义，
    // 明确报错让调用方知道要什么。
    const bool is4bpp = (fmt == 1 /*RGBA_8888*/ || fmt == 2 /*RGBX_8888*/ ||
                         fmt == 5 /*BGRA_8888*/);
    if (!is4bpp) {
        return HttpResponse::Error(
                500, "像素格式 0x" + std::to_string(fmt) +
                         " 暂不支持编码（只支持 RGBA/RGBX/BGRA_8888）；"
                         "可用 ?format=raw 取原始像素");
    }

    // BGRA → RGBA
    std::vector<uint8_t> rgba;
    const uint8_t* src = static_cast<const uint8_t*>(base);
    if (fmt == 5) {
        rgba.resize(size);
        for (uint64_t i = 0; i + 3 < size; i += 4) {
            rgba[i + 0] = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i + 0];
            rgba[i + 3] = src[i + 3];
        }
        src = rgba.data();
    }

    std::string perr;
    if (!ImageEncoder::Instance().Init(&perr)) {
        return HttpResponse::Error(500, perr);
    }
    std::string out = ImageEncoder::Instance().Encode(src, w, h, codec,
                                                      quality, &perr);
    if (out.empty()) {
        return HttpResponse::Error(500, perr.empty() ? "编码失败" : perr);
    }
    resp.status = 200;
    resp.contentType = ImageEncoder::MimeType(codec);
    resp.body = std::move(out);
    return resp;
}

// ── 安装 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleInstall(const HttpRequest& req) {
    // 两种来源：
    //   1. 请求体 = APK 字节（网页上传走这条）
    //   2. ?path=/sdcard/xxx.apk（文件已经推到设备上了）
    //
    // 两条都落到**真实文件**上，而不是 memfd。原因就是需求里那条：
    // "安装完成或者失败后删除"。memfd 没有"删除"这个动作可做，
    // 而真实文件如果忘了删，用几次就把 /sdcard 堆满了 APK。
    std::string path = req.queryParam("path", "");
    const bool fromBody = path.empty();
    bool spooled = false;      // 用的是 HttpServer 落盘的临时文件
    std::string cleanupPath;

    if (fromBody) {
        if (req.bodySize == 0 && req.body.empty()) {
            return HttpResponse::Error(
                    400, "请求体为空：把 APK 字节作为请求体发送，"
                         "或用 ?path= 指定设备上已有的文件");
        }

        // ── 大文件：HTTP 层已经落盘了，直接拿来用 ──
        //
        // 不要再往 /sdcard 抄一份：APK 动辄几百 MB，而后面
        // Dispatcher 还会再拷一次给 pm install —— 抄三遍纯属浪费，
        // 在手机上就是几十秒的等待。
        //
        // 这个文件的删除由 HttpServer 的 RAII 负责（请求处理完就删），
        // 所以这里**不要** unlink。
        if (!req.bodyFile.empty()) {
            path = req.bodyFile;
            spooled = true;
            ALOGI("收到上传的 APK（落盘）: %s（%zu 字节）",
                  path.c_str(), req.bodySize);
        } else {
        // ⚠️ 落盘位置改成**服务自己的目录**（与配置、日志同处），不再放 /sdcard。
        //
        //    /sdcard 是 FUSE（storage 层），SELinux Enforcing 下我们的域
        //    过不去（实测 avc denied { search } mnt_user_file），
        //    上传会直接 500。
        //
        //    不需要 installer 能读到这个文件：`pm install <路径>` 是
        //    **调用者进程**（我们，shell 身份）自己读文件再流给
        //    package installer 的，所以只要我们自己读得到就行。
        path = "/data/misc/remote-control/upload-" + std::to_string(getpid()) +
               "-XXXXXX";
        std::vector<char> uploadPath(path.begin(), path.end());
        uploadPath.push_back('\0');
        const int wfd = mkstemp(uploadPath.data());
        if (wfd < 0) {
            return HttpResponse::Error(500, "无法创建 " + path + ": " +
                                                strerror(errno));
        }
        path.assign(uploadPath.data());
        if (fcntl(wfd, F_SETFD, FD_CLOEXEC) < 0) {
            const int savedErrno = errno;
            close(wfd);
            unlink(path.c_str());
            return HttpResponse::Error(500, "设置上传文件标志失败: " +
                                                std::string(strerror(savedErrno)));
        }
        size_t off = 0;
        bool ok = true;
        while (off < req.body.size()) {
            const ssize_t n = write(wfd, req.body.data() + off,
                                    req.body.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            off += static_cast<size_t>(n);
        }
        fsync(wfd);
        close(wfd);
        if (!ok) {
            unlink(path.c_str());
            return HttpResponse::Error(500, std::string("写 APK 失败: ") +
                                                strerror(errno));
        }
        ALOGI("收到上传的 APK: %s（%zu 字节）", path.c_str(), req.bodySize);
        }
    } else {
        int sharedFd = -1;
        std::string sharedError;
        if (!OpenSharedStorageFile(path, &sharedFd, &cleanupPath, &sharedError)) {
            return HttpResponse::Error(404, sharedError.empty() ?
                                                "找不到或无法打开文件" : sharedError);
        }
        // Keep the securely opened fd; the generic open(path) below would
        // reintroduce the check/use race.  It is already O_NOFOLLOW and points
        // at the canonical file validated above.
        const int fd = sharedFd;
        const bool replace = req.queryParam("replace", "1") != "0";
        const uint32_t flags = replace ? static_cast<uint32_t>(kFlagReplace) : 0u;
        HttpResponse resp = Call(Cmd::InstallApp, "", flags, fd);
        close(fd);
        if (req.queryParam("keep", "0") != "1") {
            if (UnlinkAbsoluteNoFollow(cleanupPath)) {
                ALOGI("安装%s，已删除 %s", resp.status == 200 ? "成功" : "失败",
                      cleanupPath.c_str());
            } else {
                ALOGW("安装后删除 %s 失败", cleanupPath.c_str());
            }
        }
        return resp;
    }

    // 交给 InstallApp。它按 fd 顺序读，所以这里把文件打开成 fd。
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        // 打开失败也要删 —— 上传上来的文件已经落地了，
        // 不删就是垃圾。只有 keep=1 才留。
        // （落盘的临时文件不归我们管，见上面 spooled 那段。）
        const bool keep = req.queryParam("keep", "0") == "1";
        if (!keep && !spooled) unlink(path.c_str());
        return HttpResponse::Error(500, "打开 " + path + " 失败: " +
                                            strerror(errno));
    }

    const bool replace = req.queryParam("replace", "1") != "0";
    const uint32_t flags = replace ? static_cast<uint32_t>(kFlagReplace) : 0u;

    HttpResponse resp = Call(Cmd::InstallApp, "", flags, fd);
    close(fd);

    // ── 删除 ──
    //
    // **成功和失败都删**。不删的话每次安装都在 /sdcard 上留一个 APK，
    // 用一阵子就是一堆几十 MB 的文件，而且没人会想起来清。
    //
    // keep=1 可以保留（调试用：想手工确认 APK 内容时）。
    if (spooled) {
        // 落盘文件由 HttpServer 删 —— 这里动它会让 RAII 再删一次（无害），
        // 但如果调用了方还持有 path 去排查就找不到了。交给它。
        return resp;
    }
    if (req.queryParam("keep", "0") != "1") {
        if (unlink(path.c_str()) == 0) {
            ALOGI("安装%s，已删除 %s", resp.status == 200 ? "成功" : "失败",
                  path.c_str());
        } else {
            ALOGW("安装后删除 %s 失败: %s", path.c_str(), strerror(errno));
        }
    }
    return resp;
}

// ── 手势 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleGesture(const HttpRequest& req, Cmd cmd,
                                    bool needsEnd) {
    json::Value b;
    HttpResponse err;
    if (!ParseJsonBody(req, &b, &err)) return err;

    // 坐标字段名同时接受两种写法。
    //
    // 三种手势里 drag 天然要两个点，所以 x1/y1/x2/y2 更自然；
    // 而 longpress/doubletap 只有一个点，x/y 更自然。
    // 与其让调用方记两套，不如两种都收 —— 之前只认 x/y，
    // 结果网页发 x1/y1 的 drag 一直报"需要 x 与 y"。
    auto pick = [&b](const char* a, const char* c) -> int64_t {
        if (b.has(a)) return b.num(a);
        return b.num(c);
    };
    const bool hasPoint = (b.has("x") || b.has("x1")) &&
                          (b.has("y") || b.has("y1"));
    if (!hasPoint) {
        return HttpResponse::Error(400, "需要坐标（x/y 或 x1/y1）");
    }
    if (needsEnd && (!b.has("x2") || !b.has("y2"))) {
        return HttpResponse::Error(400, "拖拽需要终点 x2/y2");
    }

    Request r{};
    r.magic = kMagic;
    r.cmd   = static_cast<uint32_t>(cmd);
    r.x     = static_cast<int32_t>(pick("x", "x1"));
    r.y     = static_cast<int32_t>(pick("y", "y1"));
    r.x2    = static_cast<int32_t>(pick("x2", "x2"));
    r.y2    = static_cast<int32_t>(pick("y2", "y2"));
    if (!ParseGestureMs(b, "ms", 0, &r.durationMs, &err)) return err;
    if (b.flag("long")) r.flags |= kFlagKeyLongPress;

    ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
    ServiceState::Instance().CountRequest(r.cmd, p.reply.status);
    if (p.fd >= 0) close(p.fd);
    if (p.reply.status != kOk) {
        return HttpResponse::Error(500, StatusName(p.reply.status));
    }
    json::Writer w;
    w.Obj().Field("ok", true).Field("x", r.x).Field("y", r.y);
    if (needsEnd) w.Field("x2", r.x2).Field("y2", r.y2);
    w.EndObj();
    return HttpResponse::Json(200, w.str());
}

// ── 按键 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleKey(const HttpRequest& req) {
    json::Value b;
    HttpResponse err;
    if (!ParseJsonBody(req, &b, &err)) return err;

    const std::string k = b.str("key");
    if (k.empty()) return HttpResponse::Error(400, "需要 key（键名或键码）");

    const uint32_t flags = b.flag("long")
                               ? static_cast<uint32_t>(kFlagKeyLongPress) : 0u;
    return Call(Cmd::KeyEvent, PackArgs({k}), flags, -1);
}

// ── 剪贴板 ──────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleClipboard(const HttpRequest& req) {
    if (req.method == "GET") {
        const std::string op = req.queryParam("op", "get");
        return Call(Cmd::Clipboard, PackArgs({op}), 0, -1);
    }
    json::Value b;
    HttpResponse err;
    if (!ParseJsonBody(req, &b, &err)) return err;

    const std::string op = b.str("op", "get");
    if (op == "set") {
        const std::string t = b.str("text");
        if (t.empty()) return HttpResponse::Error(400, "set 需要 text");
        return Call(Cmd::Clipboard, PackArgs({"set", t}), 0, -1);
    }
    return Call(Cmd::Clipboard, PackArgs({op}), 0, -1);
}

// ── 流式触控（WebSocket）────────────────────────────────────────────────────

bool RestApi::HandleTouchEvent(const std::string& text, std::string* reply) {
    reply->clear();

    json::Value v;
    std::string perr;
    if (!json::Parse(text, &v, &perr)) {
        *reply = "{\"ok\":false,\"error\":\"JSON 解析失败\"}";
        return false;
    }

    const std::string t = v.str("t");

    // 心跳。客户端用它测往返延迟 —— 触控手感好不好，
    // 用户感受到的是这个数，不是帧率。
    if (t == "ping") {
        *reply = "{\"t\":\"pong\",\"s\":" +
                 std::to_string(v.num("s", 0)) + "}";
        return true;
    }

    Request r{};
    r.magic     = kMagic;
    r.pointerId = static_cast<uint32_t>(v.num("id", 0));
    r.x         = static_cast<int32_t>(v.num("x", 0));
    r.y         = static_cast<int32_t>(v.num("y", 0));
    r.x2        = static_cast<int32_t>(v.num("x2", 0));
    r.y2        = static_cast<int32_t>(v.num("y2", 0));
    HttpResponse gestureErr;
    if (!ParseGestureMs(v, "ms", 0, &r.durationMs, &gestureErr)) {
        // HttpResponse::Error 已经用 JSON writer 正确转义了错误文本。
        *reply = gestureErr.body;
        return false;
    }
    if (v.num("pressure", 0) > 0) {
        r.pressure = static_cast<float>(v.num("pressure", 0));
    }

    // 事件名 → 命令。
    //
    // down/move/up 是流式的三个原语：客户端按下就发 down，
    // 之后每次指针移动发一个 move，抬起发 up。
    // 服务端收到就立刻注入，不再等"整个手势"。
    uint32_t cmd = 0;
    bool wantReply = true;
    if (t == "down")           { cmd = static_cast<uint32_t>(Cmd::TouchDown); }
    else if (t == "move")      { cmd = static_cast<uint32_t>(Cmd::TouchMove);
                                 // move 是最高频的事件，默认不回 —— 回一个
                                 // 就等于把上行流量翻倍，而它对客户端没用。
                                 // 出错时仍然会回。
                                 wantReply = false; }
    else if (t == "up")        { cmd = static_cast<uint32_t>(Cmd::TouchUp); }
    else if (t == "cancel")    { cmd = static_cast<uint32_t>(Cmd::TouchUp); }
    else if (t == "tap")       { cmd = static_cast<uint32_t>(Cmd::Tap); }
    else if (t == "longpress") { cmd = static_cast<uint32_t>(Cmd::LongPress); }
    else if (t == "doubletap") { cmd = static_cast<uint32_t>(Cmd::DoubleTap); }
    else if (t == "drag")      { cmd = static_cast<uint32_t>(Cmd::Drag); }
    else {
        *reply = "{\"ok\":false,\"error\":\"未知事件: " + t + "\"}";
        return false;
    }
    if (v.num("ms", 0) > 0 && (t == "tap")) wantReply = true;
    r.cmd = cmd;

    ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
    ServiceState::Instance().CountRequest(r.cmd, p.reply.status);
    if (p.fd >= 0) close(p.fd);

    if (p.reply.status != kOk) {
        *reply = "{\"ok\":false,\"t\":\"" + t + "\",\"error\":\"" +
                 StatusName(p.reply.status) + "\"}";
        return false;
    }
    if (wantReply) {
        *reply = "{\"ok\":true,\"t\":\"" + t + "\"}";
    }
    return true;
}

HttpResponse RestApi::HandleTouchStream(const HttpRequest& req) {
    // 必须是一次 WebSocket 升级。普通 GET 落到这里说明调用方用错了方式 ——
    // 明确告诉他该怎么做，比返回一个看不懂的 400 好。
    if (req.header("upgrade") != "websocket") {
        return HttpResponse::Error(
                400, "这个端点需要 WebSocket 升级（用 new WebSocket(...) 连，"
                     "不要用 fetch）。一次性手势仍可用 POST /api/v1/tap 等");
    }

    std::string accept;
    if (!WsComputeAccept(req.header("sec-websocket-key"), &accept)) {
        return HttpResponse::Error(400, "Sec-WebSocket-Key 缺失或不合法");
    }

    HttpResponse resp = HttpResponse::WebSocket(accept);
    resp.streamer = [this](int fd) {
        uint64_t events = 0, failed = 0;
        while (true) {
            WsFrame f;
            std::string err;
            if (!WsReadFrame(fd, &f, &err)) {
                if (!err.empty()) {
                    ALOGW("触控流异常结束: %s", err.c_str());
                }
                break;
            }
            if (f.opcode != kWsText && f.opcode != kWsBinary) continue;

            ++events;
            std::string reply;
            if (!HandleTouchEvent(f.payload, &reply)) {
                ++failed;
            }
            if (!reply.empty()) {
                if (!WsWriteText(fd, reply)) break;
            }
        }
        ALOGI("触控流结束: %llu 个事件，%llu 个失败", 
              static_cast<unsigned long long>(events),
              static_cast<unsigned long long>(failed));
    };
    return resp;
}

namespace {

// 订阅者明细，渲染成一段现成的 JSON 数组。
//
// 单独拿出来是因为主链是 `w.Obj().Field()...` 一条表达式，
// 中间插不进 for 循环。先渲染好再 RawJson 嵌进去最干净。
std::string SubscribersJson() {
    json::Writer arr;
    arr.Arr();
    for (const auto& s : FrameHub::Instance().ListSubscribers()) {
        json::Writer sw;
        sw.Obj()
            .Field("id", static_cast<int64_t>(s.id))
            .Field("fps", static_cast<int64_t>(s.fps))
            .Field("maxWidth", static_cast<int64_t>(s.maxWidth))
            .Field("ageMs", s.ageMs)
            .Field("peer", s.peer)
            .Field("format", s.format)
            .Field("transport", s.transport)
            .Field("isMaxFps", s.isMaxFps)
         .EndObj();
        arr.RawJson(sw.str());
    }
    arr.EndArr();
    return arr.str();
}

}  // namespace

// ── 画面流的可调参数 ────────────────────────────────────────────────────────
//
// 让调用方能**查到**流支持哪些参数、当前默认值是什么，而不是去翻文档。
HttpResponse RestApi::HandleStreamParams(const HttpRequest& req) {
    const StreamParams def;   // 内置默认
    const FrameHub::Stats h = FrameHub::Instance().GetStats();
    // quality 范围也从**同一个来源**取 —— 上报的和服务端实际钳的
    // 必须是同一个数，各写一份迟早漂移。
    const QualityRange qj = QualityRangeFor(ImageFormat::kJpeg);
    const QualityRange qw = QualityRangeFor(ImageFormat::kWebp);
    const QualityRange qp = QualityRangeFor(ImageFormat::kPng);
    const QualityRange qh = QualityRangeFor(ImageFormat::kH264);
    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("endpoint", "/api/v1/stream")
        .Field("transports", "WebSocket（带 Upgrade 头）/ MJPEG（不带）")
        .Field("defaultFps", static_cast<int64_t>(def.fps))
        .Field("defaultMaxWidth", static_cast<int64_t>(def.maxWidth))
        .Field("defaultSkipUnchanged", def.skipUnchanged)
        .Field("defaultFormat", ImageEncoder::Name(ImageEncoder::Instance().BestFormat()))
        .Field("nativeCodecs", ImageEncoder::Instance().hasNativeCodecs())
        // ── 当前抓帧节奏 ──
        //
        // 抓帧线程按**所有订阅者的最高需求**跑。客户端只报自己的 fps，
        // 看不到"服务端实际在按多少抓" —— 那会让"我明明要 10fps 为什么
        // 设备这么烫"变成一个查不出来的问题。
        //
        // 这里如实报出来。activeFps=0 表示没有订阅者、一次都没在抓。
        .Key("capture").Obj()
            .Field("activeFps", static_cast<int64_t>(h.maxFps))
            .Field("subscribers", static_cast<int64_t>(h.subscribers))
            .Field("frames", h.frames)
            .Field("lastCaptureMs", h.lastCaptureMs)
            // 当前按多少宽抓（0 = 原始分辨率）。
            // 抓帧按所有订阅者里**最大的 maxWidth** ——
            // SurfaceFlinger 的 DisplayCaptureArgs.width 是源头降采样。
            .Field("captureWidth", static_cast<int64_t>(h.captureWidth))
            // served/misses：取帧时"最新帧已备好"和"没等到"的次数。
            // misses 高 = 抓帧跟不上需求，客户端在等。
            .Field("served", h.served)
            .Field("misses", h.misses)
            .Field("running", h.running)
            // 停检省下了多少：抓了 frames 帧，其中只有 changeGen 代
            // 内容是新的，其余都跟上一帧一模一样。
            // unchangedRatio 高 = 画面基本静止，停检正在起作用。
            .Field("changeGen", h.changeGen)
            .Field("unchanged", h.unchanged)
            // 每个订阅者的明细。
            //
            // 光有 activeFps 不够用：抓帧节奏由**最高需求**决定，所以只要
            // 有一个客户端挂着 60fps，整个进程就一直在满速抓 —— 而服务端
            // 原来完全看不出那是谁，日志里也不记。实测就踩过：activeFps=60
            // 挂了很久，只能挨个关客户端去试。
            //
            // isMaxFps 直接标出"就是它把节奏顶上来的"，这才是排查时
            // 唯一真正要看的那一条。
            // ⚠️ 必须 RawJson，不能用 Field —— Field 重载会走
            //    Val(const std::string&)，把整个数组当成**字符串**再转义
            //    一遍，客户端拿到的是 "[{\"id\":1,...}]" 而不是数组。
            .Key("subscriberList").RawJson(SubscribersJson())
        .EndObj()
        // ── 每种格式的 quality 范围 ──
        //
        // 各格式的量纲完全不同（PNG 是 zlib 级别 1-9，JPEG/WebP 是
        // 1-100，H.264 那边会换算成码率）。界面上的拖动条要按当前格式
        // 取这个范围，不能写死 —— 写死了就会出现"拖到 75 但 PNG 只认 9"。
        .Key("quality").Obj()
            .Key("jpeg").Obj().Field("min", qj.min).Field("max", qj.max)
                             .Field("default", qj.def).EndObj()
            .Key("webp").Obj().Field("min", qw.min).Field("max", qw.max)
                             .Field("default", qw.def).EndObj()
            .Key("png").Obj().Field("min", qp.min).Field("max", qp.max)
                            .Field("default", qp.def)
                            .Field("note", "zlib 压缩级别，不是图像质量").EndObj()
            .Key("h264").Obj().Field("min", qh.min).Field("max", qh.max)
                             .Field("default", qh.def)
                             .Field("note", "换算成码率，不是图像质量").EndObj()
        .EndObj()
        .Key("codecs").Obj()
            // 这三个是**这台设备实际**能编的，不是"理论上支持的"。
            //
            // Android 8~10 上 webp=false（没有 AndroidBitmap_compress，
            // 设备上也没有 libwebp）。客户端必须先问这个再决定
            // 要不要提供 WebP 选项 —— 硬发 format=webp 只会拿到一个错误。
            .Field("png",  ImageEncoder::Instance().Supports(ImageFormat::kPng))
            .Field("jpeg", ImageEncoder::Instance().Supports(ImageFormat::kJpeg))
            .Field("webp", ImageEncoder::Instance().Supports(ImageFormat::kWebp))
            .Field("raw",  true)
            // H.264 走设备端 MediaCodec，比软件编码器小两个数量级，
            // 但**并发有硬上限**（真机硬件编码器通常 1~2 路）。
            .Field("h264", ImageEncoder::Instance().Supports(ImageFormat::kH264))
            .Field("h264Max",  static_cast<int64_t>(H264Encoder::MaxConcurrent()))
            .Field("h264Used", static_cast<int64_t>(H264Encoder::ActiveCount()))
            .Field("backend", ImageEncoder::Instance().BackendSummary())
            // 开着 REMOTE_CONTROL_FORCE_FALLBACK 时如实标出来 ——
            // 一个强制走回退的实例，它的 codecs 不代表这台设备的真实能力。
            .Field("forced", ImageEncoder::Instance().FallbackForced())
        .EndObj()
     .EndObj();
    // 上面那串只是说明，真正的参数表在下面
    std::string base = w.str();
    base.pop_back();   // 去掉结尾的 }
    base += ",\"params\":[";
    base += "{\"name\":\"fps\",\"range\":\"1-60\",\"desc\":\"帧率\"},";
    base += "{\"name\":\"quality\",\"range\":\"PNG 1-9 / JPEG,WebP 1-100\","
            "\"desc\":\"画质\"},";
    // format 的取值范围按**实际能力**拼，不写死 ——
    // 写死成 "auto|jpeg|webp|png" 的话，在 Android 8~10 上
    // 等于告诉客户端"webp 可用"，而它并不可用。
    {
        std::string fmts = "auto";
        if (ImageEncoder::Instance().Supports(ImageFormat::kJpeg)) fmts += "|jpeg";
        if (ImageEncoder::Instance().Supports(ImageFormat::kWebp)) fmts += "|webp";
        if (ImageEncoder::Instance().Supports(ImageFormat::kPng))  fmts += "|png";
        if (ImageEncoder::Instance().Supports(ImageFormat::kH264)) fmts += "|h264";
        base += "{\"name\":\"format\",\"range\":\"" + fmts +
                "\",\"desc\":\"编码格式\"},";
    }
    base += "{\"name\":\"maxWidth\",\"range\":\"0-8192（0=不缩放）\","
            "\"desc\":\"降采样宽度\"},";
    base += "{\"name\":\"skipUnchanged\",\"range\":\"0|1\","
            "\"desc\":\"画面没变时跳过编码（停检）\"}";
    base += "],\"wsCommands\":[";
    base += "{\"t\":\"fps\",\"v\":\"1-60\"},";
    base += "{\"t\":\"quality\",\"v\":\"1-100\"},";
    base += "{\"t\":\"format\",\"v\":\"jpeg|webp|png\"},";
    base += "{\"t\":\"skipUnchanged\",\"v\":\"0|1\"},";
    base += "{\"t\":\"maxWidth\",\"v\":\"0-8192\","
            "\"desc\":\"中途改降采样宽度，0=不缩放\"},";
    base += "{\"t\":\"refresh\",\"desc\":\"立刻重发一帧\"},";
    base += "{\"t\":\"ping\",\"s\":\"序号\"}";
    base += "]}";
    return HttpResponse::Json(200, base);
}

// ── 日志流（WebSocket）──────────────────────────────────────────────────────
//
// 把环形缓冲里的新行实时推给客户端。
// 用序号做增量：客户端连上时先给一段历史（sinceSeq=0 就是全部），
// 之后只在有新行时推 —— 而不是让客户端轮询 /api/v1/log。
HttpResponse RestApi::HandleLogStream(const HttpRequest& req) {
    std::string accept;
    if (!WsComputeAccept(req.header("sec-websocket-key"), &accept)) {
        return HttpResponse::Error(400, "Sec-WebSocket-Key 缺失或不合法");
    }
    // 起始序号：客户端可以带 since=<seq> 续上一次的进度
    uint64_t since = 0;
    {
        const std::string s = req.queryParam("since", "");
        if (!s.empty()) since = strtoull(s.c_str(), nullptr, 10);
    }

    HttpResponse resp = HttpResponse::WebSocket(accept);
    // 不捕获 this：日志流只碰 LogBuffer 这个单例，没有成员要用。
    // AOSP 是 -Werror，多捕获一个就是编译失败（-Wunused-lambda-capture）。
    resp.streamer = [since](int fd) {
        uint64_t cursor = since;
        int64_t  lastSend = NowMs();
        // 一次最多推这么多行：日志刷起来可能一秒几十行，
        // 无节制地写会把这条连接变成瓶颈。
        constexpr size_t kBatch = 200;

        while (true) {
            // 处理客户端消息（主要是 ping）
            for (;;) {
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) <= 0) break;
                WsFrame f;
                std::string err;
                if (!WsReadFrame(fd, &f, &err)) {
                    if (!err.empty()) ALOGW("日志流异常结束: %s", err.c_str());
                    return;
                }
                if (f.opcode != kWsText && f.opcode != kWsBinary) continue;
                json::Value v;
                std::string jerr;
                if (!json::Parse(f.payload, &v, &jerr)) continue;
                if (v.str("t") == "ping") {
                    WsWriteText(fd, "{\"t\":\"pong\",\"s\":" +
                                            std::to_string(v.num("s", 0)) + "}");
                } else if (v.str("t") == "clear") {
                    cursor = 0;      // 客户端要重新拉全部
                }
            }

            uint64_t latest = 0;
            std::vector<LogLine> lines =
                    LogBuffer::Instance().Since(cursor, kBatch, &latest);

            if (!lines.empty()) {
                json::Writer w;
                w.Obj().Field("t", "lines").Field("dropped",
                        static_cast<int64_t>(LogBuffer::Instance().DroppedCount()));
                w.Key("lines").Arr();
                for (const LogLine& l : lines) {
                    w.Obj()
                        .Field("seq", static_cast<int64_t>(l.seq))
                        .Field("ms", l.timeMs)
                        .Field("time", l.timeStr)
                        .Field("level", static_cast<int64_t>(l.level))
                        .Field("tag", l.tag)
                        .Field("text", l.text)
                     .EndObj();
                    cursor = l.seq;
                }
                w.EndArr().EndObj();
                if (!WsWriteText(fd, w.str())) return;
                lastSend = NowMs();
            }

            // 没日志时也别空转。100ms 的粒度对"实时看日志"足够了，
            // 而 CPU 占用可以忽略。
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            poll(&pfd, 1, 100);
            (void)lastSend;
        }
    };
    return resp;
}

// ── 实时画面流 ──────────────────────────────────────────────────────────────
// ── 画面流：参数、取帧、两条传输 ────────────────────────────────────────────
//
// MJPEG 和 WebSocket 用的是**同一套取帧与编码逻辑**，区别只在怎么发出去：
//   MJPEG       multipart/x-mixed-replace，用 <img src> 就能看，零 JS
//   WebSocket   二进制帧，用 canvas 画，延迟更低、可控性更好
//
// 抽出公共部分不只是省代码 —— 两条路各写一遍的话，"跳过未变化的帧"
// 这类优化很容易只做在一条上。

namespace {

int ClampInt(const std::string& s, int lo, int hi, int def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return def;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return static_cast<int>(v);
}

// 从 query 里取参数。失败时把原因写进 error。
bool ParseStreamParams(const HttpRequest& req, StreamParams* out,
                       std::string* error) {
    out->fps = ClampInt(req.queryParam("fps", "5"), 1, 60, 5);
    out->maxWidth = ClampInt(req.queryParam("maxWidth", "720"), 0, 8192, 720);
    out->skipUnchanged = req.queryParam("skipUnchanged", "1") != "0";

    const std::string fs = req.queryParam("format", "auto");
    ImageFormat fmt;
    if (!ImageEncoder::ParseFormat(fs, &fmt)) {
        *error = "未知格式: " + fs + "（可用 auto|png|jpeg|webp）";
        return false;
    }
    if (fmt == ImageFormat::kAuto) fmt = ImageEncoder::Instance().BestFormat();
    if (fmt == ImageFormat::kRaw) {
        *error = "流不支持 format=raw";
        return false;
    }
    out->codec = static_cast<int>(fmt);

    // quality 的含义随格式变：PNG 1-9（zlib 级别），JPEG/WebP 1-100（质量）。
    // 之前只收 1..9，导致 format=jpeg&quality=75 被静默丢弃、退回 1。
    out->level = 0;
    const std::string qs = req.queryParam("quality", "");
    if (!qs.empty()) {
        out->level = ClampInt(qs, 1, 100, 0);
    }
    // 按格式的真实范围钳位。
    //
    // 早先写成 `lossy = JPEG||WebP`，其余一律当 PNG 钳到 1-9 ——
    // h264 被误伤（它也是 1-100），拖动条拖到 75 实际按 9 走。
    const QualityRange qr = QualityRangeFor(fmt);
    if (out->level < qr.min || out->level > qr.max) out->level = qr.def;
    return true;
}

// 盒式降采样。把 w×h 的 RGBA 缩到 dw×dh。
//
// 用盒式平均而不是最近邻：最近邻会把细线（文字、边框）整条丢掉，
// 看起来像画面在闪。
// 客户端还在吗？
//
// ⚠️ 这个函数是**必须**的，不是优化。
//
//    流式循环靠 `write` 失败来发现客户端断开 —— 但"画面没变"时
//    根本不 write（`continue` 跳过），于是一旦画面静止：
//      发现不了断开 → 循环空转 → 一直请求抓帧 → 订阅永不释放
//
//    实测症状：客户端断开 4 秒后，抓帧数还在涨（27 → 69），
//    FrameHub 的订阅数停在 2 不动。
//
//    这是**既有 bug**（改共享抓帧之前就有），只是以前每次循环都要
//    抓一帧（200ms），空转得慢，看不出来。
bool PeerGone(int fd) {
    // 明确的挂断/错误
    pollfd p{};
    p.fd = fd;
    p.events = POLLOUT;
    if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        return true;
    }
    // 对端关了连接 —— recv 立刻返回 0（EOF）
    char c = 0;
    const ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return true;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return true;
    }
    return false;
}

void DownscaleRgba(const uint8_t* src, uint32_t sw, uint32_t sh,
                   uint32_t dw, uint32_t dh, std::vector<uint8_t>* out) {
    out->resize(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * sh / dh);
        uint32_t y1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * sw / dw);
            uint32_t x1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + static_cast<size_t>(sy) * sw * 4;
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                    ++n;
                }
            }
            uint8_t* d = out->data() + (static_cast<size_t>(y) * dw + x) * 4;
            d[0] = static_cast<uint8_t>(r / n);
            d[1] = static_cast<uint8_t>(g / n);
            d[2] = static_cast<uint8_t>(b / n);
            d[3] = static_cast<uint8_t>(a / n);
        }
    }
}

}  // namespace

// 取一帧、降采样、编码。
//
// 返回空字符串表示"这一帧不用发"，用 *unchanged 区分是"画面没变"还是"出错"。
std::string RestApi::NextEncodedFrame(const StreamParams& p, StreamState* st,
                                      bool* unchanged) {
    *unchanged = false;

    // ── 订阅共享抓帧 ──
    //
    // 第一次调用时订阅，之后这个连接一直持有它。
    // 第一个订阅者启动抓帧线程，最后一个离开时停掉 ——
    // **没人在看画面的时候完全不抓帧**。
    if (!st->hubSub) {
        std::string err;
        // 上报目标帧率 —— 抓帧线程按所有订阅者的**最高**需求跑。
        // 消费者来了直接拿最新帧，不用等一次抓帧（那是"变卡"的根源）。
        st->hubSub = FrameHub::Instance().Subscribe(p.fps, p.maxWidth, &err);
        if (!st->hubSub) {
            ALOGW("订阅共享抓帧失败: %s", err.c_str());
            return {};
        }
    }

    // ── 等一帧比我已消费的更新的 ──
    //
    // 多个客户端会等到**同一帧** —— 这正是共享的意义：
    // 三个客户端看同一块屏幕，只需要抓一次。
    //
    // 超时给两倍帧间隔（下限 100ms、上限 500ms）：既够等到下一次
    // 抓帧完成（screencap 后端要 120ms+），又不会把控制消息
    // （改帧率/画质）拖太久 —— 那些是在同一个循环里处理的。
    const int waitMs = std::min(500, std::max(100, p.fps > 0 ? 2000 / p.fps : 200));

    uint64_t latestSeq = 0;
    FramePtr f = st->hubSub->WaitNext(st->hubSeq, waitMs, &latestSeq);
    if (!f) {
        // 没等到新帧。两种可能：
        //   - 抓帧比帧间隔还慢（超时是正常的，不是错误）
        //   - 抓帧失败
        // 分开计数，不然"画面卡住"的时候看不出是哪一种。
        if (latestSeq == st->hubSeq) ++st->hubTimeouts;
        return {};
    }
    st->hubSeq = f->seq;

    // ⚠️ 下游的 DownscaleRgba 逐行按 width 跨步。stride != width 时
    //    会画出斜的图 —— 与其静默出错，不如明确拒绝一次并说清楚。
    //    （实测见过的后端都是 stride == width，所以这是道保险。）
    if (f->stride != 0 && f->stride != f->width) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ALOGE("抓帧 stride(%u) != width(%u)，画面流暂不支持 —— 请报告",
                  f->stride, f->width);
        }
        return {};
    }

    // ── 停检：内容跟上次发的一模一样就直接走 ──
    //
    // ⚠️ 必须放在**所有像素操作之前**。抓帧层已经比过一次了
    //    （见 SharedFrame::changeGen），这里只比一个整数。
    //
    //    以前是放在降采样之后、对降采样缓冲区做 FNV 逐字节哈希：
    //    720p 下 3.66 ms/帧 一个客户端，60fps 就是 22% 一个核 ——
    //    一个"用来省性能"的开关反而成了最贵的一环。更糟的是它排在
    //    BGRA 转换和降采样**后面**，所以那些活儿在白干的帧上照样全做。
    if (p.skipUnchanged && st->haveChangeGen &&
        f->changeGen == st->lastChangeGen) {
        *unchanged = true;
        return {};
    }
    st->lastChangeGen = f->changeGen;
    st->haveChangeGen = true;

    const uint32_t w = f->width, h = f->height;
    const size_t size = f->size;
    const uint8_t* src = f->data;

    // BGRA → RGBA。每个客户端各转一次（不同客户端可能用不同的
    // 降采样），换算成本远低于一次抓帧。
    std::vector<uint8_t> rgba;
    if (f->needsBgraSwap()) {
        rgba.resize(size);
        for (size_t i = 0; i + 3 < size; i += 4) {
            rgba[i]     = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i];
            rgba[i + 3] = src[i + 3];
        }
        src = rgba.data();
    }

    uint32_t dw = w, dh = h;
    const uint8_t* enc = src;
    if (p.maxWidth > 0 && w > static_cast<uint32_t>(p.maxWidth)) {
        dw = static_cast<uint32_t>(p.maxWidth);
        dh = static_cast<uint32_t>(static_cast<uint64_t>(h) * dw / w);
        if (dh == 0) dh = 1;
        DownscaleRgba(src, w, h, dw, dh, &st->scaled);
        enc = st->scaled.data();
    }
    st->outW = dw;
    st->outH = dh;

    // ── H.264：不走 ImageEncoder ──
    //
    // 它是**有状态**的（SPS/PPS 只发一次、帧间参考），而
    // ImageEncoder 每次调用都是独立的。所以这里自己管一个实例。
    if (static_cast<ImageFormat>(p.codec) == ImageFormat::kH264) {
        std::string herr;

        if (!st->h264) {
            // 并发上限是**硬限制**：真机硬件编码器通常只支持 1~2 路。
            // 超了不是变慢，是创建失败 —— 所以先问名额，好在日志里
            // 说清楚"为什么这个流没画面"。
            if (!H264Encoder::SlotAvailable()) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    ALOGW("H.264 编码器已达上限（%d 路在用），"
                          "这个流改用 JPEG 或等一路释放",
                          H264Encoder::MaxConcurrent());
                }
                return {};
            }
            st->h264 = std::make_unique<H264Encoder>();
        }

        // 尺寸变了必须重建：编码器一旦 configure 就不能改尺寸
        if (st->h264W != dw || st->h264H != dh) {
            H264Encoder::Config cfg;
            cfg.width  = dw;
            cfg.height = dh;
            cfg.fps    = static_cast<uint32_t>(p.fps > 0 ? p.fps : 30);
            // 码率由 quality 推导。经验公式：每像素每帧约 0.1 bit
            // 是"看得过去"的量级，再按 quality/75 缩放。
            const int q = (p.level > 0 && p.level <= 100) ? p.level : 75;
            uint64_t br = static_cast<uint64_t>(dw) * dh * cfg.fps / 10 * q / 75;

            // 下限**跟着分辨率走**，不是固定值。
            //
            // 固定 200kbps 在小分辨率下够用，但 720x1480 这种尺寸下
            // 意味着每像素每帧只有 0.0075 bit —— 编码器基本不输出，
            // 客户端一帧都收不到（实测踩过）。
            // w*h*fps/50 相当于每像素每帧 0.02 bit，是"能看出画面"的底线。
            const uint64_t floorBr =
                    static_cast<uint64_t>(dw) * dh * cfg.fps / 50;
            if (br < floorBr) br = floorBr;
            if (br > 40000000) br = 40000000;      // 40Mbps，再高没意义
            cfg.bitrate = static_cast<uint32_t>(br);

            if (!st->h264->Start(cfg, &herr)) {
                ALOGW("H.264 启动失败，这个流没有画面: %s", herr.c_str());
                st->h264.reset();
                return {};
            }
            st->h264W = dw;
            st->h264H = dh;
            // 新实例：必须让下一个输出是关键帧，否则客户端开头是黑的
            // （要等到下一个 I 帧，默认 2 秒）
            st->needKeyFrame = true;
            ALOGI("流 H.264 编码器: %ux%u @%u bps",
                  dw, dh, cfg.bitrate);
        }

        if (st->needKeyFrame) {
            st->h264->RequestKeyFrame();
            st->needKeyFrame = false;
        }

        // 编码并发限制。拿不到名额就跳过这一帧 ——
        // 堵在这里的话连客户端的 fps/画质控制消息都收不到
        // （它们在同一个循环里处理）。
        EncodePool::Guard guard(200);
        if (!guard.acquired()) return {};

        std::vector<uint8_t> nal;
        if (!st->h264->EncodeRgba(enc, dw, dh, &nal, &herr)) {
            ALOGW("H.264 编码失败: %s", herr.c_str());
            st->h264.reset();
            st->h264W = st->h264H = 0;
            return {};
        }
        // 编码器有内部缓冲，不是每送一帧就出一帧 —— 空是正常的
        if (nal.empty()) {
            *unchanged = true;
            return {};
        }
        ++st->frameNo;
        return std::string(reinterpret_cast<const char*>(nal.data()), nal.size());
    }

    // 编码并发限制。多个客户端共享同一帧，所以它们会**同时**开始编码 ——
    // 10 个客户端在 4 核上就是 10 路并发抢 CPU，每个都变慢，
    // 还挤占抓帧和触控注入。限制到核数之后，多的排队。
    //
    // 排队会让超额客户端的帧率下降，这是有意的取舍：与其 10 个都卡，
    // 不如 4 个流畅 + 6 个慢一点。
    EncodePool::Guard guard(200);
    if (!guard.acquired()) {
        *unchanged = true;   // 没编成 = 这一轮没新帧，调用方按"没变"处理
        return {};
    }

    std::string perr;
    std::string out = ImageEncoder::Instance().Encode(enc, dw, dh,
                                                      static_cast<ImageFormat>(p.codec),
                                                      p.level, &perr);
    if (out.empty()) ALOGW("流编码失败: %s", perr.c_str());

    if (!out.empty()) ++st->frameNo;
    return out;
}

HttpResponse RestApi::HandleStream(const HttpRequest& req) {
    if (!ImageEncoder::Instance().Init(nullptr)) {
        return HttpResponse::Error(500, "没有可用的图像编码器");
    }

    StreamParams p;
    std::string perr;
    if (!ParseStreamParams(req, &p, &perr)) {
        return HttpResponse::Error(400, perr);
    }

    // 有 Upgrade 头就是 WebSocket，否则给 MJPEG。
    //
    // 两种都留着是有意的：MJPEG 能直接塞进 <img src>，零 JS，
    // 拿来调试或嵌到别的页面里最省事；WebSocket 延迟更低、可控性更好，
    // 是控制台自己用的那条。
    if (req.header("upgrade") == "websocket") {
        return HandleStreamWs(req, p);
    }

    const int intervalMs = 1000 / p.fps;
    const std::string boundary = p.boundary;

    HttpResponse resp = HttpResponse::Stream(
            "multipart/x-mixed-replace; boundary=" + boundary);

    // H.264 不能走 MJPEG。
    //
    // multipart/x-mixed-replace 是"每段一张独立的图"，而 H.264 的
    // P 帧依赖前面的帧 —— 拆成独立 part 就解不出来了。
    // 明确拒绝，别让客户端拿到一堆解不开的字节。
    if (static_cast<ImageFormat>(p.codec) == ImageFormat::kH264) {
        return HttpResponse::Error(
                400, "H.264 只能走 WebSocket（需要 Upgrade 头）—— "
                     "MJPEG 的 multipart 装不下带帧间依赖的流。"
                     "用 ws://…/api/v1/stream?format=h264");
    }

    resp.streamer = [this, boundary, intervalMs, p](int fd) {
        StreamState st;
        while (true) {
            const int64_t t0 = NowMs();

            bool unchanged = false;
            std::string img = NextEncodedFrame(p, &st, &unchanged);
            // 第一次拿到订阅就自报家门 —— 抓帧节奏由最高需求决定，
            // /params 里必须能看出"是谁在拉、拉多快"，否则只能靠猜。
            if (st.hubSub && !st.described) {
                st.described = true;
                st.hubSub->Describe(
                        PeerName(fd),
                        ImageEncoder::Name(static_cast<ImageFormat>(p.codec)),
                        "mjpeg");
            }
            if (img.empty()) {
                // 画面没变就整帧跳过：MJPEG 客户端会继续显示上一帧，
                // 这正是我们要的。出错也走这里，下一轮重试。
                //
                // ⚠️ 但**必须**在这里检查客户端还在不在 —— 这条路径
                //    不 write，靠 write 失败是发现不了断开的。
                if (PeerGone(fd)) break;
                const int64_t rest = intervalMs - (NowMs() - t0);
                if (rest > 0) usleep(static_cast<useconds_t>(rest) * 1000);
                continue;
            }

            std::string part;
            part.reserve(img.size() + 200);
            part += "--" + boundary + "\r\n";
            part += "Content-Type: ";
            part += ImageEncoder::MimeType(static_cast<ImageFormat>(p.codec));
            part += "\r\n";
            part += "Content-Length: " + std::to_string(img.size()) + "\r\n";
            part += "X-RemoteControl-Frame: " + std::to_string(st.frameNo) + "\r\n";
            part += "X-RemoteControl-Width: " + std::to_string(st.outW) + "\r\n";
            part += "X-RemoteControl-Height: " + std::to_string(st.outH) + "\r\n";
            part += "\r\n";
            part += img;
            part += "\r\n";

            size_t sent = 0;
            bool broken = false;
            while (sent < part.size()) {
                const ssize_t n = write(fd, part.data() + sent, part.size() - sent);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    broken = true;
                    break;
                }
                sent += static_cast<size_t>(n);
            }
            if (broken) break;

            const int64_t rest = intervalMs - (NowMs() - t0);
            if (rest > 0) usleep(static_cast<useconds_t>(rest) * 1000);
        }
    };
    return resp;
}

// ── 画面流（WebSocket）──────────────────────────────────────────────────────
//
// 相对 MJPEG 的好处：
//   - 二进制帧，不用 base64 也不用 multipart 边界解析
//   - 客户端能**反过来控制**流（改帧率/画质不用重连）
//   - canvas 绘制，没有 <img> 的布局与解码队列
//   - 能测往返延迟（和触控流同一个套路）
HttpResponse RestApi::HandleStreamWs(const HttpRequest& req,
                                     const StreamParams& params) {
    std::string accept;
    if (!WsComputeAccept(req.header("sec-websocket-key"), &accept)) {
        return HttpResponse::Error(400, "Sec-WebSocket-Key 缺失或不合法");
    }

    HttpResponse resp = HttpResponse::WebSocket(accept);
    resp.streamer = [this, params](int fd) {
        StreamParams p = params;
        StreamState st;

        // 先告诉客户端画面尺寸和格式 —— 客户端要据此建 canvas，
        // 而第一帧到达之前它没法知道。
        {
            json::Writer w;
            w.Obj()
                .Field("t", "hello")
                .Field("format", ImageEncoder::Name(static_cast<ImageFormat>(p.codec)))
                .Field("fps", static_cast<int64_t>(p.fps))
                .Field("quality", static_cast<int64_t>(p.level))
                .Field("maxWidth", static_cast<int64_t>(p.maxWidth))
                .Field("skipUnchanged", p.skipUnchanged)
                .Field("chrome", false)
             .EndObj();
            if (!WsWriteText(fd, w.str())) return;
        }

        int64_t  nextFrameAt = NowMs();
        uint64_t sent = 0;
        uint32_t lastW = 0, lastH = 0;   // 上次告诉客户端的尺寸
        bool     codecSent = false;   // H.264 的 codec 串只发一次

        // 主循环用 poll 同时等两件事：客户端发来的控制消息、下一帧的时间点。
        //
        // 早先的写法是"select 轮询 + sleep 1ms"，那样有两个毛病：
        //   1. 空转（每秒 1000 次唤醒）
        //   2. 控制消息要等到下一次循环才被看到 —— 而中间可能正卡在
        //      一次抓帧里（screencap 后端要 120ms），于是 ping 的往返
        //      延迟能到 100ms 以上
        // 用 poll 带超时之后，消息一到就被处理，帧也按时间点出。
        while (true) {
            const int64_t now = NowMs();
            int waitMs = 0;
            if (nextFrameAt > now) {
                waitMs = static_cast<int>(nextFrameAt - now);
                if (waitMs > 1000) waitMs = 1000;   // 最多等 1 秒，便于察觉断连
            }

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int pr = poll(&pfd, 1, waitMs);

            if (pr < 0) {
                if (errno == EINTR) continue;
                return;
            }

            // ── 有消息就处理 ──
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                WsFrame f;
                std::string err;
                if (!WsReadFrame(fd, &f, &err)) {
                    if (!err.empty()) ALOGW("画面流异常结束: %s", err.c_str());
                    return;   // 连接断了
                }
                if (f.opcode == kWsText || f.opcode == kWsBinary) {
                    json::Value v;
                    std::string jerr;
                    if (json::Parse(f.payload, &v, &jerr)) {
                        const std::string t = v.str("t");
                        if (t == "ping") {
                            WsWriteText(fd, "{\"t\":\"pong\",\"s\":" +
                                                std::to_string(v.num("s", 0)) + "}");
                        } else if (t == "fps") {
                            const int nf = ClampInt(std::to_string(v.num("v", p.fps)),
                                                    1, 60, p.fps);
                            p.fps = nf;
                            nextFrameAt = NowMs();    // 立刻反映新帧率
                            // 也告诉 FrameHub —— 抓帧节奏跟着最高需求走，
                            // 不报的话"客户端降到 5fps 但服务端还在 30fps 抓"
                            if (st.hubSub) st.hubSub->SetFps(nf);
                            WsWriteText(fd, "{\"t\":\"ack\",\"fps\":" +
                                                std::to_string(nf) + "}");
                        } else if (t == "quality") {
                            const QualityRange qr = QualityRangeFor(
                                    static_cast<ImageFormat>(p.codec));
                            p.level = ClampInt(std::to_string(v.num("v", p.level)),
                                               qr.min, qr.max, p.level);
                            st.haveChangeGen = false;
                            WsWriteText(fd, "{\"t\":\"ack\",\"quality\":" +
                                                std::to_string(p.level) + "}");
                        } else if (t == "format") {
                            const std::string fs = v.str("v");
                            ImageFormat nf;
                            if (ImageEncoder::ParseFormat(fs, &nf) &&
                                nf != ImageFormat::kRaw) {
                                if (nf == ImageFormat::kAuto) {
                                    nf = ImageEncoder::Instance().BestFormat();
                                }
                                p.codec = static_cast<int>(nf);
                                p.level = (nf == ImageFormat::kJpeg) ? 75
                                        : (nf == ImageFormat::kWebp) ? 80 : 1;
                                st.haveChangeGen = false;   // 换了格式，缓存作废
                                WsWriteText(fd,
                                    std::string("{\"t\":\"ack\",\"format\":\"") +
                                        ImageEncoder::Name(nf) + "\"}");
                            }
                        } else if (t == "skipUnchanged") {
                            p.skipUnchanged = (v.num("v", 1) != 0);
                            st.haveChangeGen = false;   // 重新开始判定
                            WsWriteText(fd, std::string("{\"t\":\"ack\","
                                    "\"skipUnchanged\":") +
                                    (p.skipUnchanged ? "true" : "false") + "}");
                        } else if (t == "maxWidth") {
                            // 中途换降采样宽度 —— 不用重连就能改分辨率。
                            // 0 = 不降采样（原始分辨率）。
                            const int nw = ClampInt(
                                    std::to_string(v.num("v", p.maxWidth)),
                                    0, 8192, p.maxWidth);
                            p.maxWidth = nw;
                            // 也要告诉 FrameHub：抓帧宽度取所有订阅者里
                            // **最大的**那个，改小了别人不受影响。
                            if (st.hubSub) st.hubSub->SetMaxWidth(nw);
                            // 画面尺寸变了，但**像素内容**没变 ——
                            // changeGen 不会动，不强制的话静止画面下
                            // 客户端会一直停在旧尺寸上。
                            st.haveChangeGen = false;
                            nextFrameAt = NowMs();
                            WsWriteText(fd, "{\"t\":\"ack\",\"maxWidth\":" +
                                                std::to_string(nw) + "}");
                        } else if (t == "refresh") {
                            // 客户端主动要求"下一帧无论变没变都发"，
                            // 用于页面重新可见时立刻刷新一次。
                            st.haveChangeGen = false;
                            nextFrameAt = NowMs();
                        }
                    }
                }
                continue;    // 处理完消息再看时间，避免刚好错过帧点
            }

            // ── 到点就出一帧 ──
            if (NowMs() < nextFrameAt) continue;
            const int intervalMs = 1000 / p.fps;
            nextFrameAt = NowMs() + intervalMs;

            bool unchanged = false;
            std::string img = NextEncodedFrame(p, &st, &unchanged);
            // 第一次拿到订阅就自报家门 —— 抓帧节奏由最高需求决定，
            // /params 里必须能看出"是谁在拉、拉多快"，否则只能靠猜。
            if (st.hubSub && !st.described) {
                st.described = true;
                st.hubSub->Describe(
                        PeerName(fd),
                        ImageEncoder::Name(static_cast<ImageFormat>(p.codec)),
                        "ws");
            }
            if (img.empty()) continue;      // 没变或出错，下一轮再看

            // 二进制帧。尺寸信息单独发一条文本消息 ——
            // 不然客户端在"有图没尺寸"的窗口里没法画。
            //
            // ⚠️ 不能只在第一帧发。尺寸是会**中途变**的：
            //    · 客户端改 maxWidth
            //    · 设备转屏 / 改分辨率
            //    只发一次的话客户端画布会一直停在旧尺寸上。
            if (st.outW != lastW || st.outH != lastH) {
                json::Writer w;
                w.Obj()
                    .Field("t", "size")
                    .Field("w", static_cast<int64_t>(st.outW))
                    .Field("h", static_cast<int64_t>(st.outH))
                 .EndObj();
                if (!WsWriteText(fd, w.str())) return;
                lastW = st.outW;
                lastH = st.outH;
            }

            // H.264 还要告诉客户端 **codec 串**（"avc1.42C029"）。
            //
            // WebCodecs 的 VideoDecoder 必须要它。各设备的 profile/level
            // 不同，客户端不能猜、服务端也不该写死 —— 它来自 SPS，
            // 所以要等**第一帧编出来之后**才拿得到，只能放在这里发。
            if (static_cast<ImageFormat>(p.codec) == ImageFormat::kH264 &&
                !codecSent && st.h264) {
                const std::string cs = st.h264->CodecString();
                if (!cs.empty()) {
                    json::Writer w;
                    w.Obj().Field("t", "codec").Field("codec", cs).EndObj();
                    if (!WsWriteText(fd, w.str())) return;
                    codecSent = true;
                }
            }

            if (!WsWriteFrame(fd, kWsBinary, img)) return;
            ++sent;
        }
    };
    return resp;
}

// ── 路由 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::Handle(const HttpRequest& req) {
    // ── 服务对外开关 ──
    //
    // 关掉之后一切请求都回 503，**只有服务开关本身除外**。
    // 不放行它的话，关掉服务就等于把自己锁在门外 —— 而这正是
    // 软开关想避免的情况。上位应用还能通过改配置文件开回来，
    // 但网页和 API 客户端没有文件访问权。
    if (!ServiceState::Instance().Serving()) {
        const std::string p = req.path;
        // 放行两类：
        //   1. 网页本身 —— 不放行的话用户连开关都够不着（浏览器里
        //      只会看到一个 503 的 JSON），只能靠上位应用或改配置文件
        //   2. 开关接口 —— API 客户端重新开启的入口
        const bool isPage = (p == "/" || p == "/index.html" || p == "/ui");
        const bool isSwitch = (p == "/api/v1/service");
        if (!isPage && !isSwitch) {
            return HttpResponse::Error(
                    503, "服务已关闭对外能力。用 POST /api/v1/service "
                         "{\"on\":true} 重新开启");
        }
    }

    // 归一化尾斜杠：/api/v1/ 与 /api/v1 应当等价。
    // 不做这一步的话，浏览器里多打一个斜杠就 404 —— 而 Segments() 是
    // 容忍尾斜杠的，两边判断不一致。（实测就是这么暴露的。）
    std::string path = req.path;
    while (path.size() > 1 && path.back() == '/') path.pop_back();

    const std::vector<std::string> seg = Segments(path);

    // 期望形如 api / v1 / <resource> [/{id} [/action]]
    if (seg.size() < 3 || seg[0] != "api" || seg[1] != "v1") {
        // 根路径给网页控制台 —— 用浏览器打开服务地址就能用，
        // 不必先去读文档找接口路径。
        if (path == "/" || path == "/index.html" || path == "/ui") {
            HttpResponse ui;
            ui.status = 200;
            ui.contentType = "text/html; charset=utf-8";
            ui.body = WebUiHtml();
            return ui;
        }
        if (path == "/api" || path == "/api/v1") {
            // 给个索引，浏览器打开根路径时不至于 404 得莫名其妙
            json::Writer w;
            w.Obj().Field("service", "remote-control")
                   .Field("buildId", SelfBuildId())
                   .Field("protocolVersion", ServiceState::ProtocolVersion())
                   .Field("hint", "所有接口在 /api/v1/ 下；GET /api/v1/describe 看完整清单")
             .EndObj();
            return HttpResponse::Json(200, w.str());
        }
        return HttpResponse::Error(404, "路径应为 /api/v1/...");
    }

    const std::string& res = seg[2];
    const std::string  method = req.method;

    // ── 服务自身 ──
    if (res == "describe" && method == "GET") {
        return Call(Cmd::Describe, "", 0, -1);
    }
    if (res == "config") {
        if (method == "GET") return Call(Cmd::GetConfig, "", 0, -1);
        if (method == "POST") {
            json::Value body;
            HttpResponse err;
            if (!ParseJsonBody(req, &body, &err)) return err;
            // {key: value, ...} → NUL 分隔的键值对
            std::string payload;
            bool first = true;
            for (const auto& k : body.keys()) {
                const json::Value& v = body[k];
                std::string val;
                if (v.isString())      val = v.asString();
                else if (v.isBool())   val = v.asBool() ? "true" : "false";
                else if (v.isNumber()) val = std::to_string(v.asInt());
                else {
                    return HttpResponse::Error(400, "配置项 " + k + " 的值类型不支持");
                }
                if (!first) payload.push_back('\0');
                payload += k;
                payload.push_back('\0');
                payload += val;
                first = false;
            }
            if (first) return HttpResponse::Error(400, "请求体里没有任何配置项");

            // 改鉴权相关的键要看看是不是"从无鉴权变成有鉴权"。
            //
            // 只让新连接受限是不够的：之前无鉴权连进来的人（尤其是
            // 正在拉流的）会一直免费用下去。
            const bool touchesAuth = body.has("auth") || body.has("token");

            HttpResponse r = Call(Cmd::SetConfig, payload, 0, -1);

            if (touchesAuth && r.status == 200 && httpServer_ != nullptr &&
                !ServiceState::Instance().AuthToken().empty()) {
                httpServer_->RequestKickAll("已开启鉴权");
            }
            return r;
        }
        return HttpResponse::Error(405, "config 只支持 GET / POST");
    }
    if (res == "selftest" && method == "POST") {
        return Call(Cmd::SelfTest, "", 0, -1);
    }
    if (res == "stats" && method == "GET") {
        return Call(Cmd::Stats, "", 0, -1);
    }
    if (res == "log" && method == "GET") {
        return Call(Cmd::Log, PackArgs({req.queryParam("since", "0")}), 0, -1);
    }
    if (res == "shutdown" && method == "POST") {
        return Call(Cmd::Shutdown, "", 0, -1);
    }
    if (res == "restart" && method == "POST") {
        return Call(Cmd::Restart, "", 0, -1);
    }

    // ── 截图 / 流 / 触控 ──
    if (res == "capture" && (method == "GET" || method == "POST")) {
        return HandleCapture(req);
    }
    if (res == "stream" && method == "GET") {
        return HandleStream(req);
    }
    if (res == "touch" && method == "GET") {
        return HandleTouchStream(req);
    }
    if (res == "longpress" && method == "POST") {
        return HandleGesture(req, Cmd::LongPress, /*needsEnd=*/false);
    }
    if (res == "drag" && method == "POST") {
        return HandleGesture(req, Cmd::Drag, /*needsEnd=*/true);
    }
    if (res == "doubletap" && method == "POST") {
        return HandleGesture(req, Cmd::DoubleTap, /*needsEnd=*/false);
    }
    if (res == "key" && method == "POST") {
        return HandleKey(req);
    }
    if (res == "clipboard" && (method == "GET" || method == "POST")) {
        return HandleClipboard(req);
    }
    // 服务对外开关。GET 查询、POST 切换。
    if (res == "service" && (method == "GET" || method == "POST")) {
        if (method == "GET") {
            return Call(Cmd::ServiceSwitch, PackArgs({"status"}), 0, -1);
        }
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        const std::string op = b.has("on")
                                   ? (b.flag("on") ? "on" : "off")
                                   : b.str("action", "status");
        // kFlagForce：服务已关时仍然放行（Handle 开头那段只放行路径，
        // 这里是双保险，也覆盖 socket 侧）
        HttpResponse r = Call(Cmd::ServiceSwitch, PackArgs({op}), kFlagForce, -1);

        // 关掉服务时，**已经连上的客户端也要断**。
        //
        // 只挡新请求是不够的：一个正在拉流的网页会一直拿画面，
        // "关掉服务"对它等于没关。
        //
        // 这里只登记，真正的踢连接在响应写完之后（HttpServer 的
        // RunPendingKick）—— 现在踢会把自己的响应也断掉。
        if (op == "off" && r.status == 200 && httpServer_ != nullptr) {
            httpServer_->RequestKickAll("服务已关闭");
        }
        return r;
    }
    if (res == "running" && method == "GET") {
        return Call(Cmd::RunningApps, "", 0, -1);
    }
    if (res == "logfile" && method == "GET") {
        return Call(Cmd::LogFile, "", 0, -1);
    }
    if (res == "logstream" && method == "GET") {
        return HandleLogStream(req);
    }
    if (res == "params" && method == "GET") {
        // 画面流的可调参数（第 2 项需求：能查到画质/停检/帧率）
        return HandleStreamParams(req);
    }

    // 电源。用 POST：它是有副作用的操作，GET 会被浏览器/爬虫预取。
    if (res == "power" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        const std::string what = b.str("action", "reboot");
        return Call(Cmd::Power, PackArgs({what}), 0, -1);
    }
    // 屏幕方向。同样用 POST：有副作用。
    if (res == "rotate" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        const std::string what = b.str("to", b.str("action", "status"));
        return Call(Cmd::Rotate, PackArgs({what}), 0, -1);
    }
    if (res == "info" && method == "GET") {
        // ⚠️ 不能直接 `return Call(Cmd::Info, …)`。
        //
        // Cmd::Info 把结果填在 Reply **结构体的字段**里（width/height/
        // stride/format），那是 socket 协议的表达方式；而 HTTP 这一层
        // 只看应答的 JSON 正文。直接转发的结果是
        //   {"ok":true,"status":0,"error":"ok"}
        // —— 看着成功，其实一个有用字段都没有。
        //
        // 所以这里自己发一次 Capture 探帧（走的是和截图同一条路），
        // 再从 Reply 里把尺寸读出来拼成 JSON。
        Request r{};
        r.magic = kMagic;
        r.cmd   = static_cast<uint32_t>(Cmd::Info);
        ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
        if (p.fd >= 0) close(p.fd);
        if (p.reply.status != kOk) {
            return HttpResponse::Error(500, StatusName(p.reply.status));
        }

        Request c{};
        c.magic = kMagic;
        c.cmd   = static_cast<uint32_t>(Cmd::Capture);
        ReplyPacket cp = dispatcher_->Handle(c, "", -1, 0);
        ServiceState::Instance().CountRequest(static_cast<uint32_t>(Cmd::Capture),
                                              cp.reply.status);
        if (cp.fd >= 0) close(cp.fd);
        if (cp.reply.status != kOk) {
            return HttpResponse::Error(500, StatusName(cp.reply.status));
        }

        // 注入器的实际坐标范围。配置里为 0 表示"跟随显示"，那就用抓帧尺寸。
        const InjectorConfig& inj =
                ServiceState::Instance().GetInjectorConfig();
        const uint32_t touchW = inj.touchWidth  > 0 ? inj.touchWidth
                                                    : cp.reply.width;
        const uint32_t touchH = inj.touchHeight > 0 ? inj.touchHeight
                                                    : cp.reply.height;

        json::Writer w;
        w.Obj()
            .Field("ok", true)
            // 正在跑的是哪一版。tools/rc-update.sh verify 就比对它和本地二进制。
            .Field("buildId", SelfBuildId())
            .Field("primaryWidth", static_cast<int64_t>(cp.reply.width))
            .Field("primaryHeight", static_cast<int64_t>(cp.reply.height))
            .Field("primaryStride", static_cast<int64_t>(cp.reply.stride))
            .Field("primaryFormat", static_cast<int64_t>(cp.reply.format))
            // ⚠️ 触控坐标空间报的是**注入器的实际 ABS 范围**，不是显示尺寸。
            //
            // 客户端必须按这个空间发坐标 —— 服务端把 x/y 直接当 ABS 值
            // 写下去（inject_uinput.cpp 里没有缩放），而 Android 再把这个
            // 范围按比例映射到当前显示。两者不一致就会点偏：
            // 实测踩过，显示 720 宽而 ABS 范围是 1279，于是点正中央
            // 只落在 56% 处。
            //
            // 注入器的范围是**创建时定死的**（ioctl 改不了），所以它天然
            // 是一个稳定的归一化空间 —— 转屏、改分辨率都不影响它，
            // 前提是客户端拿到的就是这个值。
            .Field("touchWidth", static_cast<int64_t>(touchW))
            .Field("touchHeight", static_cast<int64_t>(touchH))
         .EndObj();
        return HttpResponse::Json(200, w.str());
    }
    if (res == "tap" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        if (!b.has("x") || !b.has("y")) {
            return HttpResponse::Error(400, "tap 需要 x 与 y");
        }
        // 触控坐标走请求头字段（不是 payload）
        Request r{};
        r.magic = kMagic; r.cmd = static_cast<uint32_t>(Cmd::Tap);
        r.x = static_cast<int32_t>(b.num("x"));
        r.y = static_cast<int32_t>(b.num("y"));
        if (!ParseGestureMs(b, "ms", kDefaultTapMs, &r.durationMs, &err)) return err;
        ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
        ServiceState::Instance().CountRequest(static_cast<uint32_t>(Cmd::Tap), p.reply.status);
        if (p.fd >= 0) close(p.fd);
        if (p.reply.status != kOk) {
            return HttpResponse::Error(500, StatusName(p.reply.status));
        }
        json::Writer w;
        w.Obj().Field("ok", true).Field("x", r.x).Field("y", r.y).EndObj();
        return HttpResponse::Json(200, w.str());
    }
    if (res == "swipe" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        for (const char* k : {"x1", "y1", "x2", "y2"}) {
            if (!b.has(k)) return HttpResponse::Error(400, std::string("swipe 需要 ") + k);
        }
        Request r{};
        r.magic = kMagic; r.cmd = static_cast<uint32_t>(Cmd::Swipe);
        r.x  = static_cast<int32_t>(b.num("x1"));
        r.y  = static_cast<int32_t>(b.num("y1"));
        r.x2 = static_cast<int32_t>(b.num("x2"));
        r.y2 = static_cast<int32_t>(b.num("y2"));
        if (!ParseGestureMs(b, "ms", kDefaultSwipeMs, &r.durationMs, &err)) return err;
        ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
        ServiceState::Instance().CountRequest(static_cast<uint32_t>(Cmd::Swipe), p.reply.status);
        if (p.fd >= 0) close(p.fd);
        if (p.reply.status != kOk) {
            return HttpResponse::Error(500, StatusName(p.reply.status));
        }
        json::Writer w;
        w.Obj().Field("ok", true).EndObj();
        return HttpResponse::Json(200, w.str());
    }

    // ── 应用 ──
    if (res == "apps") {
        if (seg.size() == 3 && method == "GET") {
            uint32_t flags = 0;
            if (req.queryParam("system", "0") != "0") flags |= kFlagIncludeSystem;
            if (req.queryParam("meta", "0") != "0")   flags |= kFlagWithMetadata;
            return Call(Cmd::ListApps, "", flags, -1);
        }
        if (seg.size() >= 4) {
            const std::string& pkg = seg[3];
            const bool hasAction = seg.size() >= 5;
            const std::string action = hasAction ? seg[4] : "";

            if (!hasAction && method == "GET") {
                return Call(Cmd::AppInfo, PackArgs({pkg}), 0, -1);
            }
            if (action == "launch" && method == "POST") {
                json::Value b; HttpResponse err;
                if (!ParseJsonBody(req, &b, &err)) return err;
                const std::string act = b.str("activity");
                return Call(Cmd::LaunchApp,
                            PackArgs({pkg, act}), 0, -1);
            }
            if (action == "kill" && method == "POST") {
                return Call(Cmd::KillApp, PackArgs({pkg}), 0, -1);
            }
        }
        return HttpResponse::Error(405, "apps 的路径或方法不对");
    }
    if (res == "foreground" && method == "GET") {
        return Call(Cmd::ForegroundApp, "", 0, -1);
    }
    if (res == "install" && method == "POST") {
        return HandleInstall(req);
    }

    // ── 下载与文件 ──
    if (res == "download" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        const std::string url = b.str("url");
        if (url.empty()) return HttpResponse::Error(400, "download 需要 url");
        return Call(Cmd::Download,
                    PackArgs({url, b.str("filename"), b.str("subdir")}),
                    0, -1);
    }
    if (res == "files") {
        if (method == "GET") {
            // GET 只允许查询操作。此前这里把任意 op 都转发到 FileOp，
            // 导致带 query 的 GET 也能触发 delete/mkdir 等写操作；
            // 写操作必须走 POST，避免预取、重试或链接扫描误触发变更。
            const std::string op = req.queryParam("op", "list");
            if (op != "roots" && op != "list" && op != "stat" &&
                op != "exists") {
                return HttpResponse::Error(
                        405, "GET /files 只支持 roots/list/stat/exists");
            }
            return Call(Cmd::FileOp,
                        PackArgs({op, req.queryParam("path")}), 0, -1);
        }
        if (method == "POST") {
            json::Value b; HttpResponse err;
            if (!ParseJsonBody(req, &b, &err)) return err;
            const std::string op = b.str("op");
            if (op.empty()) {
                return HttpResponse::Error(
                        400, "files 需要 op（list|stat|exists|mkdir|delete|rename）");
            }
            uint32_t flags = 0;
            if (b.flag("recursive")) flags |= kFlagRecursive;
            return Call(Cmd::FileOp,
                        PackArgs({op, b.str("path"), b.str("to")}),
                        flags, -1);
        }
        return HttpResponse::Error(405, "files 只支持 GET / POST");
    }

    // ⚠️ 这里用 **kErrBadCmd（4098）而不是 HTTP 的 404**。
    //
    // 早先传的是 404，于是 `status` 字段的含义变成了"有时候是协议码、
    // 有时候是 HTTP 码" —— 调用方没法统一处理：参数错的响应里
    // status=4099，路由错的响应里 status=404，两者都是 400 类错误
    // 却长得完全不一样。
    //
    // 现在 status **始终是协议状态码**，HTTP 状态码由这一层映射出来
    // （kErrBadCmd → 400）。唯一的例外是 401 和 503 —— 那是 HTTP 层
    // 独有的情况，没有对应的协议码（见 docs/api/05-errors.md）。
    // 传**协议码**而不是 HTTP 码：未知路由的本质是"不认识这个命令"，
    // 语义上是 kErrBadCmd，不是 kErrBadArg。Error() 会据此推出 HTTP 400。
    return HttpResponse::Error(static_cast<int>(kErrBadCmd),
                               "未知资源: " + res);
}

}  // namespace remote_control
