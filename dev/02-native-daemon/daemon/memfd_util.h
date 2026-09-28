// memfd_util.h — memfd 创建，绕开 bionic 的版本限制
//
// 为什么不用 memfd_create()：
//
//   bionic 从 **API 30** 才导出这个包装函数。而 memfd_create 这个
//   **系统调用** Linux 3.17+ 就有了 —— Android 8 的内核是 4.4。
//
//   所以用 libc 包装的后果是：明明系统支持，却因为"函数不存在"
//   而编不过、跑不了。这不是能力问题，是包装层的问题。
//
//   直接发系统调用就绕过去了，而且**两条路是同一个系统调用** ——
//   实测差异 29 ns/次（memfd 每帧创建一次，可忽略）。
//
// ⚠️ 不要改回 memfd_create()。看起来更干净，但会把下限抬到 Android 11。

#pragma once

#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace remote_control {

// 创建一个匿名内存文件，返回 fd（失败返回 -1，errno 已设置）。
//
// 名字只用于调试（显示在 /proc/<pid>/fd 的目标里），不影响行为。
inline int MakeMemfd(const char* name) {
#if defined(SYS_memfd_create)
    // MFD_CLOEXEC：这个 fd 是给同进程用的，不该泄漏给子进程
    // （exec screencap / pm install 时会 fork，泄漏过去就是隐患）。
    return static_cast<int>(
            syscall(SYS_memfd_create, name, static_cast<unsigned>(MFD_CLOEXEC)));
#else
    // 理论上不会有：Android/Linux 都有这个系统调用号。
    // 编不过总比静默降级成一个不安全的实现好。
#error "这个平台没有 SYS_memfd_create，需要另找方案"
#endif
}

}  // namespace remote_control
