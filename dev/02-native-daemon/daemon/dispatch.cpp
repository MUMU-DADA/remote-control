// dispatch.cpp —— 请求分发实现
//
// 从 main.cpp 抽出来的。逻辑不变，但现在是可测试的。

#include "dispatch.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <memory>

#include "appops.h"
#include "fileops.h"
#include "json_writer.h"

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

// 把 JSON 放进 memfd，随应答一起用 SCM_RIGHTS 发回去。
//
// 为什么走 fd 而不是塞进 Reply 结构体：Reply 是定长的 40 字节，
// 而应用列表这类结果可能几十 KB，SEQPACKET 单条消息装不下（约 208KB 上限），
// 走 fd 既没有大小限制，也复用了截图已经在用的那套通道。
ReplyPacket MakeJsonReply(uint32_t cmd, const std::string& json) {
    ReplyPacket packet;
    packet.reply = MakeReply(kOk, cmd);
    packet.reply.dataSize = json.size();
    packet.reply.format   = 0;

    if (json.empty()) return packet;

    const int fd = memfd_create("autod-json", MFD_CLOEXEC);
    if (fd < 0) {
        packet.reply.status = kErrInternal;
        ALOGE("memfd_create 失败: %s", strerror(errno));
        return packet;
    }
    if (ftruncate(fd, static_cast<off_t>(json.size())) != 0) {
        close(fd);
        packet.reply.status = kErrInternal;
        return packet;
    }
    void* base = mmap(nullptr, json.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        packet.reply.status = kErrInternal;
        return packet;
    }
    memcpy(base, json.data(), json.size());
    munmap(base, json.size());

    packet.fd = fd;
    return packet;
}

// 失败应答：JSON 里带上人类可读的原因，客户端不用猜错误码的含义
ReplyPacket MakeJsonError(uint32_t cmd, uint32_t status, const std::string& message) {
    json::Writer w;
    w.Obj().Field("ok", false)
           .Field("status", static_cast<uint64_t>(status))
           .Field("error", message)
     .EndObj();
    ReplyPacket p = MakeJsonReply(cmd, w.str());
    p.reply.status = status;
    return p;
}

// 把 fd 里的内容落到临时文件，供 pm install 使用。
//
// pm install 只接受文件路径，不能从 stdin 读，所以要中转一次。
// 顺序读而不是按 st_size —— 客户端可能用管道而不是 memfd。
bool SpillFdToTempFile(int fd, const std::string& path, int64_t maxBytes,
                       int64_t* written, std::string* error) {
    const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        if (error) *error = std::string("创建临时文件失败: ") + strerror(errno);
        return false;
    }
    // 从头读，不依赖调用方的偏移
    lseek(fd, 0, SEEK_SET);

    std::vector<char> buf(128 * 1024);
    int64_t total = 0;
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            close(out);
            unlink(path.c_str());
            if (error) *error = std::string("读取 fd 失败: ") + strerror(errno);
            return false;
        }
        if (maxBytes > 0 && total + n > maxBytes) {
            close(out);
            unlink(path.c_str());
            if (error) *error = "内容超过上限 " + std::to_string(maxBytes) + " 字节";
            return false;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = write(out, buf.data() + off, static_cast<size_t>(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(out);
                unlink(path.c_str());
                if (error) *error = std::string("写临时文件失败: ") + strerror(errno);
                return false;
            }
            off += w;
        }
        total += n;
    }
    close(out);
    if (written) *written = total;
    return true;
}

// 找一个可写的临时目录
std::string TempDir() {
    const char* candidates[] = {"/data/local/tmp", "/data/tmp", "/tmp", nullptr};
    for (int i = 0; candidates[i] != nullptr; ++i) {
        struct stat st{};
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode) &&
            access(candidates[i], W_OK) == 0) {
            return candidates[i];
        }
    }
    return {};
}

}  // namespace

Dispatcher::Dispatcher(Capture* capture, Injector* injector)
      : capture_(capture), injector_(injector) {
    appOps_  = std::make_unique<AppOps>();
    fileOps_ = std::make_unique<FileOps>();
}

Dispatcher::~Dispatcher() = default;

// payload 是 NUL 分隔的字符串；空 payload 返回空 vector（而不是含空串的一项）
std::vector<std::string> Dispatcher::SplitPayload(const std::string& payload) {
    std::vector<std::string> out;
    if (payload.empty()) return out;
    size_t start = 0;
    while (start <= payload.size()) {
        size_t nul = payload.find('\0', start);
        if (nul == std::string::npos) nul = payload.size();
        out.push_back(payload.substr(start, nul - start));
        if (nul == payload.size()) break;
        start = nul + 1;
        if (start == payload.size()) break;   // 末尾的 NUL 不产生空参数
    }
    return out;
}

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

ReplyPacket Dispatcher::Handle(const Request& req, const std::string& payload,
                               int reqFd, int peerUid) {
    const std::vector<std::string> args = SplitPayload(payload);

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

        case Cmd::ListApps:
            return HandleListApps(req);
        case Cmd::AppInfo:
            return HandleAppInfo(req, args);
        case Cmd::LaunchApp:
            return HandleLaunchApp(req, args);
        case Cmd::KillApp:
            return HandleKillApp(req, args);
        case Cmd::ForegroundApp:
            return HandleForegroundApp(req);
        case Cmd::InstallApp:
            return HandleInstallApp(req, reqFd);
        case Cmd::Download:
            return HandleDownload(req, args);
        case Cmd::FileOp:
            return HandleFileOp(req, args);

        default:
            return MakeJsonError(req.cmd, kErrBadCmd,
                                 "未知命令 " + std::to_string(req.cmd));
    }
}

// ── v2：应用管理 ────────────────────────────────────────────────────────────

ReplyPacket Dispatcher::HandleListApps(const Request& req) {
    const bool includeSystem = (req.flags & kFlagIncludeSystem) != 0;
    const bool withMetadata  = (req.flags & kFlagWithMetadata) != 0;

    std::string error;
    if (!appOps_->Init(&error)) {
        return MakeJsonError(req.cmd, kErrUnsupported, error);
    }

    std::vector<AppEntry> apps;
    if (!appOps_->ListApps(includeSystem, withMetadata, &apps, &error)) {
        return MakeJsonError(req.cmd, kErrInternal, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("count", static_cast<uint64_t>(apps.size()))
                            .Field("includeSystem", includeSystem)
                            .Field("withMetadata", withMetadata)
        .Key("apps").Arr();
    for (const auto& a : apps) {
        w.Obj().Field("package", a.package);
        if (withMetadata) {
            if (!a.apkPath.empty())   w.Field("apkPath", a.apkPath);
            if (!a.installer.empty()) w.Field("installer", a.installer);
            if (a.versionCode != 0)   w.Field("versionCode", a.versionCode);
            w.Field("system", a.system);
        }
        w.EndObj();
    }
    w.EndArr().EndObj();

    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleAppInfo(const Request& req,
                                      const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "缺少包名参数（payload 应为 <package>）");
    }
    const std::string& pkg = args[0];

    std::string error;
    if (!appOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    AppDetail d;
    if (!appOps_->Detail(pkg, &d, &error)) {
        const uint32_t st = error.find("不存在") != std::string::npos ? kErrNotFound
                                                                     : kErrInternal;
        return MakeJsonError(req.cmd, st, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("package", d.package)
                            .Field("versionName", d.versionName)
                            .Field("versionCode", d.versionCode)
                            .Field("uid", d.uid)
                            .Field("minSdk", d.minSdk)
                            .Field("targetSdk", d.targetSdk)
                            .Field("apkPath", d.apkPath)
                            .Field("dataDir", d.dataDir)
                            .Field("system", d.system)
                            .Field("enabled", d.enabled)
                            .Field("firstInstallTime", d.firstInstallTime)
                            .Field("lastUpdateTime", d.lastUpdateTime)
                            .Field("signatureDigest", d.signatureDigest);
    if (!d.installer.empty()) w.Field("installer", d.installer);

    auto strArray = [&](const char* key, const std::vector<std::string>& v) {
        w.Key(key).Arr();
        for (const auto& x : v) w.Val(x);
        w.EndArr();
    };
    strArray("permissions", d.permissions);
    strArray("activities", d.activities);
    strArray("services", d.services);
    strArray("receivers", d.receivers);
    strArray("providers", d.providers);

    w.EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleLaunchApp(const Request& req,
                                        const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload, "缺少包名参数");
    }
    const std::string activity = args.size() > 1 ? args[1] : std::string();

    std::string error;
    if (!appOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    std::string component;
    if (!appOps_->Launch(args[0], activity, &component, &error)) {
        return MakeJsonError(req.cmd, kErrInternal, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("package", args[0])
                            .Field("component", component)
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleKillApp(const Request& req,
                                      const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload, "缺少包名参数");
    }
    std::string error;
    if (!appOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    if (!appOps_->Kill(args[0], &error)) {
        return MakeJsonError(req.cmd, kErrInternal, error);
    }

    // 确认真的停了 —— force-stop 是异步的，立刻查可能还在
    int32_t pid = -1;
    bool stillRunning = appOps_->IsRunning(args[0], &pid);
    if (stillRunning) {
        usleep(500 * 1000);
        stillRunning = appOps_->IsRunning(args[0], &pid);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("package", args[0])
                            .Field("stillRunning", stillRunning);
    if (stillRunning) w.Field("pid", pid);
    w.EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleForegroundApp(const Request& req) {
    std::string error;
    if (!appOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    ForegroundInfo fg;
    if (!appOps_->Foreground(&fg, &error)) {
        return MakeJsonError(req.cmd, kErrNotFound, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("package", fg.package)
                            .Field("activity", fg.activity)
                            .Field("pid", fg.pid)
                            .Field("userId", fg.userId)
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleInstallApp(const Request& req, int reqFd) {
    if (reqFd < 0) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "缺少 APK：本命令要求通过 SCM_RIGHTS 传一个 fd");
    }
    std::string error;
    if (!appOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    const std::string dir = TempDir();
    if (dir.empty()) {
        return MakeJsonError(req.cmd, kErrIo, "找不到可写的临时目录");
    }
    char nameBuf[128];
    snprintf(nameBuf, sizeof(nameBuf), "/autod-install-%d-%ld.apk", getpid(),
             static_cast<long>(time(nullptr)));
    const std::string tmp = dir + nameBuf;

    constexpr int64_t kMaxApk = 2LL << 30;   // 2 GB
    int64_t written = 0;
    if (!SpillFdToTempFile(reqFd, tmp, kMaxApk, &written, &error)) {
        return MakeJsonError(req.cmd, kErrIo, error);
    }
    if (written == 0) {
        unlink(tmp.c_str());
        return MakeJsonError(req.cmd, kErrPayload, "APK 内容为空");
    }

    const bool replace = (req.flags & kFlagReplace) != 0;
    std::string installedPkg;
    const bool ok = appOps_->Install(tmp, replace, &installedPkg, &error);

    unlink(tmp.c_str());   // 不管成败都清掉

    if (!ok) {
        return MakeJsonError(req.cmd, kErrInternal, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("bytes", written).Field("replace", replace);
    if (!installedPkg.empty()) w.Field("package", installedPkg);
    w.EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

// ── v2：下载与文件 ──────────────────────────────────────────────────────────

ReplyPacket Dispatcher::HandleDownload(const Request& req,
                                       const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload, "缺少 url 参数");
    }
    const std::string url      = args[0];
    const std::string filename = args.size() > 1 ? args[1] : std::string();
    const std::string subdir   = args.size() > 2 ? args[2] : std::string();

    std::string error;
    if (!fileOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    std::string savedPath;
    int64_t bytes = 0;
    if (!fileOps_->Download(url, filename, subdir, req.durationMs > 0 ? 0 : 0,
                            0, &savedPath, &bytes, &error)) {
        const uint32_t st = !fileOps_->httpAvailable() ? kErrUnsupported : kErrIo;
        return MakeJsonError(req.cmd, st, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true)
           .Field("path", savedPath)
           .Field("absPath", fileOps_->downloadDir() + "/" + savedPath)
           .Field("bytes", bytes)
           .Field("url", url)
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleFileOp(const Request& req,
                                     const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "缺少 op 参数（list|stat|mkdir|delete|rename|exists）");
    }
    const std::string op   = args[0];
    const std::string path = args.size() > 1 ? args[1] : std::string();
    const std::string arg2 = args.size() > 2 ? args[2] : std::string();

    std::string error;
    if (!fileOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    json::Writer w;

    if (op == "list") {
        std::vector<FileEntry> entries;
        if (!fileOps_->List(path, &entries, &error)) {
            return MakeJsonError(req.cmd, kErrNotFound, error);
        }
        w.Obj().Field("ok", true).Field("dir", path.empty() ? "." : path)
                                .Field("count", static_cast<uint64_t>(entries.size()))
            .Key("entries").Arr();
        for (const auto& e : entries) {
            w.Obj().Field("name", e.name).Field("path", e.path)
                                    .Field("dir", e.isDir)
                                    .Field("size", e.size)
                                    .Field("mtime", e.mtime)
             .EndObj();
        }
        w.EndArr().EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "stat" || op == "exists") {
        FileEntry e;
        const bool found = fileOps_->Stat(path, &e, &error);
        if (op == "exists") {
            w.Obj().Field("ok", true).Field("exists", found)
                                    .Field("path", path)
             .EndObj();
            return MakeJsonReply(req.cmd, w.str());
        }
        if (!found) return MakeJsonError(req.cmd, kErrNotFound, error);
        w.Obj().Field("ok", true).Field("name", e.name).Field("path", e.path)
                                .Field("dir", e.isDir).Field("size", e.size)
                                .Field("mtime", e.mtime)
         .EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "mkdir") {
        const bool parents = (req.flags & kFlagRecursive) != 0;
        if (!fileOps_->Mkdir(path, parents, &error)) {
            return MakeJsonError(req.cmd, kErrIo, error);
        }
        w.Obj().Field("ok", true).Field("path", path).EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "delete") {
        const bool recursive = (req.flags & kFlagRecursive) != 0;
        if (!fileOps_->Delete(path, recursive, &error)) {
            return MakeJsonError(req.cmd, kErrIo, error);
        }
        w.Obj().Field("ok", true).Field("path", path).Field("recursive", recursive)
         .EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "rename") {
        if (arg2.empty()) {
            return MakeJsonError(req.cmd, kErrPayload, "rename 需要 <from> 和 <to>");
        }
        if (!fileOps_->Rename(path, arg2, &error)) {
            return MakeJsonError(req.cmd, kErrIo, error);
        }
        w.Obj().Field("ok", true).Field("from", path).Field("to", arg2).EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    return MakeJsonError(req.cmd, kErrBadArg, "未知 op: " + op);
}

}  // namespace autod
