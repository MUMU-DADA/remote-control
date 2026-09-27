// dispatch.h —— 请求分发
//
// 把协议请求翻译成 Capture / Injector 的调用。
//
// 为什么单独成文件：这样集成测试可以直接用**真实的**分发逻辑，
// 而不是在测试里复制一份。main.cpp 里的原来那份是匿名命名空间的
// 静态函数，测试无法复用。

#pragma once

#include <string>

#include "protocol.h"

namespace autod {

class Capture;
class Injector;

class Dispatcher {
  public:
    Dispatcher(Capture* capture, Injector* injector);

    // 处理一个请求。不会抛异常；失败通过 reply.status 表达。
    ReplyPacket Handle(const Request& req, int peerUid);

  private:
    ReplyPacket HandleInfo(const Request& req);
    ReplyPacket HandleCapture(const Request& req);
    ReplyPacket HandleTouch(const Request& req);

    Capture*  capture_;
    Injector* injector_;
};

}  // namespace autod
