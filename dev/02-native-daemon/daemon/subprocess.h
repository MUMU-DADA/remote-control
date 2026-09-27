// subprocess.h — 执行外部命令并取回输出
//
// 为什么需要它：应用管理（pm/am/cmd/dumpsys）和下载（curl/wget）都要跑外部
// 命令。screencap 后端里有一份类似的 fork/exec 代码，但那份只需要把 stdout
// 接进 memfd，不需要解析，也不处理超时。
//
// 安全：**永远传 argv 数组，不经过 sh -c**。
// 包名、路径、URL 都可能来自客户端，拼字符串就等于把 shell 注入送上门。
// execvp 直接执行，参数里的空格/分号/反引号都只是普通字符。

#pragma once

#include <string>
#include <vector>

namespace autod {

struct CommandResult {
    int         exitCode = -1;   // -1 表示没跑起来（exec 失败/超时被杀）
    bool        timedOut = false;
    bool        truncated = false;  // 输出超过上限被截断
    std::string out;             // stdout
    std::string err;             // stderr（末尾几行，用于报错）
};

// 执行命令并等待结束。
//
// timeoutMs <= 0 表示不设超时。
// maxOutputBytes 限制捕获量（dumpsys 动辄几 MB），超了截断并置 truncated。
//
// 返回 false 只有一种情况：fork 本身失败（系统级错误，如 EAGAIN）。
// 命令不存在 / 返回非零 / 超时，都通过 CommandResult 表达，返回 true。
bool RunCommand(const std::vector<std::string>& argv,
                int timeoutMs,
                size_t maxOutputBytes,
                CommandResult* result,
                std::string* error);

// 便捷重载：默认 10 秒超时、1 MB 输出上限
bool RunCommand(const std::vector<std::string>& argv, CommandResult* result,
                std::string* error);

// 降权到指定 uid/gid 后再 exec。
//
// 为什么需要：有些系统服务的访问控制是**按 UID 和包名一起**判定的，
// 从 root 调用反而会被拒。剪贴板就是这样 —— root+包名"android"被拒，
// 而 shell(2000)+"com.android.shell"通过（后者持有
// READ_CLIPBOARD_IN_BACKGROUND）。
//
// uid/gid 传 -1 表示保持不变。降权失败直接 _exit，不 exec ——
// 静默地以 root 跑出去会拿到比预期大得多的权限。
bool RunCommandAs(const std::vector<std::string>& argv, int uid, int gid,
                  int timeoutMs, size_t maxOutputBytes, CommandResult* result,
                  std::string* error);

// 命令是否存在（查 PATH）
bool CommandExists(const char* name);

}  // namespace autod
