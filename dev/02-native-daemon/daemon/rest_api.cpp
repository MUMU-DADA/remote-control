// rest_api.cpp — REST 适配器实现

#include "rest_api.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

#include <vector>

#include "autod_log.h"
#include "dispatch.h"
#include "json_parser.h"
#include "json_writer.h"
#include "png_encoder.h"
#include "protocol.h"
#include "service_state.h"
#include "webui.h"

namespace autod {
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

// 盒式降采样。把 w×h 的 RGBA 缩到 (w*num/den) × (h*num/den)。
//
// 为什么在编码前缩：PNG 的编码耗时大致随像素数走，缩一半面积就少四分之三
// 的编码量。控制台用来看画面和点坐标，半分辨率完全够 —— 而且字节数小了，
// 弱网下帧率反而更高。
//
// 用盒式平均而不是最近邻：最近邻会把细线（文字、边框）整条丢掉，
// 看起来像画面在闪。
void DownscaleRgba(const uint8_t* src, uint32_t sw, uint32_t sh,
                   uint32_t dw, uint32_t dh, std::vector<uint8_t>* out) {
    out->resize(static_cast<size_t>(dw) * dh * 4);
    // 每个目标像素覆盖的源区域 [x0,x1) × [y0,y1)
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

// FNV-1a。用来判断画面有没有变 —— 比编码便宜得多，
// 静止画面（大多数时候）可以整帧跳过编码。
uint64_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
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
// **空的尾参数会被省略** —— 协议的约定是"没给的参数就是没给"，
// 而不是"给了一个空字符串"。比如 Download 的 filename 为空时，
// 服务端应该自己去 URL 里推断，而不是收到一个空文件名。
std::string PackArgs(std::initializer_list<std::string> args) {
    std::string out;
    bool first = true;
    for (const auto& a : args) {
        if (a.empty() && !first) continue;
        if (!first) out.push_back('\0');
        out += a;
        first = false;
    }
    return out;
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
    const std::string format = req.queryParam("format", "png");

    Request request{};
    request.magic = kMagic;
    request.cmd   = static_cast<uint32_t>(Cmd::Capture);
    request.flags = 0;

    ReplyPacket packet = dispatcher_->Handle(request, "", -1, 0);
    ServiceState::Instance().CountRequest(static_cast<uint32_t>(Cmd::Capture), packet.reply.status);

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

    // mmap 而不是 read：大帧少一次拷贝，而且后面编 PNG 要按行随机访问
    void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, packet.fd, 0);
    if (base == MAP_FAILED) {
        close(packet.fd);
        return HttpResponse::Error(500, std::string("mmap 失败: ") + strerror(errno));
    }

    HttpResponse resp;
    if (format == "raw") {
        resp.status = 200;
        resp.contentType = "application/octet-stream";
        resp.body.assign(static_cast<const char*>(base), size);
    } else {
        // 只处理 4 字节/像素的格式。别的（RGB_565 等）在传输层转没意义，
        // 明确报错让调用方知道要什么。
        const bool is4bpp = (fmt == 1 /*RGBA_8888*/ || fmt == 2 /*RGBX_8888*/ ||
                             fmt == 5 /*BGRA_8888*/);
        if (!is4bpp) {
            munmap(base, size);
            close(packet.fd);
            return HttpResponse::Error(
                    500, "像素格式 0x" + std::to_string(fmt) +
                             " 暂不支持编码（只支持 RGBA/RGBX/BGRA_8888）；"
                             "可用 ?format=raw 取原始像素");
        }

        // BGRA → RGBA：PNG 要求 RGBA 顺序
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
        if (!PngEncoder::Instance().Init(&perr)) {
            munmap(base, size);
            close(packet.fd);
            return HttpResponse::Error(500, perr);
        }
        std::string png = PngEncoder::Instance().EncodeRgba(src, w, h, 6, &perr);
        if (png.empty()) {
            munmap(base, size);
            close(packet.fd);
            return HttpResponse::Error(500, "PNG 编码失败: " + perr);
        }
        resp.status = 200;
        resp.contentType = "image/png";
        resp.body = std::move(png);
    }

    munmap(base, size);
    close(packet.fd);

    // 尺寸信息放在头里，方便调用方不解析图片就知道分辨率
    resp.extraHeaders.emplace_back("X-Autod-Width", std::to_string(w));
    resp.extraHeaders.emplace_back("X-Autod-Height", std::to_string(h));
    resp.extraHeaders.emplace_back("X-Autod-PixelFormat", std::to_string(fmt));
    return resp;
}

// ── 安装 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleInstall(const HttpRequest& req) {
    if (req.body.empty()) {
        return HttpResponse::Error(400, "请求体为空：请把 APK 字节作为请求体发送");
    }

    // 落成临时文件：协议要求通过 fd 传 APK，而 fd 背后得有个真实数据源。
    // 用 memfd 更干净，但 InstallApp 的实现会把它当普通 fd 顺序读，
    // 这里直接写 memfd 省掉一次磁盘往返。
    const int fd = memfd_create("apk-http", MFD_CLOEXEC);
    if (fd < 0) {
        return HttpResponse::Error(500, std::string("memfd_create 失败: ") +
                                            strerror(errno));
    }

    size_t off = 0;
    while (off < req.body.size()) {
        const ssize_t n = write(fd, req.body.data() + off, req.body.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return HttpResponse::Error(500, std::string("写入临时 fd 失败: ") +
                                                strerror(errno));
        }
        off += static_cast<size_t>(n);
    }
    lseek(fd, 0, SEEK_SET);

    const bool replace = req.queryParam("replace", "1") != "0";
    const uint32_t flags = replace ? static_cast<uint32_t>(kFlagReplace) : 0u;

    HttpResponse resp = Call(Cmd::InstallApp, "", flags, fd);
    close(fd);
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
    r.durationMs = static_cast<uint32_t>(b.num("ms", 0));
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

// ── 实时画面流 ──────────────────────────────────────────────────────────────
HttpResponse RestApi::HandleStream(const HttpRequest& req) {
    // 帧率：默认 5，限制在 1..30。
    // 上限不是为了省事 —— 服务端每帧都要抓屏 + 编 PNG，30fps 在
    // 低端设备上会把 CPU 吃满，而画面只会更卡。
    int fps = 5;
    {
        const std::string s = req.queryParam("fps", "5");
        char* end = nullptr;
        const long v = strtol(s.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && v > 0) fps = static_cast<int>(v);
    }
    if (fps > 30) fps = 30;
    if (fps < 1)  fps = 1;

    const std::string boundary = "autodframe";
    HttpResponse resp = HttpResponse::Stream(
            "multipart/x-mixed-replace; boundary=" + boundary);

    // 每帧之间至少隔这么久。抓帧本身就要几十毫秒，实际帧率会更低 ——
    // 与其追求标称帧率，不如保证"不会把设备打满"。
    const int intervalMs = 1000 / fps;

    // 降采样上限：默认 720 宽。0 = 不缩。
    int maxWidth = 720;
    {
        const std::string s = req.queryParam("maxWidth", "720");
        char* end = nullptr;
        const long v = strtol(s.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && v >= 0) maxWidth = static_cast<int>(v);
    }
    // PNG 压缩级别。默认 1（最快）—— 流的场景下带宽换帧率是划算的，
    // 而且降采样之后字节数本来就小了。
    int level = 1;
    {
        const std::string s = req.queryParam("quality", "1");
        char* end = nullptr;
        const long v = strtol(s.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && v >= 1 && v <= 9) level = static_cast<int>(v);
    }
    // 画面没变时是否跳过。默认开 —— 静止画面下编码开销直接归零。
    const bool skipUnchanged = req.queryParam("skipUnchanged", "1") != "0";

    resp.streamer = [this, boundary, intervalMs, maxWidth, level, skipUnchanged](
                            int fd) {
        if (!PngEncoder::Instance().Init(nullptr)) {
            const char* msg = "zlib 不可用，无法编码 PNG\n";
            ssize_t ig = write(fd, msg, strlen(msg));
            (void)ig;
            return;
        }

        uint64_t frameNo = 0;      // 实际发出的帧数
        uint64_t iterNo = 0;       // 循环次数（含跳过）
        uint64_t lastHash = 0;
        bool     haveLast = false;
        std::vector<uint8_t> scaled;

        while (true) {
            const int64_t t0 = NowMs();

            Request r{};
            r.magic = kMagic;
            r.cmd   = static_cast<uint32_t>(Cmd::Capture);
            ReplyPacket p = dispatcher_->Handle(r, "", -1, 0);
            if (p.reply.status != kOk || p.fd < 0) {
                if (p.fd >= 0) close(p.fd);
                usleep(intervalMs * 1000);
                continue;
            }

            const uint32_t w = p.reply.width, h = p.reply.height;
            const uint64_t size = p.reply.dataSize;
            const uint32_t fmt = p.reply.format;
            void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, p.fd, 0);
            if (base == MAP_FAILED) {
                close(p.fd);
                usleep(intervalMs * 1000);
                continue;
            }

            std::string png;
            bool wroteFrame = false;

            if (fmt != 1 && fmt != 2 && fmt != 5) {
                // 不支持的像素格式：跳过这一帧，但不终止整条流
                munmap(base, size);
                close(p.fd);
                usleep(intervalMs * 1000);
                continue;
            }

            // BGRA → RGBA
            const uint8_t* src = static_cast<const uint8_t*>(base);
            std::vector<uint8_t> rgba;
            if (fmt == 5) {
                rgba.resize(size);
                for (uint64_t i = 0; i + 3 < size; i += 4) {
                    rgba[i]     = src[i + 2];
                    rgba[i + 1] = src[i + 1];
                    rgba[i + 2] = src[i];
                    rgba[i + 3] = src[i + 3];
                }
                src = rgba.data();
            }

            // 变化检测要在**降采样之后**做：降采样本来就是平均，
            // 顺带把传感器噪声这类微小抖动滤掉了，跳过率更高。
            uint32_t dw = w, dh = h;
            const uint8_t* enc = src;
            if (maxWidth > 0 && w > static_cast<uint32_t>(maxWidth)) {
                dw = static_cast<uint32_t>(maxWidth);
                dh = static_cast<uint32_t>(static_cast<uint64_t>(h) * dw / w);
                if (dh == 0) dh = 1;
                DownscaleRgba(src, w, h, dw, dh, &scaled);
                enc = scaled.data();
            }

            const size_t encBytes = static_cast<size_t>(dw) * dh * 4;
            const uint64_t hash = HashBytes(enc, encBytes);

            if (skipUnchanged && haveLast && hash == lastHash) {
                // 画面没变 —— 整帧跳过编码。
                // 不写任何东西：MJPEG 客户端会继续显示上一帧，
                // 这正是我们要的。连接靠 TCP 自己保活。
                munmap(base, size);
                close(p.fd);
                ++iterNo;
                const int64_t elapsed = NowMs() - t0;
                const int64_t rest = intervalMs - elapsed;
                if (rest > 0) usleep(static_cast<useconds_t>(rest) * 1000);
                continue;
            }
            lastHash = hash;
            haveLast = true;

            std::string perr;
            png = PngEncoder::Instance().EncodeRgba(enc, dw, dh, level, &perr);

            munmap(base, size);
            close(p.fd);

            if (png.empty()) {
                usleep(intervalMs * 1000);
                continue;
            }

            std::string part;
            part.reserve(png.size() + 160);
            part += "--" + boundary + "\r\n";
            part += "Content-Type: image/png\r\n";
            part += "Content-Length: " + std::to_string(png.size()) + "\r\n";
            part += "X-Autod-Frame: " + std::to_string(frameNo) + "\r\n";
            part += "X-Autod-Width: " + std::to_string(dw) + "\r\n";
            part += "X-Autod-Height: " + std::to_string(dh) + "\r\n";
            part += "\r\n";
            part += png;
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

            ++frameNo;
            ++iterNo;
            wroteFrame = true;
            (void)wroteFrame;

            const int64_t elapsed = NowMs() - t0;
            const int64_t rest = intervalMs - elapsed;
            if (rest > 0) usleep(static_cast<useconds_t>(rest) * 1000);
        }
    };
    return resp;
}

// ── 路由 ────────────────────────────────────────────────────────────────────
HttpResponse RestApi::Handle(const HttpRequest& req) {
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
            w.Obj().Field("service", "autod")
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
            return Call(Cmd::SetConfig, payload, 0, -1);
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
    // 电源。用 POST：它是有副作用的操作，GET 会被浏览器/爬虫预取。
    if (res == "power" && method == "POST") {
        json::Value b; HttpResponse err;
        if (!ParseJsonBody(req, &b, &err)) return err;
        const std::string what = b.str("action", "reboot");
        return Call(Cmd::Power, PackArgs({what}), 0, -1);
    }
    if (res == "info" && method == "GET") {
        return Call(Cmd::Info, "", 0, -1);
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
        r.durationMs = static_cast<uint32_t>(b.num("ms", kDefaultTapMs));
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
        r.durationMs = static_cast<uint32_t>(b.num("ms", kDefaultSwipeMs));
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
            return Call(Cmd::FileOp,
                        PackArgs({"list", req.queryParam("path")}), 0, -1);
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

    return HttpResponse::Error(404, "未知资源: " + res);
}

}  // namespace autod
