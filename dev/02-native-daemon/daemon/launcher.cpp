// launcher.cpp — remote-control 的"壳"
//
// =============================================================================
// 为什么需要它
// =============================================================================
//
// init **不重读 rc**（property_service.cpp 只暴露 ctl.start/stop/restart 与
// sys.powerctl，没有 reload），而模拟器的 /system 又没有持久写入通道 ——
// 所以"服务起来之后还能换二进制"必须靠一层间接：
//
//     /system/bin/remote-control-launch   ← 壳：几十行，几乎永不改
//     /system/etc/init/remote-control.rc  ← 一次定稿，指向壳
//             │  读指针 → 校验 sha256 → fork+exec
//             ▼
//     /data/misc/remote-control/
//         ├── current -> releases/<sha256>/remote-control   ← 原子切换的指针
//         ├── releases/<sha256>/remote-control              ← 真正在跑的载荷
//         └── remote-control.conf                           ← 配置
//
// 换版本 = push 新二进制 + 切指针 + `setprop ctl.restart remote-control`，
// 秒级生效，不重启设备、不重编镜像。见 tools/rc-update.sh。
//
// =============================================================================
// ⚠️ 为什么是 fork+exec，而不是直接 exec
// =============================================================================
//
// 直接 exec 的话 PID 不变、init 跟踪照旧 —— 但**壳就没了**，没有任何人
// 能判断"新版本到底起没起来"。坏版本的后果是：init 按 restart_period
// 无限重拉一个起不来的东西，服务彻底失联，只能到宿主上 adb root 救。
//
// 所以留一个父进程做**探活**：拉起载荷后等它就绪（载荷写 ready 文件），
// 超时或提前退出就认为这版是坏的 → 指针切回 last-good 再来一次。
//
// =============================================================================
// 就绪信号
// =============================================================================
//
// 壳通过 `--ready-file <路径>` 告诉载荷往哪写；载荷在 socket 与 HTTP
// 都就绪之后，把自己的**自哈希**写进去。
//
// ⚠️ 写入的内容必须等于期望的 sha256 —— 只判断"文件存在"的话，
//    上一版留下的残留文件会让一个根本起不来的新版被判成就绪。

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "sha256.h"

using remote_control::Sha256FileHex;

namespace {

// 载荷槽的根。init 在 post-fs-data 里建好并给 shell 属主（见 remote-control.rc）。
constexpr const char* kRoot = "/data/misc/remote-control";
constexpr const char* kCurrent = "/data/misc/remote-control/current";
constexpr const char* kLastGood = "/data/misc/remote-control/last-good";
constexpr const char* kReady = "/data/misc/remote-control/ready";
constexpr const char* kLog = "/data/misc/remote-control/launcher.log";

// 镜像里自带的那一份。**第一次开机 / 槽还是空的时候用它** ——
// 这样全新机器开箱即可用，不需要先手工推一版。
constexpr const char* kSystemPayload = "/system/bin/remote-control";

// 等载荷就绪的上限。载荷要初始化 SurfaceFlinger / uinput / HTTP，
// 冷启动实测在 1 秒级；给 20 秒是留给"设备刚开机、负载很高"的情况。
constexpr int kReadyTimeoutSec = 20;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // init 会把服务的 stderr 送进内核日志，所以两边都写
    fprintf(stderr, "[launcher] %s\n", msg);
    FILE* f = fopen(kLog, "a");
    if (f != nullptr) {
        time_t now = time(nullptr);
        struct tm tmv{};
        localtime_r(&now, &tmv);
        fprintf(f, "%02d-%02d %02d:%02d:%02d %s\n", tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
        fclose(f);
    }
}

std::string ReadFileTrim(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return "";
    char buf[512];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// 原子写：先写临时文件再 rename。同分区 rename 是原子的，
// 半截写入不会让指针指向一个不存在的东西。
bool WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    if (fclose(f) != 0 || !ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string DirName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string BaseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool FileExists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

int64_t NowMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 指针文件里存的是**相对路径**（如 releases/<sha>/remote-control），
// 这样整个槽目录搬走也还能用。期望的哈希就是槽目录名 —— 一个值，
// 一处来源，不会出现"哈希和路径写得不一致"。
struct Version {
    std::string rel;       // 相对 kRoot 的路径；空 = 用镜像里那份
    std::string path;      // 绝对路径
    std::string sha;       // 期望的 sha256；空 = 不做校验（镜像里那份）
    bool fromSystem = false;
};

Version ResolveVersion(const std::string& rel) {
    Version v;
    if (rel.empty()) {
        v.path = kSystemPayload;
        v.fromSystem = true;
        return v;
    }
    v.rel = rel;
    v.path = std::string(kRoot) + "/" + rel;
    v.sha = BaseName(DirName(rel));   // releases/<sha>/remote-control → <sha>
    return v;
}

// 拉起载荷，返回 pid；失败返回 -1。
pid_t Spawn(const Version& v, int argc, char** argv, const std::string& readyFile) {
    std::vector<std::string> args;
    args.push_back(v.path);
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    args.push_back("--ready-file");
    args.push_back(readyFile);

    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        Log("fork 失败: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(v.path.c_str(), cargv.data());
        // exec 失败：127 是约定俗成的"找不到/执行不了"
        fprintf(stderr, "[launcher] exec %s 失败: %s\n", v.path.c_str(),
                strerror(errno));
        _exit(127);
    }
    return pid;
}

// 等载荷就绪。三种结束条件：
//   ready 文件出现且内容 == 期望哈希  → 就绪
//   子进程先退出                      → 这版坏了
//   超时                              → 这版坏了
bool WaitReady(pid_t pid, const std::string& wantSha) {
    const int64_t deadline = NowMs() + kReadyTimeoutSec * 1000;
    while (NowMs() < deadline) {
        const std::string got = ReadFileTrim(kReady);
        if (!got.empty() && (wantSha.empty() || got == wantSha)) return true;

        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            Log("载荷在就绪前退出（退出码 %d）", WEXITSTATUS(status));
            return false;
        }
        if (r < 0 && errno != EINTR) {
            Log("waitpid 出错: %s", strerror(errno));
            return false;
        }
        usleep(100 * 1000);
    }
    Log("载荷 %d 秒内没有就绪", kReadyTimeoutSec);
    return false;
}

void KillAndReap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void PrintUsage(const char* argv0) {
    fprintf(stderr,
            "remote-control-launch — remote-control 的启动壳\n"
            "\n"
            "用法: %s [载荷的参数...]\n"
            "\n"
            "它按 %s 指向的版本槽拉起真正的载荷，把收到的参数原样转交，\n"
            "并额外传一个 --ready-file。载荷起不来时会回退到上一版。\n"
            "\n"
            "环境:\n"
            "  REMOTE_CONTROL_LAUNCH_DRY=1   只打印选中的版本，不真的拉起\n",
            argv0, kCurrent);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    mkdir(kRoot, 0770);

    const std::string want = ReadFileTrim(kCurrent);
    const std::string lastGood = ReadFileTrim(kLastGood);

    // REMOTE_CONTROL_LAUNCH_DRY=1：只报告选中的版本，不拉起。
    // rc-update.sh 的 list/verify 用它，免得为了看一眼就得真起一个进程。
    if (getenv("REMOTE_CONTROL_LAUNCH_DRY") != nullptr) {
        printf("current=%s\n", want.empty() ? "(镜像自带)" : want.c_str());
        printf("last-good=%s\n", lastGood.empty() ? "(无)" : lastGood.c_str());
        printf("payload=%s\n",
               ResolveVersion(want).path.c_str());
        return 0;
    }

    // 候选顺序：指针指的版本 → last-good → 镜像自带的那份。
    //
    // ⚠️ 必须是**三条**。只留两条的话，"当前版坏 + last-good 也坏"就直接
    //    放弃了，而镜像里那份明明还能跑 —— 全新设备或 /data 被清过之后
    //    正是这个组合。按顺序去重，最多试三次。
    std::vector<std::string> candidates;
    candidates.push_back(want);
    if (!lastGood.empty()) candidates.push_back(lastGood);
    candidates.push_back("");   // "" = /system/bin/remote-control
    {
        std::vector<std::string> uniq;
        for (const auto& c : candidates) {
            bool seen = false;
            for (const auto& u : uniq) {
                if (u == c) { seen = true; break; }
            }
            if (!seen) uniq.push_back(c);
        }
        candidates.swap(uniq);
    }

    for (size_t attempt = 0; attempt < candidates.size(); ++attempt) {
        const std::string rel = candidates[attempt];
        Version v = ResolveVersion(rel);

        // ── 校验 ──────────────────────────────────────────────────────────
        // 槽目录名就是期望的哈希。**必须在 exec 之前核对** ——
        // 这是"更新源放在 /data"这套方案唯一的安全依据。
        if (!v.fromSystem) {
            if (!FileExists(v.path)) {
                Log("版本槽不存在：%s", v.path.c_str());
                v.sha.clear();
                v.path.clear();
            } else {
                std::string actual;
                if (!Sha256FileHex(v.path, &actual)) {
                    Log("算不出哈希：%s", v.path.c_str());
                    v.path.clear();
                } else if (actual != v.sha) {
                    Log("哈希不符，拒绝执行 %s", v.path.c_str());
                    Log("  期望 %s", v.sha.c_str());
                    Log("  实际 %s", actual.c_str());
                    v.path.clear();
                }
            }
            if (v.path.empty()) {
                // 这版不可用 → 试下一个候选
                Log("这份载荷不可用，试下一个候选");
                continue;
            }
        }

        Log("拉起 %s（%s）", v.path.c_str(),
            v.fromSystem ? "镜像自带，不做哈希校验"
                         : ("槽 " + v.sha.substr(0, 12) + "…").c_str());

        // 清掉上一轮可能留下的就绪标记 —— 不然"文件已存在"会把
        // 一个根本起不来的新版误判成就绪。
        remove(kReady);

        const pid_t pid = Spawn(v, argc, argv, kReady);
        if (pid < 0) {
            continue;   // 试下一个候选
        }

        if (!WaitReady(pid, v.sha)) {
            KillAndReap(pid);
            Log("这版没起来，试下一个候选");
            continue;
        }

        Log("就绪（pid %d）", static_cast<int>(pid));

        // 记下"这一版是好的"。下次坏版本回退时就退到这里。
        if (!v.fromSystem && !v.rel.empty()) {
            WriteFileAtomic(kLastGood, v.rel);
        }

        // 载荷在跑。等它退出，然后**自己也退出** —— init 会重新拉壳，
        // 壳重新读指针。保活与"指针可能已经被换掉"这两件事就都自动成立了。
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (WIFSIGNALED(status)) {
            Log("载荷被信号 %d 杀死", WTERMSIG(status));
            return 128 + WTERMSIG(status);
        }
        Log("载荷退出（退出码 %d），退出让 init 重拉", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }

    Log("所有候选都没起来（试过 %zu 个），退出让 init 重拉", candidates.size());
    return 1;
}
