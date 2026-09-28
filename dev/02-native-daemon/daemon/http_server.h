// http_server.h — 把服务能力通过 HTTP/JSON 对外暴露
//
// 为什么需要它：Unix socket 是**本机**的，别的机器、别的语言、浏览器都够不着。
// 而"对外提供 api"这句话的实际含义就是"不必写一个用 C++ 的客户端才能调"。
//
// 设计上的几个决定：
//
//   1. **默认只绑 127.0.0.1。** 这是个能截图、注入触控、装应用、删文件的
//      接口 —— 绑到 0.0.0.0 等于把设备交给同网段所有人。要对外必须显式
//      指定 --http-bind，并且**必须**配 --http-token。
//
//   2. **不引入 HTTP 库。** 需要的只是 HTTP/1.1 的一个子集：
//      请求行 + 头 + Content-Length 正文。没有 chunked、没有 keep-alive
//      （一个请求一个连接，Connection: close），代码量可控。
//
//   3. **REST 只是适配器**，真正的操作还是走同一个 Dispatcher。
//      两条传输（socket / HTTP）共用一套实现，不会出现"某个功能只有
//      一种传输支持"。

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace autod {

struct HttpRequest {
    std::string method;                       // GET / POST / ...
    std::string path;                         // 已去掉 query，已 URL 解码
    std::string rawPath;                      // 解码前，便于日志
    std::vector<std::pair<std::string, std::string>> query;
    std::vector<std::pair<std::string, std::string>> headers;   // 键已转小写
    std::string body;

    std::string queryParam(const std::string& key,
                           const std::string& def = "") const;
    std::string header(const std::string& lowerKey,
                       const std::string& def = "") const;
};

struct HttpResponse {
    int         status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;

    // 流式响应。
    //
    // 非空时 body/status 只用来写响应头（通常 200 + 一个长连接的
    // Content-Type），**响应体由这个回调负责**：它拿到 connFd，
    // 自己循环写数据直到不想写了再返回。
    //
    // 为什么需要：MJPEG 这类"一直推下去"的响应没有 Content-Length，
    // 也不能先在内存里拼好 —— 那会把整个流缓冲成字符串。
    // 回调返回后连接就关闭，所以它必须自己判断何时停
    // （客户端断开时 write 会失败，那就是停止信号）。
    std::function<void(int connFd)> streamer;

    // WebSocket 升级。
    //
    // 非空时 ServeConnection 不再发普通的 HTTP 响应，而是发
    //   101 Switching Protocols + Upgrade + Connection + Sec-WebSocket-Accept
    // 然后把连接交给 streamer —— 之后这条连接上跑的就是 WebSocket 帧了。
    //
    // 复用 streamer 而不是另开一套：升级之后"连接归回调管"这件事
    // 和 MJPEG 是一样的，区别只在握手阶段。
    std::string wsAccept;

    bool isStreaming() const { return static_cast<bool>(streamer); }
    bool isWebSocket() const { return !wsAccept.empty(); }

    static HttpResponse Json(int status, const std::string& json);
    static HttpResponse Text(int status, const std::string& text);
    static HttpResponse Error(int status, const std::string& message);

    // 构造一个流式响应。contentType 里的 boundary 由调用方给全。
    static HttpResponse Stream(const std::string& contentType);

    // 构造一个 WebSocket 升级响应。accept 由 WsComputeAccept 算出。
    static HttpResponse WebSocket(const std::string& accept);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
  public:
    struct Options {
        std::string bindAddr = "127.0.0.1";
        uint16_t    port     = 8088;
        // 访问令牌。**空 = 无鉴权**，这是默认状态（首启即是）。
        // 非空时 /api/ 下的所有请求都要带令牌；
        // 网页本身（/ 和 /ui）不校验 —— 它只是个静态页面，
        // 不含任何秘密，而用户得先打开它才能输入令牌。
        std::string token;
        size_t      maxBodyBytes = 64u << 20;   // 64MB，够传 APK
    };

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // token 非空但绑定地址不是回环时返回 false —— 拒绝"对外且不鉴权"的配置。
    // 与其在文档里写"请不要这样"，不如让它在启动时就失败。
    bool Start(const Options& opts, std::string* error);

    void Run(const HttpHandler& handler);   // 阻塞
    void Stop();

    // 请求踢掉所有**其它**连接，但**不立刻执行**。
    //
    // 为什么不能立刻踢：发起请求的那条连接此刻还没收到响应 ——
    // 在 handler 里 shutdown 自己，客户端只会看到一个断掉的连接，
    // 而不是 "{"ok":true,"serving":false}"。
    //
    // 所以这里只登记，等 ServeConnection 把响应写完再执行。
    void RequestKickAll(const char* reason);

    // 执行 RequestKickAll 登记的那次踢连接（如果有）。
    //
    // 由 ServeConnection 在**响应写完、作用域退出时**调用 ——
    // 用 RAII 而不是在每个 return 前手写：那个函数有好几个提前
    // return 的分支，漏一个就会出现"关了服务但连接还在"。
    void RunPendingKick(int exceptFd);

    // 终止所有**活跃**连接，但不关监听 fd。
    //
    // 用在两个地方：
    //   - 服务被软开关关掉 —— 已经连上的客户端不该继续享受服务
    //   - 开启了鉴权 —— 之前无鉴权连进来的人不该继续免费用
    //
    // 等它们真的退出（最多 timeoutMs），返回实际等了多久。
    // exceptFd：跳过这条连接（发起请求的那条）。
    int KickAllConnections(const char* reason, int timeoutMs = 600,
                           int exceptFd = -1);

    // 令牌来源改成**回调**，这样鉴权可以热改。
    //
    // 原来 token_ 是 Start() 时定死的，改鉴权只能重启进程 ——
    // 而重启会连带断掉所有连接，那不是"开启鉴权"，那是"重启"。
    void SetTokenProvider(std::function<std::string()> fn);

    bool     running() const { return listenFd_ >= 0; }
    uint16_t port() const { return port_; }
    const std::string& bindAddr() const { return bindAddr_; }

  private:
    // 活跃连接。
    //
    // ⚠️ 这是为了**关闭时不崩**：连接线程是 detached 的，主线程走到
    //    main() 结尾就把 Dispatcher（含它的操作锁）析构了，而流式响应
    //    的回调还在那些线程里跑着 —— 表现是
    //      FORTIFY: pthread_mutex_lock called on a destroyed mutex
    //    Stop() 因此要主动 shutdown 掉这些连接，并等它们真的退出。
    // mutable：CheckAuth 是 const，但它要读 tokenProvider_
    mutable std::mutex      connMutex_;
    // 非空 = 有一条待执行的"踢掉其它连接"请求（见 RequestKickAll）
    std::string             pendingKick_;
    std::set<int>           connFds_;

    void ServeConnection(int connFd, const HttpHandler& handler);

    // 校验请求的令牌。token_ 为空时一律放行（无鉴权模式）。
    bool CheckAuth(const HttpRequest& req) const;
    bool ReadRequest(int connFd, HttpRequest* out, HttpResponse* errReply);

    int         listenFd_ = -1;
    uint16_t    port_ = 0;
    std::string bindAddr_;
    std::string token_;
    // 非空时优先用它 —— 让鉴权可以运行时改变
    std::function<std::string()> tokenProvider_;
    size_t      maxBody_ = 0;
    bool        stop_ = false;
};

}  // namespace autod
