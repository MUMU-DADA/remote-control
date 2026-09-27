// thread_util.h — 不用异常的线程创建
//
// 为什么不用 std::thread：
//   AOSP 默认 **-fno-exceptions**，而 std::thread 在创建失败（资源不足、
//   线程数到顶）时是**抛 std::system_error**。在 -fno-exceptions 下
//   抛出去就是 std::terminate —— 整个服务直接死。
//   实测症状：AOSP 构建直接编不过（"cannot use 'try' with exceptions
//   disabled"），因为我写了 try/catch 兜它。
//
//   pthread_create 不抛异常，它返回错误码。这才是这个项目该用的东西 ——
//   全程不用异常是既定约束（见 json_parser.h）。

#pragma once

#include <pthread.h>

#include <new>
#include <utility>

namespace autod {

namespace detail {

// 线程入口：把堆上的可调用对象跑掉再删掉自己
template <typename F>
void* Trampoline(void* p) {
    F* task = static_cast<F*>(p);
    (*task)();
    delete task;
    return nullptr;
}

}  // namespace

// 起一个 detached 线程。
//
// 返回 false 表示**线程没起来**（调用方应当有退路 —— 比如退回串行处理，
// 而不是把这次连接丢掉）。这一点比 std::thread 好：后者要么成功，
// 要么把进程带走。
template <typename F>
bool SpawnDetached(F&& fn) {
    using Fn = typename std::decay<F>::type;
    Fn* task = new (std::nothrow) Fn(std::forward<F>(fn));
    if (task == nullptr) return false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t tid;
    const int rc = pthread_create(&tid, &attr, &detail::Trampoline<Fn>, task);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        delete task;
        return false;
    }
    return true;
}

}  // namespace autod
