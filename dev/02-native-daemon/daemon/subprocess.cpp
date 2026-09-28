// subprocess.cpp — 执行外部命令并取回输出

#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "remote_control_log.h"

namespace remote_control {
namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 把一个 fd 的内容追加到 out，最多再读 budget 字节
// 返回 false 表示对端已关闭（EOF）
bool DrainFd(int fd, std::string* out, size_t budget, bool* truncated) {
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (out->size() + static_cast<size_t>(n) > budget) {
                const size_t room = budget > out->size() ? budget - out->size() : 0;
                out->append(buf, room);
                *truncated = true;
                // 继续读但丢弃，直到 EOF —— 否则子进程会因为管道写满而卡住
                continue;
            }
            out->append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;                    // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;   // 还有数据
        return false;                                // 其他错误当 EOF
    }
}

}  // namespace

bool CommandExists(const char* name) {
    if (name == nullptr || *name == 0) return false;
    // 带斜杠的按路径判断，否则查 PATH
    if (strchr(name, '/') != nullptr) {
        return access(name, X_OK) == 0;
    }
    const char* path = getenv("PATH");
    if (path == nullptr) path = "/system/bin:/system/xbin:/vendor/bin";
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t colon = p.find(':', start);
        if (colon == std::string::npos) colon = p.size();
        std::string dir = p.substr(start, colon - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return true;
        }
        start = colon + 1;
    }
    return false;
}

bool RunCommandAs(const std::vector<std::string>& argv, int uid, int gid,
                  int timeoutMs, size_t maxOutputBytes, CommandResult* result,
                  std::string* error) {
    if (argv.empty()) {
        if (error) *error = "argv 为空";
        return false;
    }
    *result = CommandResult{};

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0) {
        if (error) *error = std::string("pipe: ") + strerror(errno);
        return false;
    }
    if (pipe(errPipe) != 0) {
        if (error) *error = std::string("pipe: ") + strerror(errno);
        close(outPipe[0]); close(outPipe[1]);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = std::string("fork: ") + strerror(errno);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return false;
    }

    if (pid == 0) {
        // ── 子进程 ──
        // stdin 接 /dev/null：pm install 之类的命令会读 stdin，
        // 不接的话它可能继承到 socket 连接的 fd 并把协议流吃掉。
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);

        // 降权。**失败就直接退出，绝不继续 exec** ——
        // 静默地以 root 跑出去，子进程拿到的权限比调用方以为的大得多，
        // 而调用方还以为自己已经降权了。
        if (uid >= 0) {
            if (setgid(static_cast<gid_t>(gid >= 0 ? gid : uid)) != 0 ||
                setuid(static_cast<uid_t>(uid)) != 0) {
                const char* m = "setuid/setgid failed\n";
                ssize_t ig = write(STDERR_FILENO, m, strlen(m));
                (void)ig;
                _exit(126);
            }
        }

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        execvp(cargv[0], cargv.data());
        // exec 失败：往 stderr 写一行再退出，父进程能从 err 里看到原因
        const char* msg = "exec failed\n";
        ssize_t ignored = write(STDERR_FILENO, msg, strlen(msg));
        (void)ignored;
        _exit(127);
    }

    // ── 父进程 ──
    close(outPipe[1]);
    close(errPipe[1]);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    const int64_t deadline = timeoutMs > 0 ? NowMs() + timeoutMs : 0;
    bool outOpen = true;
    bool errOpen = true;

    while (outOpen || errOpen) {
        pollfd fds[2];
        int n = 0;
        if (outOpen) fds[n++] = {outPipe[0], POLLIN, 0};
        if (errOpen) fds[n++] = {errPipe[0], POLLIN, 0};

        int waitMs = 200;
        if (deadline > 0) {
            const int64_t left = deadline - NowMs();
            if (left <= 0) {
                result->timedOut = true;
                break;
            }
            if (left < waitMs) waitMs = static_cast<int>(left);
        }

        const int pr = poll(fds, static_cast<nfds_t>(n), waitMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // pr == 0（超时）继续循环，由 deadline 判断是否真的超时

        int idx = 0;
        if (outOpen) {
            const bool more = DrainFd(outPipe[0], &result->out, maxOutputBytes,
                                      &result->truncated);
            if (!more && (fds[idx].revents & (POLLHUP | POLLIN | POLLERR))) {
                // 再读一次确认 EOF
                const bool more2 = DrainFd(outPipe[0], &result->out, maxOutputBytes,
                                           &result->truncated);
                if (!more2) outOpen = false;
            }
            ++idx;
        }
        if (errOpen) {
            const bool more = DrainFd(errPipe[0], &result->err, 64 * 1024,
                                      &result->truncated);
            if (!more && (fds[idx].revents & (POLLHUP | POLLIN | POLLERR))) {
                const bool more2 = DrainFd(errPipe[0], &result->err, 64 * 1024,
                                           &result->truncated);
                if (!more2) errOpen = false;
            }
        }
    }

    if (result->timedOut) {
        ALOGW("subprocess: %s 超时 %d ms，强杀", argv[0].c_str(), timeoutMs);
        kill(pid, SIGKILL);
    }

    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (result->timedOut) {
        result->exitCode = -1;
    } else if (WIFEXITED(status)) {
        result->exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->exitCode = -1;
        if (error) {
            *error = argv[0] + " 被信号 " + std::to_string(WTERMSIG(status)) + " 终止";
        }
    }
    return true;
}

bool RunCommand(const std::vector<std::string>& argv,
                int timeoutMs,
                size_t maxOutputBytes,
                CommandResult* result,
                std::string* error) {
    return RunCommandAs(argv, -1, -1, timeoutMs, maxOutputBytes, result, error);
}

bool RunCommand(const std::vector<std::string>& argv, CommandResult* result,
                std::string* error) {
    return RunCommandAs(argv, -1, -1, 10000, 1u << 20, result, error);
}

}  // namespace remote_control
