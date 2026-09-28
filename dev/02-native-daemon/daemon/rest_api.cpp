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
#include <signal.h>
#include <unistd.h>

#include <vector>

#include "autod_log.h"
#include "dispatch.h"
#include "json_parser.h"
#include "json_writer.h"
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
     .EndObj();
    // 上面那串只是说明，真正的参数表在下面
    std::string base = w.str();
    base.pop_back();   // 去掉结尾的 }
    base += ",\"params\":[";
    base += "{\"name\":\"fps\",\"range\":\"1-60\",\"desc\":\"帧率\"},";
    base += "{\"name\":\"quality\",\"range\":\"PNG 1-9 / JPEG,WebP 1-100\","
            "\"desc\":\"画质\"},";
    base += "{\"name\":\"format\",\"range\":\"auto|jpeg|webp|png\","
            "\"desc\":\"编码格式\"},";
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

    Request r{};
    r.magic = kMagic;
    r.cmd   = static_cast<uint32_t>(Cmd::Capture);
    ReplyPacket rp = dispatcher_->Handle(r, "", -1, 0);
    if (rp.reply.status != kOk || rp.fd < 0) {
        if (rp.fd >= 0) close(rp.fd);
        return {};
    }

    const uint32_t w = rp.reply.width, h = rp.reply.height;
    const uint64_t size = rp.reply.dataSize;
    const uint32_t fmt = rp.reply.format;
    void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, rp.fd, 0);
    if (base == MAP_FAILED) {
        close(rp.fd);
        return {};
    }

    if (fmt != 1 && fmt != 2 && fmt != 5) {
        munmap(base, size);
        close(rp.fd);
        return {};
    }

    const uint8_t* src = static_cast<const uint8_t*>(base);
    std::vector<uint8_t> rgba;
    if (fmt == 5) {   // BGRA → RGBA
        rgba.resize(size);
        for (uint64_t i = 0; i + 3 < size; i += 4) {
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
        munmap(base, size);
        close(rp.fd);
        *unchanged = true;
        return {};
    }
    st->lastHash = hash;
    st->haveLast = true;

    std::string perr;
    std::string out = ImageEncoder::Instance().Encode(enc, dw, dh,
                                                      static_cast<ImageFormat>(p.codec),
                                                      p.level, &perr);
    munmap(base, size);
    close(rp.fd);

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

    resp.streamer = [this, boundary, intervalMs, p](int fd) {
        StreamState st;
        while (true) {
            const int64_t t0 = NowMs();

            bool unchanged = false;
            std::string img = NextEncodedFrame(p, &st, &unchanged);
            if (img.empty()) {
                // 画面没变就整帧跳过：MJPEG 客户端会继续显示上一帧，
                // 这正是我们要的。出错也走这里，下一轮重试。
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
        return Call(Cmd::ServiceSwitch, PackArgs({op}), kFlagForce, -1);
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
