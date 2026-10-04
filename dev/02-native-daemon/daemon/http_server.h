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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "protocol.h"

namespace remote_control {

struct HttpRequest {
    std::string method;                       // GET / POST / ...
    std::string path;                         // 已去掉 query，已 URL 解码
    std::string rawPath;                      // 解码前，便于日志
    std::vector<std::pair<std::string, std::string>> query;
    std::vector<std::pair<std::string, std::string>> headers;   // 键已转小写

    // 正文。**两个只有一个非空**：
    //   body      —— 小请求体，直接放在内存里
    //   bodyFile  —— 大请求体（超过 Options::spoolThresholdBytes）落到了
    //                这个临时文件里，读完由 HttpServer 负责删
    //
    // 为什么必须落盘：APK 动辄几百 MB，而设备总共才几 GB 内存。
    // 早先全量读进 std::string，上限只能定在 64 MB —— 结果就是
    // 稍大一点的 APK 直接 413，网页上传"莫名其妙传不上去"。
    std::string body;
    std::string bodyFile;

    // 正文长度（不管在内存还是文件里）
    size_t bodySize = 0;
    // HTTP 层为文件上传正文预留的总磁盘预算，在 handler 完成后释放。
    uint64_t bodyAdmissionDiskBytes = 0;

    // 取正文内容。bodyFile 非空时把文件读进来（调用方自己别对
    // 大文件用它 —— 那就白落盘了）。
    bool ReadBody(std::string* out, std::string* error) const;

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
        // 请求体上限。现在大体会**落盘**而不是进内存，所以可以给得很宽
        // —— 真正的约束是磁盘，不是内存。
        // ⚠️ 但非落盘的那条路仍然全量进内存，所以这个值不能当成
        //    "随便多大都行"：超过 spoolThresholdBytes 的会自动走落盘。
        // 和安装路径共用同一个常量，见 protocol.h —— 两处分开写会漂移。
        size_t      maxBodyBytes = kMaxHttpUploadBytes;

        // 超过这个大小就落盘，不留在内存里。
        // 4MB 是个折中：小于它的请求（JSON、剪贴板文本）走内存更快，
        // 大于它的（APK、文件上传）走磁盘不会被 OOM 干掉。
        size_t      spoolThresholdBytes = 4u << 20;   // 4MB

        // 上传正文 spool 与目标临时文件同时存在，故为两者预留空间。
        size_t      maxFileUploadBytes = kMaxHttpUploadBytes;
        uint64_t    maxInFlightFileUploadDiskBytes = 2 * kMaxUploadBytes;

        // 落盘目录。空 = 优先 /data/misc/remote-control，不可用时回退到
        // /data/local/tmp 等开发环境目录。
        // 要选一个 **installer 能读到** 的地方（pm install 用的是
        // 它自己的权限，不是我们的）。
        std::string spoolDir;

        // 并发连接上限（每连接一个线程）
        size_t      maxConns = 128;
    };

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // token 非空但绑定地址不是回环时返回 false —— 拒绝"对外且不鉴权"的配置。
    // 与其在文档里写"请不要这样"，不如让它在启动时就失败。
    bool Start(const Options& opts, std::string* error);

    // Run 在调用线程阻塞；销毁对象前，调用方必须在 Stop 后 join 该线程。
    void Run(HttpHandler handler);
    // 停止接收请求、关闭其他连接并等待 worker 退出。handler 可调用 Stop；
    // 此时 Stop 等待其他 worker，当前 handler 返回后连接自行收尾。
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

    bool     running() const {
        std::lock_guard<std::mutex> lk(connMutex_);
        return listenFd_ >= 0;
    }
    uint16_t port() const { return port_; }
    const std::string& bindAddr() const { return bindAddr_; }

  private:
    // 从 accept 后直到连接线程退出的连接。
    //
    // ⚠️ 这是为了**关闭时不崩**：连接线程是 detached 的，主线程走到
    //    main() 结尾就把 Dispatcher（含它的操作锁）析构了，而流式响应
    //    的回调还在那些线程里跑着 —— 表现是
    //      FORTIFY: pthread_mutex_lock called on a destroyed mutex
    //    Stop() 因此要主动 shutdown 掉这些连接，并等它们真的退出。
    // 在 accept 后登记，确保未发完请求头的连接也受连接上限和 Stop 管理。
    // mutable：CheckAuth 是 const，但它要读 tokenProvider_
    mutable std::mutex      connMutex_;
    std::condition_variable connCv_;
    // 非空 = 有一条待执行的"踢掉其它连接"请求（见 RequestKickAll）
    std::string             pendingKick_;
    std::set<int>           connFds_;

    void ServeConnection(int connFd, const HttpHandler& handler);

    // 校验请求的令牌。token_ 为空时一律放行（无鉴权模式）。
    bool CheckAuth(const HttpRequest& req) const;
    // onHeaders：**头部解析完、正文还没读**时回调。
    // 返回 false 表示"到此为止"，errReply 里是要回给客户端的应答。
    //
    // 为什么要有这个钩子：鉴权必须能在**读正文之前**做。
    // 否则未授权的客户端可以让服务端先收完（并落盘）最多 4 GiB 的请求体
    // 再被拒 —— 一个不需要任何凭据的磁盘打满 + 线程占用手段。
    bool ReadRequest(int connFd, HttpRequest* out, HttpResponse* errReply,
                     const std::function<bool(const HttpRequest&)>& onHeaders);
    bool ReserveFileUploadDisk(size_t bytes, uint64_t* reservedBytes,
                               HttpResponse* error);
    void ReleaseFileUploadDisk(uint64_t bytes);

    int         listenFd_ = -1;
    uint16_t    port_ = 0;
    std::string bindAddr_;
    std::string token_;
    // 非空时优先用它 —— 让鉴权可以运行时改变
    std::function<std::string()> tokenProvider_;
    size_t      maxBody_ = 0;

    // 并发连接上限。**每连接一个线程**，没有上限的话
    // 一个客户端狂开连接就能把线程和 fd 耗光（实测没有上限）。
    size_t      maxConns_ = 128;
    size_t      spoolThreshold_ = 4u << 20;   // 超过就落盘
    std::string spoolDir_;                     // 空 = 选择服务目录或开发回退目录
    size_t      maxFileUploadBytes_ = kMaxHttpUploadBytes;
    uint64_t    maxInFlightFileUploadDiskBytes_ = 2 * kMaxUploadBytes;
    std::mutex  uploadBudgetMutex_;
    uint64_t    uploadDiskBytesInFlight_ = 0;
    std::atomic<bool> stop_{false};
};

}  // namespace remote_control
