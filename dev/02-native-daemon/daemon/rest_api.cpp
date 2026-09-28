// rest_api.cpp — REST 适配器实现

#include "rest_api.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include <vector>

#include "autod_log.h"
#include "dispatch.h"
#include "json_parser.h"
#include "json_writer.h"
#include "encode_pool.h"
#include "frame_hub.h"
#include "image_encoder.h"
#include "png_encoder.h"
#include "protocol.h"
#include "service_state.h"
#include "websocket.h"
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
    resp.extraHeaders.push_back({"X-Autod-Width", std::to_string(w)});
    resp.extraHeaders.push_back({"X-Autod-Height", std::to_string(h)});
    resp.extraHeaders.push_back({"X-Autod-PixelFormat", std::to_string(fmt)});

    if (codec == ImageFormat::kRaw) {
        resp.status = 200;
        resp.contentType = "application/octet-stream";
        resp.extraHeaders.push_back({"X-Autod-Stride",
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

    if (fromBody) {
        if (req.body.empty()) {
            return HttpResponse::Error(
                    400, "请求体为空：把 APK 字节作为请求体发送，"
                         "或用 ?path= 指定设备上已有的文件");
        }
        // 放 /sdcard 而不是 /data/local/tmp：installer 对两者都能读，
        // 但 /sdcard 上的文件用户自己也能看见 —— 出问题时好排查。
        path = "/sdcard/autod-upload-" + std::to_string(getpid()) + ".apk";

        const int wfd = open(path.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        if (wfd < 0) {
            return HttpResponse::Error(500, "无法创建 " + path + ": " +
                                                strerror(errno));
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
        ALOGI("收到上传的 APK: %s（%zu 字节）", path.c_str(), req.body.size());
    } else if (access(path.c_str(), R_OK) != 0) {
        return HttpResponse::Error(404, "找不到文件: " + path);
    }

    // 交给 InstallApp。它按 fd 顺序读，所以这里把文件打开成 fd。
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        // 打开失败也要删 —— 上传上来的文件已经落地了，
        // 不删就是垃圾。只有 keep=1 才留。
        const bool keep = req.queryParam("keep", "0") == "1";
        if (!keep) unlink(path.c_str());
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
    r.durationMs = static_cast<uint32_t>(v.num("ms", 0));
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

// ── 画面流的可调参数 ────────────────────────────────────────────────────────
//
// 让调用方能**查到**流支持哪些参数、当前默认值是什么，而不是去翻文档。
HttpResponse RestApi::HandleStreamParams(const HttpRequest& req) {
    const StreamParams def;   // 内置默认
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
            // 开着 AUTOD_FORCE_FALLBACK 时如实标出来 ——
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
    const bool lossy = (fmt == ImageFormat::kJpeg || fmt == ImageFormat::kWebp);
    if (lossy) {
        if (out->level < 1 || out->level > 100) {
            out->level = (fmt == ImageFormat::kWebp) ? 80 : 75;
        }
    } else {
        if (out->level < 1 || out->level > 9) out->level = 1;
    }
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

// FNV-1a。判断画面有没有变 —— 比编码便宜得多。
uint64_t HashBytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
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
        st->hubSub = FrameHub::Instance().Subscribe(&err);
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
    FramePtr f = FrameHub::Instance().WaitNext(st->hubSeq, waitMs, &latestSeq);
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

    // 变化检测放在降采样之后：降采样本来就是平均，顺带把传感器噪声
    // 这类微小抖动滤掉了，跳过率更高。
    const size_t encBytes = static_cast<size_t>(dw) * dh * 4;
    const uint64_t hash = HashBytes(enc, encBytes);
    if (p.skipUnchanged && st->haveLast && hash == st->lastHash) {
        *unchanged = true;
        return {};
    }
    st->lastHash = hash;
    st->haveLast = true;

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
            part += "X-Autod-Frame: " + std::to_string(st.frameNo) + "\r\n";
            part += "X-Autod-Width: " + std::to_string(st.outW) + "\r\n";
            part += "X-Autod-Height: " + std::to_string(st.outH) + "\r\n";
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
                            WsWriteText(fd, "{\"t\":\"ack\",\"fps\":" +
                                                std::to_string(nf) + "}");
                        } else if (t == "quality") {
                            const int nq = ClampInt(std::to_string(v.num("v", p.level)),
                                                    1, 100, p.level);
                            const bool lossy =
                                (p.codec == static_cast<int>(ImageFormat::kJpeg) ||
                                 p.codec == static_cast<int>(ImageFormat::kWebp));
                            p.level = lossy ? nq : ((nq > 9) ? 9 : nq);
                            st.haveLast = false;
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
                                st.haveLast = false;   // 换了格式，缓存作废
                                WsWriteText(fd,
                                    std::string("{\"t\":\"ack\",\"format\":\"") +
                                        ImageEncoder::Name(nf) + "\"}");
                            }
                        } else if (t == "skipUnchanged") {
                            p.skipUnchanged = (v.num("v", 1) != 0);
                            st.haveLast = false;   // 重新开始判定
                            WsWriteText(fd, std::string("{\"t\":\"ack\","
                                    "\"skipUnchanged\":") +
                                    (p.skipUnchanged ? "true" : "false") + "}");
                        } else if (t == "refresh") {
                            // 客户端主动要求"下一帧无论变没变都发"，
                            // 用于页面重新可见时立刻刷新一次。
                            st.haveLast = false;
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
            if (img.empty()) continue;      // 没变或出错，下一轮再看

            // 二进制帧。第一帧附带尺寸信息（之后客户端就不用再解析了）——
            // 单独发一条文本消息会让客户端在"有图没尺寸"的窗口里没法画。
            if (sent == 0) {
                json::Writer w;
                w.Obj()
                    .Field("t", "size")
                    .Field("w", static_cast<int64_t>(st.outW))
                    .Field("h", static_cast<int64_t>(st.outH))
                 .EndObj();
                if (!WsWriteText(fd, w.str())) return;
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

        json::Writer w;
        w.Obj()
            .Field("ok", true)
            .Field("primaryWidth", static_cast<int64_t>(cp.reply.width))
            .Field("primaryHeight", static_cast<int64_t>(cp.reply.height))
            .Field("primaryStride", static_cast<int64_t>(cp.reply.stride))
            .Field("primaryFormat", static_cast<int64_t>(cp.reply.format))
            .Field("touchWidth", static_cast<int64_t>(p.reply.width))
            .Field("touchHeight", static_cast<int64_t>(p.reply.height))
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

}  // namespace autod
