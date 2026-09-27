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

    static HttpResponse Json(int status, const std::string& json);
    static HttpResponse Text(int status, const std::string& text);
    static HttpResponse Error(int status, const std::string& message);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
  public:
    struct Options {
        std::string bindAddr = "127.0.0.1";
        uint16_t    port     = 8088;
        // 空 = 不鉴权（只允许绑回环时这么用）
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

    bool     running() const { return listenFd_ >= 0; }
    uint16_t port() const { return port_; }
    const std::string& bindAddr() const { return bindAddr_; }

  private:
    void ServeConnection(int connFd, const HttpHandler& handler);
    bool ReadRequest(int connFd, HttpRequest* out, HttpResponse* errReply);

    int         listenFd_ = -1;
    uint16_t    port_ = 0;
    std::string bindAddr_;
    std::string token_;
    size_t      maxBody_ = 0;
    bool        stop_ = false;
};

}  // namespace autod
