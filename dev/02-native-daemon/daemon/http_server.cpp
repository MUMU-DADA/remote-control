// http_server.cpp — HTTP/JSON 传输层

#include "http_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "thread_util.h"

#include "autod_log.h"
#include "json_writer.h"

namespace autod {
namespace {

constexpr int kReadTimeoutSec = 30;
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

HttpResponse HttpResponse::Error(int status, const std::string& message) {
    json::Writer w;
    w.Obj().Field("ok", false).Field("status", status)
           .Field("error", message).EndObj();
    return Json(status, w.str());
}

// ── HttpServer ──────────────────────────────────────────────────────────────
HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(const Options& opts, std::string* error) {
    bindAddr_ = opts.bindAddr;
    port_     = opts.port;
    token_    = opts.token;
    maxBody_  = opts.maxBodyBytes;

    const bool loopbackOnly = (opts.bindAddr == "127.0.0.1" ||
                               opts.bindAddr == "::1" ||
                               opts.bindAddr == "localhost");
    if (!loopbackOnly && opts.token.empty()) {
        // 与其在文档里写"请不要这样"，不如让它在启动时就失败。
        // 这是个能截图、注入触控、装应用、删文件的接口。
        if (error) {
            *error = "拒绝启动：绑定到 " + opts.bindAddr +
                     " 却不设 token —— 这会把设备控制权交给整个网络。"
                     "请用 --http-token 设置令牌，或改回 --http-bind 127.0.0.1";
        }
        return false;
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
          token_.empty() ? "无鉴权，仅限本机" : "需要 Bearer token");
    return true;
}

void HttpServer::Stop() {
    stop_ = true;
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

bool HttpServer::ReadRequest(int connFd, HttpRequest* out, HttpResponse* errReply) {
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

    while (body.size() < contentLength) {
        const ssize_t n = read(connFd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            *errReply = HttpResponse::Error(400, "读请求体失败");
            return false;
        }
        if (n == 0) {
            *errReply = HttpResponse::Error(400, "请求体不完整");
            return false;
        }
        body.append(tmp, static_cast<size_t>(n));
    }
    body.resize(contentLength);
    out->body = std::move(body);

    // 鉴权
    if (!token_.empty()) {
        std::string got = out->header("x-autod-token");
        if (got.empty()) {
            const std::string auth = out->header("authorization");
            const std::string prefix = "Bearer ";
            if (auth.compare(0, prefix.size(), prefix) == 0) {
                got = auth.substr(prefix.size());
            }
        }
        if (got != token_) {
            *errReply = HttpResponse::Error(401, "缺少或错误的 token");
            return false;
        }
    }

    return true;
}

void HttpServer::ServeConnection(int connFd, const HttpHandler& handler) {
    HttpRequest req;
    HttpResponse errReply;
    if (!ReadRequest(connFd, &req, &errReply)) {
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

    // 直接调用，不包 try/catch。
    //
    // AOSP 是 -fno-exceptions，写了也编不过；而且我们全程不用异常，
    // 处理器本来就不抛。真出了 bad_alloc 这类，-fno-exceptions 下
    // 本来就是 abort，catch 也救不回来。
    const HttpResponse resp = handler(req);

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
        setsockopt(connFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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
        // 这里不做超时：长连接是流式响应的正常形态，
        // 真正要防的"客户端不发数据就占着"在 ReadRequest 那边已经用
        // SO_RCVTIMEO 挡掉了。
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

}  // namespace autod
