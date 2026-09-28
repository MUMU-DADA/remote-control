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
#include <memory>
#include <string>
#include <vector>

#include "frame_hub.h"
#include "h264_encoder.h"
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

    // ── 共享抓帧（见 frame_hub.h）──
    //
    // 订阅的生命周期 = 这个 StreamState 的生命周期 = 这条连接。
    // 第一个订阅者启动抓帧线程，最后一个离开时停掉它 ——
    // **没人在看的时候完全不抓帧**。
    std::unique_ptr<FrameHub::Sub> hubSub;

    // ── H.264（只有 format=h264 时才建）──
    //
    // H.264 是**有状态**的：SPS/PPS 只发一次、后面是帧间参考。
    // 所以它不能用 ImageEncoder（那个每次调用都是独立的），
    // 必须每个流自己持有一个实例。
    //
    // 尺寸变了要 Stop 再 Start —— 编码器一旦 configure 就不能改尺寸。
    std::unique_ptr<H264Encoder> h264;
    uint32_t h264W = 0, h264H = 0;   // 当前编码器的尺寸
    bool     needKeyFrame = false;   // 下一个输出要是关键帧
    uint64_t hubSeq = 0;          // 本连接已经消费到哪一帧
    uint64_t hubTimeouts = 0;     // 等新帧超时的次数（诊断用）
};

class Dispatcher;
class HttpServer;

class RestApi {
  public:
    explicit RestApi(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

    // 让 RestApi 能踢掉活跃连接。
    //
    // 用在两处：**关闭服务**和**开启鉴权** —— 那两种情况下，
    // 已经连上的客户端不该继续享受服务。main() 里注入。
    void SetHttpServer(HttpServer* server) { httpServer_ = server; }

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

    // 画面流的可调参数（让调用方能查到，而不是翻文档）
    HttpResponse HandleStreamParams(const HttpRequest& req);

    // 日志流（WebSocket，按序号增量推）
    HttpResponse HandleLogStream(const HttpRequest& req);

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
    // 可能为空（没启用 HTTP 时）
    HttpServer* httpServer_ = nullptr;
};

}  // namespace autod
