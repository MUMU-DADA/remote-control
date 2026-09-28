// websocket.h — 最小可用的 WebSocket 服务端（RFC 6455 子集）
//
// 为什么需要它：
//
//   流式触控不能走「一个手势一个 HTTP 请求」。原因有两层：
//
//   1. **延迟**。每次 POST 都要 TCP 往返 + HTTP 头解析 + 分发，
//      拖拽时每个移动点都这么来一遍，手感必然是"一顿一顿的"。
//      WebSocket 建一次连接，之后每个事件就是一个几字节的帧。
//
//   2. **方向**。HTTP 是客户端发起、服务端应答。而流式触控需要
//      "按下 → 一直发移动 → 抬起"这样一条**持续的事件流**，
//      服务端还要能实时把结果推回来（比如出错、设备忙）。
//
// 只实现需要的部分：文本帧、二进制帧、ping/pong、close。
// 不做扩展（permessage-deflate）、不做分片续帧 —— 我们的消息都很小，
// 而且**明确拒绝**比半懂不懂地解析安全得多（和 HTTP 那边一样的原则）。

#pragma once

#include <cstdint>
#include <string>

namespace remote_control {

// WebSocket 帧的操作码
enum WsOpcode : uint8_t {
    kWsContinuation = 0x0,
    kWsText         = 0x1,
    kWsBinary       = 0x2,
    kWsClose        = 0x8,
    kWsPing         = 0x9,
    kWsPong         = 0xA,
};

struct WsFrame {
    uint8_t     opcode = 0;
    std::string payload;
    bool        fin = true;
};

// 计算 Sec-WebSocket-Accept。
//
// 算法：base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
// 那个 GUID 是 RFC 6455 写死的，不是随便一个常量。
//
// key 为空或不是合法的 base64(16 字节) 时返回 false。
bool WsComputeAccept(const std::string& key, std::string* acceptOut);

// 从 fd 读一帧。阻塞。
//
// 返回：
//   true  → 读到一帧，*out 有效
//   false → 连接结束或出错，*error 说明原因（对端正常关闭时 error 为空）
//
// 会自动回应 ping（发 pong）。
bool WsReadFrame(int fd, WsFrame* out, std::string* error);

// 写一帧。服务端发出的帧**不打掩码**（RFC 6455 规定）。
bool WsWriteFrame(int fd, uint8_t opcode, const std::string& payload);

// 便捷：写一个文本帧
bool WsWriteText(int fd, const std::string& text);

// sha1 与 base64 单独暴露出来是为了能写主机侧测试 ——
// 握手算错的表现是"浏览器直接拒绝连接"，没有任何可读的错误信息，
// 必须能离线验证。
std::string Sha1(const std::string& data);      // 返回 20 字节原始摘要
std::string Base64Encode(const std::string& data);
bool        Base64Decode(const std::string& in, std::string* out);

}  // namespace remote_control
