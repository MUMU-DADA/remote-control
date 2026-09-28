// inject_backend.h —— 注入后端的内部接口
//
// 不对外暴露。只有 inject.cpp（共享手势逻辑）和各后端实现会包含它。
//
// 一个后端 = 一个 .cpp 文件，实现：
//   1. InjectorBackend 的纯虚函数
//   2. CreateInjectorBackend()
//
// 链接哪个后端由 Android.bp 的 srcs 决定。因为只需要一个后端，
// CreateInjectorBackend() 用强符号即可，不需要工厂注册机制。

#pragma once

#include <memory>
#include <string>

#include "inject.h"

namespace remote_control {

class InjectorBackend {
  public:
    virtual ~InjectorBackend() = default;

    // 后端名，用于日志
    virtual const char* Name() const = 0;

    // 打开后端。失败时 error 有值。
    virtual bool Open(const InjectorConfig& config, std::string* error) = 0;

    // 发出一个触控事件。
    //   action      kActionDown / kActionMove / kActionUp 等
    //   downTimeNs  当前手势起始时间戳（纳秒，CLOCK_MONOTONIC）
    //   eventTimeNs 本次事件时间戳
    virtual bool Emit(int32_t action, const TouchPoint& point,
                      int64_t downTimeNs, int64_t eventTimeNs,
                      bool async, std::string* error) = 0;

    // 释放资源。允许在 Open 失败后被调用。
    virtual void Close() {}
};

// 由选中的后端 .cpp 实现。整个程序里只能有一个定义。
std::unique_ptr<InjectorBackend> CreateInjectorBackend();

}  // namespace remote_control
