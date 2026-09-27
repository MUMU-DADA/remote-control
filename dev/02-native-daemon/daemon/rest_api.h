// rest_api.h — REST 适配器
//
// 把 HTTP 请求翻译成协议命令，交给同一个 Dispatcher。
//
// **不做第二套实现**：HTTP 和 Unix socket 走的是同一套后端与同一套
// 分发逻辑。这样不会出现"某个功能只有一种传输支持"，也不会出现
// 两边行为不一致（那是最难查的一类问题）。
//
// 唯一在适配层做的是格式转换：
//   - 截图 → PNG（浏览器能直接显示）或原始像素
//   - 返回的 memfd 内容 → HTTP 响应体

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "http_server.h"
#include "protocol.h"

namespace autod {

// ── 画面流的参数与状态 ──
//
// 放头文件而不是 .cpp 的匿名命名空间：成员函数签名里要用到它们。
// 只有流那条路径会碰这两个结构。
struct StreamParams {
    int         fps           = 5;
    int         maxWidth      = 720;
    int         level         = 0;      // 0 = 按格式选默认
    bool        skipUnchanged = true;
    int         codec         = 0;      // ImageFormat；用 int 免得头文件依赖
    std::string boundary      = "autodframe";
};

// 每条流各自持有一份（跳过未变化帧的判断是有状态的）
struct StreamState {
    uint64_t lastHash = 0;
    bool     haveLast = false;
    uint64_t frameNo  = 0;
    uint32_t outW     = 0;
    uint32_t outH     = 0;
    std::vector<uint8_t> scaled;
};

class Dispatcher;

class RestApi {
  public:
    explicit RestApi(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

    HttpResponse Handle(const HttpRequest& req);

  private:
    // 转发一次操作给 Dispatcher 并把结果转成 HTTP 响应。
    //
    // binaryReply=false 时把 fd 内容当 JSON 原样返回；
    // true 时按 binaryContentType 返回（截图用）。
    HttpResponse Call(Cmd cmd, const std::string& payload, uint32_t flags,
                      int reqFd, bool binaryReply = false,
                      const char* binaryContentType = "application/octet-stream");

    // 截图：把返回的帧编成 PNG / PPM / 原始像素
    HttpResponse HandleCapture(const HttpRequest& req);

    // 安装：请求体就是 APK 字节，落成临时文件后用 fd 送过去
    HttpResponse HandleInstall(const HttpRequest& req);

    // 画面流。同一个端点两种传输：
    //   有 Upgrade 头 → WebSocket（二进制帧，控制台用这条）
    //   没有          → MJPEG（<img src> 就能看，零 JS）
    HttpResponse HandleStream(const HttpRequest& req);
    HttpResponse HandleStreamWs(const HttpRequest& req, const StreamParams& p);

    // 取一帧并编码。两条传输共用 —— 各写一遍的话，
    // "跳过未变化的帧"这类优化很容易只做在一条上。
    // 返回空表示这帧不用发，*unchanged 区分"画面没变"和"出错"。
    std::string NextEncodedFrame(const StreamParams& p, StreamState* st,
                                 bool* unchanged);

    // 手势类：x,y 走请求头的字段（和 tap/swipe 一致）
    HttpResponse HandleGesture(const HttpRequest& req, Cmd cmd, bool needsEnd);

    // 按键 / 剪贴板
    HttpResponse HandleKey(const HttpRequest& req);
    HttpResponse HandleClipboard(const HttpRequest& req);

    // 流式触控（WebSocket）。
    //
    // 一个手势一个 HTTP 请求是行不通的：每次都要 TCP 往返 + HTTP 解析，
    // 拖拽时"一顿一顿"。WebSocket 建一次连接，之后每个触控点就是一个
    // 几字节的帧，而且服务端能实时把错误推回来。
    HttpResponse HandleTouchStream(const HttpRequest& req);

    // 处理一条触控事件（JSON）。reply 非空时应当回给客户端。
    // 返回 false 表示这次事件失败。
    bool HandleTouchEvent(const std::string& text, std::string* reply);

    Dispatcher* dispatcher_;
};

}  // namespace autod
