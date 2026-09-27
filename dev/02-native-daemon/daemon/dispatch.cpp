// dispatch.cpp —— 请求分发实现
//
// 从 main.cpp 抽出来的。逻辑不变，但现在是可测试的。

#include "dispatch.h"

#include "autod_log.h"
#include "capture.h"
#include "inject.h"

namespace autod {
namespace {

Reply MakeReply(uint32_t status, uint32_t cmd) {
    Reply r{};
    r.magic  = kMagic;
    r.status = status;
    r.cmd    = cmd;
    return r;
}

// 从请求里提取一个触控点，缺省值填上
TouchPoint PointFromRequest(const Request& req) {
    TouchPoint p;
    p.id       = static_cast<int32_t>(req.pointerId);
    p.x        = req.x;
    p.y        = req.y;
    p.pressure = req.pressure > 0.0f ? req.pressure : kDefaultPressure;
    p.size     = req.size     > 0.0f ? req.size     : kDefaultSize;
    return p;
}

}  // namespace

Dispatcher::Dispatcher(Capture* capture, Injector* injector)
      : capture_(capture), injector_(injector) {}

// ---------------------------------------------------------------------------

ReplyPacket Dispatcher::HandleInfo(const Request& req) {
    ReplyPacket packet;
    packet.reply = MakeReply(kOk, req.cmd);

    std::vector<DisplayInfo> displays;
    std::string error;
    if (!capture_->ListDisplays(&displays, &error)) {
        packet.reply.status = kErrNoDisplay;
        ALOGE("ListDisplays 失败: %s", error.c_str());
        return packet;
    }
    if (displays.empty()) {
        packet.reply.status = kErrNoDisplay;
        return packet;
    }

    // Info 只回主显示的尺寸，够客户端做坐标换算
    packet.reply.width    = displays.front().width;
    packet.reply.height   = displays.front().height;
    packet.reply.stride   = displays.front().width;
    packet.reply.format   = 0;
    packet.reply.dataSize = 0;

    ALOGI("info: %zu 个显示, 主显示 %ux%u @%uHz", displays.size(),
          displays.front().width, displays.front().height,
          displays.front().refreshHz);
    return packet;
}

ReplyPacket Dispatcher::HandleCapture(const Request& req) {
    ReplyPacket packet;
    packet.reply = MakeReply(kOk, req.cmd);

    Frame frame;
    std::string error;
    if (!capture_->Grab(&frame, &error)) {
        packet.reply.status = kErrCaptured;
        ALOGE("截图失败: %s", error.c_str());
        return packet;
    }

    packet.reply.width    = frame.width;
    packet.reply.height   = frame.height;
    packet.reply.stride   = frame.stride;
    packet.reply.format   = frame.format;
    packet.reply.dataSize = frame.size;
    packet.fd             = frame.fd;
    frame.fd              = -1;   // 所有权转移给 packet

    ALOGI("截图 %ux%u stride=%u format=0x%x size=%llu", frame.width,
          frame.height, frame.stride, frame.format,
          static_cast<unsigned long long>(frame.size));
    return packet;
}

ReplyPacket Dispatcher::HandleTouch(const Request& req) {
    ReplyPacket packet;
    packet.reply = MakeReply(kOk, req.cmd);

    const bool async = (req.flags & kFlagAsync) != 0;
    std::string error;
    bool ok = false;

    switch (static_cast<Cmd>(req.cmd)) {
        case Cmd::Tap: {
            const TouchPoint p = PointFromRequest(req);
            const uint32_t ms =
                    req.durationMs > 0 ? req.durationMs : kDefaultTapMs;
            ok = injector_->Tap(p, ms, async, &error);
            break;
        }
        case Cmd::Swipe: {
            TouchPoint from = PointFromRequest(req);
            TouchPoint to   = from;
            to.x = req.x2;
            to.y = req.y2;
            const uint32_t ms =
                    req.durationMs > 0 ? req.durationMs : kDefaultSwipeMs;
            ok = injector_->Swipe(from, to, ms, /*steps=*/0, async, &error);
            break;
        }
        case Cmd::TouchDown:
            ok = injector_->TouchDown(PointFromRequest(req), async, &error);
            break;
        case Cmd::TouchMove:
            ok = injector_->TouchMove(PointFromRequest(req), async, &error);
            break;
        case Cmd::TouchUp:
            ok = injector_->TouchUp(PointFromRequest(req), async, &error);
            break;
        default:
            packet.reply.status = kErrUnsupported;
            return packet;
    }

    if (!ok) {
        packet.reply.status = kErrInjected;
        ALOGE("注入失败: %s", error.c_str());
    }
    return packet;
}

// ---------------------------------------------------------------------------

ReplyPacket Dispatcher::Handle(const Request& req, int peerUid) {
    if (req.magic != kMagic) {
        ALOGE("magic 不匹配 (收到 0x%x), uid=%d", req.magic, peerUid);
        return ReplyPacket{MakeReply(kErrBadMagic, req.cmd), -1};
    }

    switch (static_cast<Cmd>(req.cmd)) {
        case Cmd::Info:
            return HandleInfo(req);
        case Cmd::Capture:
            return HandleCapture(req);
        case Cmd::Tap:
        case Cmd::Swipe:
        case Cmd::TouchDown:
        case Cmd::TouchMove:
        case Cmd::TouchUp:
            return HandleTouch(req);
        default:
            return ReplyPacket{MakeReply(kErrBadCmd, req.cmd), -1};
    }
}

}  // namespace autod
