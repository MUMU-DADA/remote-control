// http_server.cpp — HTTP/JSON 传输层

#include "http_server.h"

#include "secure_compare.h"

#include "protocol.h"   // kErr* 协议状态码（Error 要把它写进 status 字段）

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include "thread_util.h"

#include "remote_control_log.h"
#include "json_writer.h"

namespace remote_control {
namespace {

constexpr int kReadTimeoutSec = 30;

// 流式响应的**发送**超时。
//
// 客户端不读了（进程被杀 / 卡住 / 网断了）时 write 会永远阻塞，
// 而流式循环靠 write 失败发现断开、释放 FrameHub 的订阅。
// 没有这个超时，一个僵尸客户端能让服务端一直按满速给它抓帧+编码，
// 永不停止。
//
// 实测踩过：被杀掉的测试客户端留下一条 Send-Q 堆到 14KB 的连接，
// 服务端仍在 60fps 地抓帧，看起来像"抓帧率不跟随需求"。
//
// 10 秒是宽松值：真正慢的客户端是"读得慢但一直在读"，
// 每一帧都能写出去一部分，攒不到 10 秒。
constexpr int kSendTimeoutSec = 10;
constexpr size_t kMaxHeaderBytes = 64 * 1024;

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX 解码。'+' 解成空格只在 query 里成立（表单约定），
// 路径段里的 '+' 就是加号本身 —— 所以用一个开关区分。
std::string UrlDecode(const std::string& s, bool plusIsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = HexVal(s[i + 1]);
            const int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (plusIsSpace && s[i] == '+') { out += ' '; continue; }
        out += s[i];
    }
    return out;
}

const char* StatusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

}  // namespace

// ── HttpRequest / HttpResponse ──────────────────────────────────────────────
std::string HttpRequest::queryParam(const std::string& key,
                                    const std::string& def) const {
    for (const auto& kv : query) {
        if (kv.first == key) return kv.second;
    }
    return def;
}

std::string HttpRequest::header(const std::string& lowerKey,
                                const std::string& def) const {
    for (const auto& kv : headers) {
        if (kv.first == lowerKey) return kv.second;
    }
    return def;
}

bool HttpRequest::ReadBody(std::string* out, std::string* error) const {
    if (bodyFile.empty()) {
        *out = body;
        return true;
    }
    const int fd = open(bodyFile.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "打开 " + bodyFile + " 失败: " + strerror(errno);
        return false;
    }
    out->clear();
    out->reserve(bodySize);
    char buf[64 * 1024];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("读 ") + bodyFile + " 失败";
            close(fd);
            return false;
        }
        if (n == 0) break;
        out->append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return true;
}

HttpResponse HttpResponse::Json(int status, const std::string& json) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = json;
    return r;
}

HttpResponse HttpResponse::Text(int status, const std::string& text) {
    HttpResponse r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = text;
    return r;
}

HttpResponse HttpResponse::Stream(const std::string& contentType) {
    HttpResponse r;
    r.status = 200;
    r.contentType = contentType;
    return r;
}

HttpResponse HttpResponse::WebSocket(const std::string& accept) {
    HttpResponse r;
    r.status = 101;
    r.wsAccept = accept;
    return r;
}

// HTTP 状态码 → 协议状态码。
//
// 为什么要有这个映射：`status` 字段必须是**协议状态码**，否则调用方
// 没法统一处理 —— 参数错的响应里 status=4099，路由错的响应里 status=404，
// 两者都是 400 类错误却长得完全不一样。
//
// 映射表只覆盖有对应协议码的那几个。401 和 503 是 HTTP 层独有的情况
// （需要令牌 / 服务被软开关关掉），没有协议码可用，**原样保留** ——
// 这一点在 docs/api/05-errors.md 里明确写了。
static int ToProtocolStatus(int code) {
    // 已经给了协议码（≥ 0x1000）就原样用
    if (code >= 0x1000) return code;
    switch (code) {
        case 400: return remote_control::kErrBadArg;
        case 403: return remote_control::kErrPermission;
        case 404: return remote_control::kErrNotFound;
        case 500: return remote_control::kErrInternal;
        case 501: return remote_control::kErrUnsupported;
        case 504: return remote_control::kErrTimeout;
        default:  return code;   // 401 / 503 等 HTTP 层专属，保留原值
    }
}

// 反向：协议码 → HTTP 状态码。调用方直接传协议码时用。
static int ToHttpStatus(int proto) {
    switch (proto) {
        case remote_control::kErrBadMagic:
        case remote_control::kErrBadCmd:
        case remote_control::kErrBadArg:
        case remote_control::kErrPayload:     return 400;
        case remote_control::kErrPermission:  return 403;
        case remote_control::kErrNotFound:    return 404;
        case remote_control::kErrUnsupported: return 501;
        case remote_control::kErrTimeout:     return 504;
        default:                     return 500;
    }
}

HttpResponse HttpResponse::Error(int status, const std::string& message) {
    HttpResponse r;
    // 调用方可以传 HTTP 状态码（400）也可以传协议码（kErrBadCmd）。
    // 两种都支持是因为调用点写什么的都有，而**输出必须统一**。
    r.status = (status >= 0x1000) ? ToHttpStatus(status) : status;
    // status 字段用协议码；HTTP 状态码保持调用方给的那个（两者已经对应上）
    json::Writer w;
    w.Obj().Field("ok", false)
           .Field("status", static_cast<uint64_t>(ToProtocolStatus(status)))
           .Field("error", message)
     .EndObj();
    r.body = w.str();
    r.contentType = "application/json; charset=utf-8";
    return r;
}

// ── HttpServer ──────────────────────────────────────────────────────────────
HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(const Options& opts, std::string* error) {
    bindAddr_ = opts.bindAddr;
    port_     = opts.port;
    token_    = opts.token;
    maxBody_  = opts.maxBodyBytes;
    spoolThreshold_ = opts.spoolThresholdBytes;
    maxConns_ = opts.maxConns > 0 ? opts.maxConns : 128;
    spoolDir_ = opts.spoolDir;

    const bool loopbackOnly = (opts.bindAddr == "127.0.0.1" ||
                               opts.bindAddr == "::1" ||
                               opts.bindAddr == "localhost");
    // 绑定到非回环地址却不开鉴权时**只告警，不拒绝启动**。
    //
    // 早先这里是硬拒绝，理由是"这是个能截图、注入触控、装应用、
    // 删文件的接口"。但产品要求首启就是无鉴权模式，用户自己决定
    // 什么时候开 —— 硬拒绝会让"先绑 0.0.0.0 试试"这种正常操作
    // 直接起不来。所以降级成一条**显眼的告警**，并把状态如实反映到
    // /api/v1/config 里，让上位机能看到。
    if (!loopbackOnly && opts.token.empty()) {
        ALOGW("⚠️  HTTP API 绑定到 %s 且**未开启鉴权** —— 同网络的任何人都能"
              "完全控制本设备（截图、触控、装应用、删文件）。"
              "要收紧请在 /sdcard/remote-control.conf 里设 auth=1",
              opts.bindAddr.c_str());
    }

    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(opts.port);
    if (inet_pton(AF_INET, opts.bindAddr.c_str(), &addr.sin_addr) != 1) {
        // 不是点分十进制就当 127.0.0.1（也支持 "localhost"）
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error) {
            *error = "bind(" + opts.bindAddr + ":" + std::to_string(opts.port) +
                     ") 失败: " + strerror(errno);
        }
        close(fd);
        return false;
    }
    if (listen(fd, 16) < 0) {
        if (error) *error = std::string("listen: ") + strerror(errno);
        close(fd);
        return false;
    }

    // port 传 0 时由内核分配，回读实际端口 —— 测试常用
    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
        port_ = ntohs(actual.sin_port);
    }

    listenFd_ = fd;
    stop_ = false;
    ALOGI("HTTP API 就绪: http://%s:%u/（%s）", bindAddr_.c_str(), port_,
          token_.empty() ? "无鉴权" : "需要访问令牌");
    return true;
}

bool HttpServer::CheckAuth(const HttpRequest& req) const {
    // 令牌优先从 provider 取（可以热改），没有就用手里的那份。
    std::string tok = token_;
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        if (tokenProvider_) tok = tokenProvider_();
    }
    if (tok.empty()) return true;         // 无鉴权模式

    // 网页本身不校验：它只是个静态页面，不含秘密，
    // 而用户得先打开它才有地方输入令牌。
    // 其余（/api/ 下的一切）都要校验。
    const std::string& path = req.path;
    const bool isPage = (path == "/" || path == "/index.html" || path == "/ui");
    if (isPage) return true;

    // 1) Authorization: Bearer <token>  —— 标准做法
    // ⚠️ 三处比较都必须用 ConstantTimeEquals，不能用 `==`。
    //    std::string 的 == 是短路的（第一个不同的字节就返回），
    //    攻击者可以靠测响应时间逐字节猜出令牌 —— 局域网里没有互联网
    //    那种抖动噪声，这种攻击完全可行。见 secure_compare.h。
    const std::string auth = req.header("authorization");
    if (auth.size() > 7 && auth.compare(0, 7, "Bearer ") == 0 &&
        ConstantTimeEquals(auth.substr(7), tok)) {
        return true;
    }
    // 2) X-Remote-Control-Token: <token>  —— 给不方便设 Authorization 的客户端
    if (ConstantTimeEquals(req.header("x-remote-control-token"), tok)) return true;
    // 3) ?token=<token>  —— 给 <img src="/api/v1/stream?..."> 这种
    //    没法自定义请求头的场景。
    //    ⚠️ 令牌会出现在 URL 里，可能被日志和浏览器历史记录留下。
    //    只在确实没法带头的场合用它。
    if (ConstantTimeEquals(req.queryParam("token"), tok)) return true;

    return false;
}

void HttpServer::RequestKickAll(const char* reason) {
    std::lock_guard<std::mutex> lk(connMutex_);
    // 已经有一条待执行的就别覆盖 —— 先来的原因更有说明性
    if (pendingKick_.empty()) {
        pendingKick_ = reason != nullptr ? reason : "";
    }
}

void HttpServer::RunPendingKick(int exceptFd) {
    std::string reason;
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        reason.swap(pendingKick_);
    }
    if (reason.empty()) return;
    KickAllConnections(reason.c_str(), 600, exceptFd);
}

int HttpServer::KickAllConnections(const char* reason, int timeoutMs,
                                   int exceptFd) {
    // 先数一下，好在日志里说清楚"踢了几条" ——
    // 不然调用方只能从"客户端好像断了"间接猜。
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        for (int fd : connFds_) if (fd != exceptFd) ++n;
        for (int fd : connFds_) {
            if (fd == exceptFd) continue;   // 发起请求的那条，让它把响应发完
            // shutdown 而不是 close：fd 还归连接线程所有，
            // 我们只是让它上面的阻塞读写立刻失败。
            shutdown(fd, SHUT_RDWR);
        }
    }
    if (n == 0) return 0;

    // 等它们真的退出。超时也继续往下走 —— 调用方可能是"关服务"，
    // 卡在这里会让那个 API 请求一直不返回。
    const int stepMs = 10;
    int waited = 0;
    while (waited < timeoutMs) {
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            if (connFds_.empty()) break;
        }
        usleep(stepMs * 1000);
        waited += stepMs;
    }

    size_t left = 0;
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        left = connFds_.size();
    }
    ALOGI("终止了 %zu 条连接（%s），等了 %d ms%s", n - left,
          reason != nullptr ? reason : "", waited,
          left == 0 ? "" : "，仍有残留");
    if (left != 0) {
        ALOGW("仍有 %zu 条连接没退出", left);
    }
    return waited;
}

void HttpServer::SetTokenProvider(std::function<std::string()> fn) {
    std::lock_guard<std::mutex> lk(connMutex_);
    tokenProvider_ = std::move(fn);
}

void HttpServer::Stop() {
    stop_ = true;

    // 先把活跃连接踢掉，再关监听 fd。
    //
    // 顺序很重要：流式响应（MJPEG / WebSocket）的回调会一直循环到
    // write 失败为止。不主动 shutdown 的话，客户端不松手它们就永远
    // 不退出，而 main() 随后就会析构 Dispatcher —— 那些线程再去碰
    // 它的操作锁就是一个已销毁的互斥量。
    KickAllConnections("服务关闭");

    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
}

void HttpServer::Run(const HttpHandler& handler) {
    while (!stop_) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        const int connFd = accept4(listenFd_, reinterpret_cast<sockaddr*>(&peer),
                                   &peerLen, SOCK_CLOEXEC);
        if (connFd < 0) {
            if (errno == EINTR) continue;
            if (stop_) break;
            ALOGW("HTTP accept 失败: %s", strerror(errno));
            continue;
        }

        // 并发连接上限。**每连接一线程** —— 没有上限的话，一个客户端
        // 狂开连接就能把线程和 fd 耗光（实测原本确实没有上限）。
        // 超限时明确回 503 再关，比默默排队或直接崩要好排查。
        {
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lk(connMutex_);
                n = connFds_.size();
            }
            if (n >= maxConns_) {
                ALOGW("HTTP 连接数已达上限 %zu，拒绝新连接", maxConns_);
                const char* body =
                    "{\"ok\":false,\"error\":\"并发连接数已达上限，稍后重试\"}";
                std::string head =
                    std::string("HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Type: application/json; charset=utf-8\r\n"
                                "Retry-After: 1\r\n"
                                "Content-Length: ") +
                    std::to_string(strlen(body)) +
                    "\r\nConnection: close\r\n\r\n" + body;
                ssize_t ig = write(connFd, head.data(), head.size());
                (void)ig;
                close(connFd);
                continue;
            }
        }

        timeval tv{};
        tv.tv_sec = kReadTimeoutSec;
        setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(connFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // 和 socket 服务端一样：每连接一线程。
        // 一个慢客户端不该让整个 API 卡住。
        // 线程起不来就退回串行处理 —— 至少不丢这次连接。
        // 用 SpawnDetached 而不是 std::thread：后者创建失败会抛异常，
        // 而 AOSP 是 -fno-exceptions，抛出去就是整个进程 terminate。
        if (!SpawnDetached([this, connFd, &handler]() {
                ServeConnection(connFd, handler);
                close(connFd);
            })) {
            ALOGW("HTTP: 起线程失败，本连接串行处理");
            ServeConnection(connFd, handler);
            close(connFd);
        }
    }
    ALOGI("HTTP accept 循环退出");
}

bool HttpServer::ReadRequest(int connFd, HttpRequest* out,
                            HttpResponse* errReply,
                            const std::function<bool(const HttpRequest&)>&
                                    onHeaders) {
    std::string buf;
    buf.reserve(4096);

    // 一直读到 \r\n\r\n —— 头结束
    size_t headerEnd = std::string::npos;
    char tmp[4096];
    while (headerEnd == std::string::npos) {
        const ssize_t n = read(connFd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            *errReply = HttpResponse::Error(400, std::string("读请求失败: ") +
                                                     strerror(errno));
            return false;
        }
        if (n == 0) {
            *errReply = HttpResponse::Error(400, "连接在请求头读完前关闭");
            return false;
        }
        buf.append(tmp, static_cast<size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > kMaxHeaderBytes) {
            *errReply = HttpResponse::Error(413, "请求头过大");
            return false;
        }
    }

    const std::string head = buf.substr(0, headerEnd);
    std::string body = buf.substr(headerEnd + 4);

    // 请求行
    const size_t lineEnd = head.find("\r\n");
    const std::string reqLine = head.substr(0, lineEnd);
    {
        const size_t s1 = reqLine.find(' ');
        const size_t s2 = (s1 == std::string::npos) ? std::string::npos
                                                    : reqLine.find(' ', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos) {
            *errReply = HttpResponse::Error(400, "请求行格式不对");
            return false;
        }
        out->method = reqLine.substr(0, s1);
        const std::string target = reqLine.substr(s1 + 1, s2 - s1 - 1);
        out->rawPath = target;

        const size_t q = target.find('?');
        out->path = UrlDecode(q == std::string::npos ? target
                                                     : target.substr(0, q),
                              /*plusIsSpace=*/false);
        if (q != std::string::npos) {
            const std::string qs = target.substr(q + 1);
            size_t start = 0;
            while (start <= qs.size()) {
                size_t amp = qs.find('&', start);
                if (amp == std::string::npos) amp = qs.size();
                const std::string pair = qs.substr(start, amp - start);
                const size_t eq = pair.find('=');
                if (eq == std::string::npos) {
                    if (!pair.empty()) out->query.emplace_back(
                            UrlDecode(pair, true), "");
                } else {
                    out->query.emplace_back(UrlDecode(pair.substr(0, eq), true),
                                            UrlDecode(pair.substr(eq + 1), true));
                }
                if (amp == qs.size()) break;
                start = amp + 1;
            }
        }
    }

    // 头
    size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos) eol = head.size();
        const std::string line = head.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            out->headers.emplace_back(ToLower(Trim(line.substr(0, colon))),
                                      Trim(line.substr(colon + 1)));
        }
        pos = eol + 2;
    }

    // 正文：只支持 Content-Length。chunked 对请求体是罕见的，
    // 真遇到了明确拒绝比半懂不懂地解析更安全。
    const std::string te = out->header("transfer-encoding");
    if (!te.empty() && ToLower(te).find("chunked") != std::string::npos) {
        *errReply = HttpResponse::Error(400, "不支持 Transfer-Encoding: chunked");
        return false;
    }

    // 头读完了，正文**还没读** —— 给调用方一个提前拒绝的机会
    // （鉴权就走这里，见 ServeConnection）。
    if (onHeaders && !onHeaders(*out)) return false;

    const std::string clStr = out->header("content-length");
    size_t contentLength = 0;
    if (!clStr.empty()) {
        char* end = nullptr;
        const long long v = strtoll(clStr.c_str(), &end, 10);
        if (v < 0) {
            *errReply = HttpResponse::Error(400, "Content-Length 非法");
            return false;
        }
        contentLength = static_cast<size_t>(v);
        if (contentLength > maxBody_) {
            *errReply = HttpResponse::Error(413, "请求体超过上限 " +
                                                     std::to_string(maxBody_) + " 字节");
            return false;
        }
    }

    // ── 大请求体落盘 ──
    //
    // 超过阈值的写进临时文件，不留在内存里。
    //
    // 为什么：APK 动辄几百 MB，而设备总共才几 GB 内存。早先全量读进
    // std::string，上限只能定 64 MB —— 稍大的 APK 直接 413，
    // 网页上传表现为"传不上去"。
    //
    // ⚠️ 阈值判断用的是 **Content-Length**，是客户端说了算的值。
    //    所以落盘这条路也不能让 body 无限增长 —— 往下写多少磁盘就是
    //    多少，最后靠 maxBody_ 兜底（它在上面已经查过了）。
    const bool spool = (contentLength > spoolThreshold_);

    std::string  spoolPath;
    int          spoolFd = -1;
    if (spool) {
        const std::string dir = spoolDir_.empty() ? "/data/local/tmp"
                                                  : spoolDir_;
        spoolPath = dir + "/remote-control-body-" + std::to_string(getpid()) + "-" +
                    std::to_string(reinterpret_cast<uintptr_t>(out)) + ".tmp";
        spoolFd = open(spoolPath.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (spoolFd < 0) {
            *errReply = HttpResponse::Error(
                    500, "无法创建落盘文件 " + spoolPath + ": " +
                                 strerror(errno));
            return false;
        }
        // 已经把头部 buf 里带过来的那截正文写进去
        if (!body.empty()) {
            if (write(spoolFd, body.data(), body.size()) !=
                static_cast<ssize_t>(body.size())) {
                close(spoolFd);
                unlink(spoolPath.c_str());
                *errReply = HttpResponse::Error(500, "写落盘文件失败");
                return false;
            }
        }
    }

    size_t written = body.size();
    while (written < contentLength) {
        const ssize_t n = read(connFd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (spoolFd >= 0) { close(spoolFd); unlink(spoolPath.c_str()); }
            *errReply = HttpResponse::Error(400, "读请求体失败");
            return false;
        }
        if (n == 0) {
            if (spoolFd >= 0) { close(spoolFd); unlink(spoolPath.c_str()); }
            *errReply = HttpResponse::Error(400, "请求体不完整");
            return false;
        }
        if (spoolFd >= 0) {
            const char* p = tmp;
            ssize_t left = n;
            while (left > 0) {
                const ssize_t w = write(spoolFd, p, static_cast<size_t>(left));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    close(spoolFd);
                    unlink(spoolPath.c_str());
                    *errReply = HttpResponse::Error(500, "写落盘文件失败");
                    return false;
                }
                p += w;
                left -= w;
            }
        } else {
            body.append(tmp, static_cast<size_t>(n));
        }
        written += static_cast<size_t>(n);
    }

    out->bodySize = contentLength;
    if (spoolFd >= 0) {
        fsync(spoolFd);
        close(spoolFd);
        out->bodyFile = spoolPath;
        out->body.clear();          // 落盘之后内存里不留
    } else {
        body.resize(contentLength);
        out->body = std::move(body);
    }

    // 鉴权**不在这里**做。
    //
    // 这里原本有一份检查，但它和 ServeConnection 里的 CheckAuth 是
    // 两套逻辑，而且这版更弱：不支持 ?token=、也不放行网页本身。
    // 两份检查并存的结果是"改了一处以为生效了，实际被另一处先拦下"。
    // 统一到 CheckAuth 一处 —— 鉴权这种事，入口越少越好。

    return true;
}

namespace {
// 作用域退出时跑一次回调。给"响应写完之后再执行待办"用。
struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() { if (fn) fn(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    explicit ScopeExit(std::function<void()> f) : fn(std::move(f)) {}
};
}  // namespace

void HttpServer::ServeConnection(int connFd, const HttpHandler& handler) {
    // 无论从哪条路径返回（出错 / 流式结束 / 正常写完），
    // 都要执行一次待办的踢连接。
    ScopeExit kickGuard([this, connFd]() { RunPendingKick(connFd); });

    HttpRequest req;
    HttpResponse errReply;
    // 提前鉴权：在**读正文之前**判。
    // 早先鉴权在 ReadRequest 之后，也就是先收完（并落盘）整个请求体
    // 再拒 —— 未授权的客户端仅凭发大包就能占满磁盘和线程。
    HttpResponse earlyDeny;
    bool denied = false;
    const bool headersOk = ReadRequest(
            connFd, &req, &errReply, [&](const HttpRequest& h) -> bool {
                if (CheckAuth(h)) return true;
                denied = true;
                earlyDeny = HttpResponse::Error(
                        401, "需要访问令牌。请在页面顶部填入，或用 "
                             "Authorization: Bearer <token> / X-Remote-Control-Token: <token>");
                return false;
            });
    if (denied) {
        ALOGW("HTTP 拒绝（缺少或错误的令牌）: %s %s", req.method.c_str(),
              RedactToken(req.rawPath).c_str());
        std::string head =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "WWW-Authenticate: Bearer realm=\"remote-control\"\r\n"
                "Content-Length: " + std::to_string(earlyDeny.body.size()) +
                "\r\nConnection: close\r\n\r\n";
        if (write(connFd, head.data(), head.size()) > 0) {
            ssize_t ig = write(connFd, earlyDeny.body.data(),
                               earlyDeny.body.size());
            (void)ig;
        }
        // 正文没读。直接关会让对端收到 RST、可能看不到上面那个 401，
        // 所以先把在途数据**限量**排掉（上限 1MB，超了就直接关）——
        // 不能无限排，否则又变成"未授权也能让我们读 4GB"。
        {
            timeval tiny{};
            tiny.tv_usec = 200 * 1000;
            setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tiny, sizeof(tiny));
            char sink[16 * 1024];
            size_t drained = 0;
            while (drained < (1u << 20)) {
                const ssize_t n = read(connFd, sink, sizeof(sink));
                if (n <= 0) break;
                drained += static_cast<size_t>(n);
            }
        }
        return;
    }
    if (!headersOk) {
        // 读失败时也要把错误应答发回去，否则客户端只能看到连接被关
        const std::string head =
                "HTTP/1.1 " + std::to_string(errReply.status) + " " +
                StatusText(errReply.status) + "\r\n" +
                "Content-Type: " + errReply.contentType + "\r\n" +
                "Content-Length: " + std::to_string(errReply.body.size()) + "\r\n" +
                "Connection: close\r\n\r\n" + errReply.body;
        ssize_t ignored = write(connFd, head.data(), head.size());
        (void)ignored;
        return;
    }

    // 鉴权已经在 ReadRequest 的 onHeaders 回调里做完了 —— 那时候
    // 正文还没读，未授权的请求一个字节都不会落盘。
    // **不留第二份检查**：两处并存的结果是改了一处以为生效了、
    // 实际被另一处先拦下（这个坑本项目踩过一次）。

    // 直接调用，不包 try/catch。
    //
    // AOSP 是 -fno-exceptions，写了也编不过；而且我们全程不用异常，
    // 处理器本来就不抛。真出了 bad_alloc 这类，-fno-exceptions 下
    // 本来就是 abort，catch 也救不回来。
    const HttpResponse resp = handler(req);

    // 登记本连接，让 Stop() 能把它踢掉
    {
        std::lock_guard<std::mutex> lk(connMutex_);
        connFds_.insert(connFd);
    }
    struct ConnGuard {
        HttpServer* self;
        int fd;
        ~ConnGuard() {
            std::lock_guard<std::mutex> lk(self->connMutex_);
            self->connFds_.erase(fd);
        }
    } connGuard{this, connFd};

    // ── WebSocket 升级 ──
    //
    // 必须在普通响应分支之前处理：101 响应没有 Content-Length，
    // 头也不一样（Upgrade / Connection: Upgrade）。
    if (resp.isWebSocket()) {
        std::string head =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + resp.wsAccept + "\r\n\r\n";
        if (write(connFd, head.data(), head.size()) < 0) return;

        // 升级之后这条连接是长连接，不能再让它带着读超时 ——
        // ReadRequest 设了 SO_RCVTIMEO 防"连上不发数据"，但 WebSocket
        // 空闲是正常的（用户没碰屏幕时就没有事件）。
        // 不清掉的话，空闲几秒就会被自己的超时掐断，
        // 表现为"停一会儿不动，再拖就失灵了"。
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // ⚠️ 读超时要清掉，**发送超时不能清**。
        //
        //    读：WebSocket 空闲是正常的（用户没碰屏幕就没有事件），
        //        带读超时的话空闲几秒就被自己掐断，
        //        表现为"停一会儿不动，再拖就失灵了"。
        //
        //    写：清掉的话客户端**不读了**（进程被杀、网线拔了、
        //        单纯卡住）write 会永远阻塞 —— 而流式循环靠 write
        //        失败来发现断开、释放 FrameHub 的订阅。
        //        实测踩过：一个被杀掉的客户端留下一条 Send-Q 堆到
        //        14KB 的连接，服务端还在按 60fps 给它抓帧+编码，
        //        一直不停。
        //
        //    10 秒是宽松值：真正慢的客户端是"读得慢但一直在读"，
        //    每一帧都能写出去一部分，不会攒到 10 秒。
        timeval snd{};
        snd.tv_sec  = kSendTimeoutSec;
        snd.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

        resp.streamer(connFd);
        return;
    }

    // 流式响应没有 Content-Length（长度事先不知道），
    // 也不能带 Content-Length —— 带了客户端会等满那么多字节才渲染。
    std::string head =
            "HTTP/1.1 " + std::to_string(resp.status) + " " +
            StatusText(resp.status) + "\r\n" +
            "Content-Type: " + resp.contentType + "\r\n" +
            // 一个请求一个连接：不实现 keep-alive 就没有连接状态的复杂度
            "Connection: close\r\n" +
            "Cache-Control: no-store\r\n";
    if (!resp.isStreaming()) {
        head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    }
    for (const auto& kv : resp.extraHeaders) {
        head += kv.first + ": " + kv.second + "\r\n";
    }
    head += "\r\n";

    // 先发头再发体，避免大响应体在内存里再拼一次
    if (write(connFd, head.data(), head.size()) < 0) return;

    if (resp.isStreaming()) {
        // 交给回调。它自己判断何时停 —— 客户端断开时 write 会失败。
        //
        // 读超时不设（长连接是流式响应的正常形态），但**发送超时要设**：
        // 客户端不读了的话，write 会永远阻塞，而这个循环发现断开
        // 靠的就是 write 失败 —— 没有超时它永远发现不了，
        // FrameHub 的订阅也就永远不释放（实测踩过，见 WebSocket 那处）。
        timeval snd{};
        snd.tv_sec  = kSendTimeoutSec;
        snd.tv_usec = 0;
        setsockopt(connFd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        resp.streamer(connFd);
        return;
    }
    size_t sent = 0;
    while (sent < resp.body.size()) {
        const ssize_t n = write(connFd, resp.body.data() + sent,
                                resp.body.size() - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

// ── 便捷：构造常用的响应头 ──────────────────────────────────────────────────
HttpResponse MakeBinaryResponse(int status, const char* contentType,
                                std::string body) {
    HttpResponse r;
    r.status = status;
    r.contentType = contentType;
    r.body = std::move(body);
    return r;
}

}  // namespace remote_control
