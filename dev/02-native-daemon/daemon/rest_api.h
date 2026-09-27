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

#include "http_server.h"
#include "protocol.h"

namespace autod {

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

    // 实时画面流（MJPEG over multipart/x-mixed-replace）。
    // 浏览器把它当"会不断更新的图"，原生支持，不需要 JS 解帧。
    HttpResponse HandleStream(const HttpRequest& req);

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
