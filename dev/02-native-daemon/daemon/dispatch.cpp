// dispatch.cpp —— 请求分发实现
//
// 从 main.cpp 抽出来的。逻辑不变，但现在是可测试的。

#include "memfd_util.h"
#include "dispatch.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <unistd.h>
#include <memory>

#include "appops.h"
#include "clipops.h"
#include "encode_pool.h"
#include "frame_hub.h"
#include "image_encoder.h"
#include "png_encoder.h"
#include "keyboard.h"
#include "log_buffer.h"
#include "selftest.h"
#include "service_state.h"
#include "subprocess.h"
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

    const int fd = MakeMemfd("autod-json");
    if (fd < 0) {
        packet.reply.status = kErrInternal;
        ALOGE("memfd 创建失败: %s", strerror(errno));
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
// 可用字节数。拿不到返回 -1。
int64_t FreeBytes(const std::string& path) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) return -1;
    return static_cast<int64_t>(vfs.f_bavail) *
           static_cast<int64_t>(vfs.f_frsize);
}

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

    // 图像编码器**在启动时初始化一次**，不要懒加载。
    //
    // 之前的写法是让各个查询点自己 Init()（HandleDescribe、Supports()…），
    // 那些 Init 会 dlopen(libz / libjpeg)，各占一个 fd。后果有两个：
    //
    //   1. 第一次 /describe 会**产生副作用**（默默加载一个库、多一个 fd）。
    //      一个"报告状态"的接口不该改变状态。
    //   2. fd 泄漏检测会误报 —— 集成测试测 200 次抓帧前后 fd 数，
    //      中间的 /describe 一调就 +1，看着像漏了。
    //
    // 启动时初始化之后，那些查询点看到的是已经就绪的状态，纯只读。
    std::string encErr;
    if (!ImageEncoder::Instance().Init(&encErr)) {
        ALOGW("图像编码器不可用: %s", encErr.c_str());
    }
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

    // req.x 复用成"目标抓帧宽度"（Capture 用不到 x 字段）。
    //
    // 让 SurfaceFlinger **在合成阶段就按这个尺寸渲染**，而不是全分辨率
    // 抓下来再软件缩放 —— 后者在 1440x2960 上是 4.3M 像素，白干 4 倍。
    // 详见 capture.h 的 SetTargetWidth。
    //
    // 0 = 原始分辨率。screencap 后端会忽略它。
    capture_->SetTargetWidth(req.x > 0 ? static_cast<uint32_t>(req.x) : 0);

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

    // 只记第一次。
    //
    // 这条原本是每帧一条 —— 单次截图时无害，但画面流每秒要抓 20~30 帧，
    // 于是日志被同一行刷屏：既看不清别的东西，10KB 的落盘历史几秒就被冲光。
    // 帧尺寸这类信息在 /api/v1/config 和自检里都有，不需要每帧重复。
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        ALOGI("截图 %ux%u stride=%u format=0x%x size=%llu（后续帧不再重复记录）",
              frame.width, frame.height, frame.stride, frame.format,
              static_cast<unsigned long long>(frame.size));
    }
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
    // 所有操作串行化。理由见 dispatch.h 里的 opMutex_ ——
    // 关键是流式响应（WebSocket / MJPEG）的回调是在处理器**返回之后**
    // 才跑的，锁放调用方保护不到它们。
    std::lock_guard<std::mutex> opLock(opMutex_);

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

        case Cmd::LongPress:
        case Cmd::Drag:
        case Cmd::DoubleTap:
            return HandleGesture(req);
        case Cmd::KeyEvent:
            return HandleKeyEvent(req, args);
        case Cmd::Clipboard:
            return HandleClipboard(req, args);
        case Cmd::Rotate:
            return HandleRotate(req, args);

        case Cmd::Power:
            return HandlePower(req, args);

        case Cmd::ServiceSwitch:
            return HandleServiceSwitch(req, args);
        case Cmd::RunningApps:
            return HandleRunningApps(req);
        case Cmd::LogFile:
            return HandleLogFile(req);

        case Cmd::Describe:
            return HandleDescribe(req);
        case Cmd::GetConfig:
            return HandleGetConfig(req);
        case Cmd::SetConfig:
            return HandleSetConfig(req, args);
        case Cmd::SelfTest:
            return HandleSelfTest(req);
        case Cmd::Stats:
            return HandleStats(req);
        case Cmd::Log:
            return HandleLog(req, args);
        case Cmd::Shutdown:
            return HandleShutdown(req, /*restart=*/false);
        case Cmd::Restart:
            return HandleShutdown(req, /*restart=*/true);

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

    // 上限和 HTTP 层共用同一个常量 —— 见 protocol.h 里的说明。
    // 以前这里是写死的 2GB，而 HTTP 层放行 4GB：用户传完 3GB 的 APK
    // 才在这里被拒，报错还像是传输出了问题。
    constexpr int64_t kMaxApk = static_cast<int64_t>(kMaxUploadBytes);

    // ── 空间预检：装不下就**别落盘**，直接说清楚 ──
    //
    // 为什么必须在落盘之前判：一次失败的安装会**泄漏一份 APK 大小的空间**
    // —— `/data/app/~~xxx/.../base.apk` 被 system_server 持着不放
    // （实测：失败后 df 少 900MB，`/proc/<system_server>/fd` 里能看到
    // 那个 deleted 的 base.apk；下一次**成功**的安装才会连带清理掉）。
    //
    // 于是失败会自我强化：装不下 → 失败 → 更装不下 → 再失败。用户看到的
    // 是"重试几次之后什么都装不上了"。宁可在花掉空间之前就拒绝。
    //
    // 实测峰值是我们拿到的那份 + 落盘的副本 + pm 自己的 session 副本
    // ≈ **3 倍** APK 大小。留 256MB 余量给系统。
    {
        struct stat fst{};
        if (fstat(reqFd, &fst) == 0 && fst.st_size > 0) {
            const int64_t need = static_cast<int64_t>(fst.st_size) * 3 +
                                 256LL * 1024 * 1024;
            const int64_t freeB = FreeBytes(dir);
            if (freeB >= 0 && freeB < need) {
                char buf[320];
                snprintf(buf, sizeof(buf),
                         "空间不足（预检）：APK %.0f MB，安装峰值约需 %.0f MB，"
                         "当前可用 %.0f MB。\n"
                         "⚠️ 每次**失败的**安装会占用一份 APK 大小的空间且不释放，"
                         "直到下一次成功安装或重启 —— 这会让重试越来越装不下。"
                         "先清出空间或重启设备再试。",
                         fst.st_size / 1048576.0, need / 1048576.0,
                         freeB / 1048576.0);
                return MakeJsonError(req.cmd, kErrIo, buf);
            }
        }
    }

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
                             "缺少 op 参数"
                             "（roots|list|stat|mkdir|delete|rename|exists）");
    }
    const std::string op   = args[0];
    const std::string path = args.size() > 1 ? args[1] : std::string();
    const std::string arg2 = args.size() > 2 ? args[2] : std::string();

    std::string error;
    if (!fileOps_->Init(&error)) return MakeJsonError(req.cmd, kErrUnsupported, error);

    json::Writer w;

    // 让客户端能**问出**边界在哪，而不是靠猜或翻文档。
    // 边界变了（不同设备 /sdcard 挂在哪）客户端也能自适应。
    if (op == "roots") {
        w.Obj().Field("ok", true)
            .Field("storage", fileOps_->storageRoot())
            .Field("download", fileOps_->downloadDir())
            .Field("note", "相对路径相对 download；绝对路径在 storage 之内即可")
         .EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "list") {
        std::vector<FileEntry> entries;
        if (!fileOps_->List(path, &entries, &error)) {
            return MakeJsonError(req.cmd, kErrNotFound, error);
        }
        w.Obj().Field("ok", true).Field("dir", path.empty() ? "." : path)
                                .Field("storage", fileOps_->storageRoot())
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

// ── v3：服务自身 ────────────────────────────────────────────────────────────

namespace {

struct CommandSpec {
    const char* name;
    uint32_t    cmd;
    uint32_t    since;        // 哪个协议版本引入
    const char* params;       // 人类可读的参数说明
    const char* desc;
};

// 命令清单。
//
// 这是 API 的"自描述"—— 客户端不必靠文档猜服务端支持什么，
// 也不必用"发一个试试看报不报错"来探测。
const CommandSpec kCommands[] = {
    {"Info",          1,  1, "无",                      "查询显示参数"},
    {"Capture",       2,  1, "flags",                   "截图，应答附带 memfd"},
    {"Tap",           3,  1, "x,y,durationMs",          "单击"},
    {"Swipe",         4,  1, "x1,y1,x2,y2,durationMs",  "滑动"},
    {"TouchDown",     5,  1, "pointerId,x,y",           "多点触控：按下"},
    {"TouchMove",     6,  1, "pointerId,x,y",           "多点触控：移动"},
    {"TouchUp",       7,  1, "pointerId,x,y",           "多点触控：抬起"},
    {"KeyEvent",      8,  4, "<键名或键码>",             "按键注入（uinput 虚拟键盘）"},
    {"ListApps",     10,  2, "flags",                   "列出应用"},
    {"AppInfo",      11,  2, "<package>",               "应用详情与清单"},
    {"LaunchApp",    12,  2, "<package>[,activity]",    "启动应用"},
    {"KillApp",      13,  2, "<package>",               "强制停止应用"},
    {"ForegroundApp",14,  2, "无",                      "当前前台应用"},
    {"InstallApp",   15,  2, "fd=APK, flags",           "安装 APK"},
    {"Download",     16,  2, "<url>[,filename[,subdir]]","下载到下载目录"},
    {"FileOp",       17,  2, "<op>[,path[,arg]]",       "下载目录文件操作"},
    {"LongPress",    30,  4, "x,y,durationMs",          "长按（按下不动再抬起）"},
    {"Drag",         31,  4, "x1,y1,x2,y2,durationMs",  "拖拽（起点停顿 + 慢速移动）"},
    {"DoubleTap",    32,  4, "x,y[,intervalMs]",        "双击"},
    {"Clipboard",    33,  4, "get|set\\0<文本>|info",    "剪贴板读写"},
    {"Power",        34,  5, "reboot|shutdown|reboot-*", "设备关机 / 重启"},
    {"ServiceSwitch",35,  6, "on|off|status",           "服务对外开关（不真停进程）"},
    {"RunningApps",  36,  6, "无",                      "正在运行的应用与进程状态"},
    {"LogFile",      37,  6, "无",                      "落盘的历史日志（最近 10KB）"},
    {"Rotate",       38,  7, "0|90|180|270|free|status","屏幕方向（有退路，见应答的 method）"},
    {"Describe",     20,  3, "无",                      "本清单：有哪些命令、哪些可用"},
    {"GetConfig",    21,  3, "无",                      "当前配置与运行时状态"},
    {"SetConfig",    22,  3, "<key>\0<value>...",       "热改配置"},
    {"SelfTest",     23,  3, "无",                      "环境自检（有副作用）"},
    {"Stats",        24,  3, "无",                      "运行统计"},
    {"Log",          25,  3, "[sinceSeq]",              "取最近日志"},
    {"Shutdown",     26,  3, "无",                      "优雅退出"},
    {"Restart",      27,  3, "无",                      "退出并由 init 重启"},
};

}  // namespace

ReplyPacket Dispatcher::HandleDescribe(const Request& req) {
    // ⚠️ 先把懒初始化的后端初始化掉，再汇总能力。
    //
    //    AppOps/FileOps 都是懒初始化的，没 Init 过时它们的可用性查询恒为 false
    //    —— 那会让 Describe 谎报"某项不可用"，而实际完全能用，是最误导人的
    //    一种错误。（实测踩过：capabilities.download 报了 false。）
    //    注意这段必须在拼 capabilities **之前**，下面循环里的按命令判定
    //    不能替代它。
    if (appOps_  != nullptr) appOps_->Init(nullptr);
    if (fileOps_ != nullptr) fileOps_->Init(nullptr);

    json::Writer w;
    w.Obj()
        .Field("service", "autod")
        .Field("protocolVersion", ServiceState::ProtocolVersion())
        .Field("pid", static_cast<int64_t>(getpid()))
        .Key("capabilities").Obj()
            .Field("screenshot", true)
            .Field("touch", true)
            .Field("multiTouch", true)
            .Field("appManagement", appOps_ != nullptr && appOps_->Init(nullptr))
            .Field("download", fileOps_ != nullptr && fileOps_->httpAvailable())
            .Field("fileManagement", fileOps_ != nullptr && fileOps_->Init(nullptr))
            // 文件管理的**边界**。不加这个，客户端只能猜 /sdcard 能不能用 ——
            // 而不同设备挂载点不同（/sdcard 只是 /storage/emulated/0 的软链接）。
            .Field("fileStorageRoot",
                  fileOps_ != nullptr ? fileOps_->storageRoot() : "")
            .Field("keyInjection", access("/dev/uinput", W_OK) == 0)
            .Field("clipboard", ClipOps::Instance().Init(nullptr))
            .Field("screenStream", PngEncoder::Instance().Available())
            // 编码能力按**这台设备实际**能用的报。
            //
            // 以前这里只有 screenStream 一个布尔，客户端看不出
            // "JPEG 有没有""WebP 有没有" —— 而 Android 8~10 上
            // WebP 确实没有（没有 AndroidBitmap_compress，
            // 设备上也没 libwebp）。只能靠试错是最糟的接口设计。
            .Key("codecs").Obj()
                .Field("png",  ImageEncoder::Instance().Supports(ImageFormat::kPng))
                .Field("jpeg", ImageEncoder::Instance().Supports(ImageFormat::kJpeg))
                .Field("webp", ImageEncoder::Instance().Supports(ImageFormat::kWebp))
                .Field("backend", ImageEncoder::Instance().BackendSummary())
                .Field("forced", ImageEncoder::Instance().FallbackForced())
            .EndObj()
            .Field("webUi", true)
            .Field("power", true)
            // 旋转**能力**要如实报：正规入口在，不代表这台设备转得动。
            // 实测有的 ROM 上 user-rotation 返回成功但 mRotation 不变。
            // 这里只报"这条命令实现了"，能不能真转见命令应答的 applied。
            .Field("rotate", true)
            .Field("rotateMethod", "user-rotation|wm-size")
            .Field("serviceSwitch", true)
            .Field("runningApps", true)
            .Field("logFile", LogBuffer::Instance().HistoryPath().empty() ? false : true)
            .Field("selfControl", true)
        .EndObj()
        .Key("commands").Arr();

    for (const auto& c : kCommands) {
        w.Obj().Field("name", c.name)
               .Field("cmd", c.cmd)
               .Field("since", c.since)
               .Field("params", c.params)
               .Field("desc", c.desc);

        // 每个命令单独判定可用性
        bool available = true;
        std::string reason;
        switch (static_cast<Cmd>(c.cmd)) {
            case Cmd::KeyEvent:
                // 按键走 uinput 虚拟键盘（延迟创建）。
                // 这里只判断能不能建 —— 真去建设备会让 Describe 产生副作用。
                available = access("/dev/uinput", W_OK) == 0;
                if (!available) {
                    reason = "/dev/uinput 不可写，无法创建虚拟键盘";
                }
                break;
            case Cmd::ListApps:
            case Cmd::AppInfo:
            case Cmd::LaunchApp:
            case Cmd::KillApp:
            case Cmd::ForegroundApp:
            case Cmd::InstallApp: {
                std::string err;
                available = appOps_ != nullptr && appOps_->Init(&err);
                if (!available) reason = err;
                break;
            }
            case Cmd::Download: {
                // ⚠️ 必须先 Init 再问 httpAvailable()。
                //    FileOps 是懒初始化的，没 Init 过时 httpAvailable() 恒为 false
                //    —— 于是 Describe 会谎报"下载不可用"，而实际完全能用。
                //    （实测就是这么暴露的：capabilities.download 报了 false。）
                std::string err;
                if (fileOps_ != nullptr) fileOps_->Init(&err);
                available = fileOps_ != nullptr && fileOps_->httpAvailable();
                if (!available) {
                    reason = err.empty()
                                 ? "设备上没有可用的 libcurl，服务端无法下载"
                                 : err;
                }
                break;
            }
            case Cmd::Clipboard: {
                std::string err;
                available = ClipOps::Instance().Init(&err);
                if (!available) reason = err;
                break;
            }
            case Cmd::FileOp: {
                std::string err;
                available = fileOps_ != nullptr && fileOps_->Init(&err);
                if (!available) reason = err;
                break;
            }
            default:
                break;
        }
        w.Field("available", available);
        if (!available) w.Field("reason", reason);
        w.EndObj();
    }
    w.EndArr().EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleGetConfig(const Request& req) {
    ServiceState& st = ServiceState::Instance();
    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("protocolVersion", ServiceState::ProtocolVersion())
        .Key("config").RawJson(st.ConfigJson())
        .Key("runtime").RawJson(st.RuntimeJson())
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleSetConfig(const Request& req,
                                        const std::vector<std::string>& args) {
    if (args.empty()) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "缺少参数（payload 应为 <key>\\0<value> 序列）");
    }
    // 参数成对出现：key, value, key, value…
    if (args.size() % 2 != 0) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "参数必须成对：<key>\\0<value>，当前收到 " +
                             std::to_string(args.size()) + " 个");
    }
    std::vector<std::pair<std::string, std::string>> kv;
    for (size_t i = 0; i + 1 < args.size(); i += 2) {
        kv.emplace_back(args[i], args[i + 1]);
    }

    ServiceState& st = ServiceState::Instance();
    const auto result = st.Apply(kv);
    return MakeJsonReply(req.cmd, st.ApplyResultJson(result));
}

ReplyPacket Dispatcher::HandleSelfTest(const Request& req) {
    ServiceState& st = ServiceState::Instance();
    const auto cfg = st.GetConfig();
    // 触控范围：优先用运行时实际生效的值（可能已被 SetConfig 改过）
    uint32_t w = cfg.touchWidth;
    uint32_t h = cfg.touchHeight;
    const InjectorConfig& ic = st.GetInjectorConfig();
    if (ic.touchWidth > 0)  w = ic.touchWidth;
    if (ic.touchHeight > 0) h = ic.touchHeight;

    return MakeJsonReply(req.cmd, RunSelfTestJson(cfg.verbose, w, h));
}

ReplyPacket Dispatcher::HandleStats(const Request& req) {
    // 顺便报共享抓帧的状态。
    //
    // 没有这段的话，"抓帧线程在不在跑""这一帧是共享来的还是自己抓的"
    // 都只能靠猜 —— 而这两件事正是这次改造的核心。
    auto extra = [](json::Writer& w) {
        FrameHub::Stats h = FrameHub::Instance().GetStats();
        EncodePool::Stats ep = EncodePool::Instance().GetStats();
        w.Key("encodePool").Obj()
             .Field("limit", static_cast<int64_t>(ep.limit))
             .Field("inUse", static_cast<int64_t>(ep.inUse))
             .Field("acquired", ep.acquired)
             // timeouts：因为拿不到名额而跳过的帧数
             .Field("timeouts", ep.timeouts)
         .EndObj();

        w.Key("frameHub").Obj()
             .Field("running", h.running)
             .Field("subscribers", static_cast<int64_t>(h.subscribers))
             .Field("frames", h.frames)
             .Field("lastSeq", h.lastSeq)
             .Field("lastCaptureMs", h.lastCaptureMs)
             // maxFps: 当前按多少 fps 在抓（所有订阅者里的最高需求）
             .Field("maxFps", static_cast<int64_t>(h.maxFps))
             // served: 取帧直接命中（抓帧线程已经备好）的次数
             // misses: 没等到新帧（超时）的次数
             //
             // 这个比值就是"卡不卡"的机器可读版本：
             // 按需触发的那版每次都要等一个抓帧周期，misses 会很高。
             .Field("served", h.served)
             .Field("misses", h.misses)
         .EndObj();
    };
    return MakeJsonReply(req.cmd, ServiceState::Instance().StatsJson(extra));
}

ReplyPacket Dispatcher::HandleLog(const Request& req,
                                  const std::vector<std::string>& args) {
    uint64_t since = 0;
    if (!args.empty() && !args[0].empty()) {
        char* end = nullptr;
        since = strtoull(args[0].c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            return MakeJsonError(req.cmd, kErrBadArg, "sinceSeq 需要是数字");
        }
    }

    constexpr size_t kMaxLines = 500;
    uint64_t latest = 0;
    const auto lines = LogBuffer::Instance().Since(since, kMaxLines, &latest);

    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("sinceSeq", since)
        .Field("latestSeq", latest)
        .Field("dropped", LogBuffer::Instance().DroppedCount())
        .Field("minLevel", LogLevelName(LogBuffer::Instance().MinLevel()))
        .Field("count", static_cast<uint64_t>(lines.size()))
        .Key("lines").Arr();
    for (const auto& ln : lines) {
        w.Obj()
            .Field("seq", ln.seq)
            .Field("timeMs", ln.timeMs)
            .Field("time", ln.timeStr)
            .Field("level", LogLevelName(ln.level))
            .Field("tag", ln.tag)
            .Field("text", ln.text)
         .EndObj();
    }
    w.EndArr().EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleShutdown(const Request& req, bool restart) {
    ServiceState::Instance().RequestShutdown(restart);

    json::Writer w;
    w.Obj().Field("ok", true)
           .Field("action", restart ? "restart" : "shutdown")
           .Field("note", restart
                    ? "进程即将退出；需要 init（autod.rc）负责拉起，否则不会自动回来"
                    : "进程即将优雅退出：关闭 uinput 设备、清理 socket 文件")
     .EndObj();
    ALOGI("收到 %s 请求，准备退出", restart ? "重启" : "关闭");
    return MakeJsonReply(req.cmd, w.str());
}

// ── v4：手势 / 按键 / 剪贴板 ─────────────────────────────────────────────────

ReplyPacket Dispatcher::HandleGesture(const Request& req) {
    ReplyPacket packet;
    packet.reply = MakeReply(kOk, req.cmd);

    const bool async = (req.flags & kFlagAsync) != 0;
    std::string error;
    bool ok = false;
    const char* name = "?";

    switch (static_cast<Cmd>(req.cmd)) {
        case Cmd::LongPress: {
            name = "longPress";
            ok = injector_->LongPress(PointFromRequest(req), req.durationMs,
                                      async, &error);
            break;
        }
        case Cmd::Drag: {
            name = "drag";
            TouchPoint from = PointFromRequest(req);
            TouchPoint to   = from;
            to.x = req.x2;
            to.y = req.y2;
            ok = injector_->Drag(from, to, req.durationMs, async, &error);
            break;
        }
        case Cmd::DoubleTap: {
            name = "doubleTap";
            ok = injector_->DoubleTap(PointFromRequest(req), req.durationMs,
                                      async, &error);
            break;
        }
        default:
            packet.reply.status = kErrUnsupported;
            return packet;
    }

    if (!ok) {
        packet.reply.status = kErrInjected;
        ALOGE("%s 失败: %s", name, error.c_str());
        return packet;
    }
    ALOGI("%s 完成 (%d,%d)", name, req.x, req.y);
    return packet;
}

ReplyPacket Dispatcher::HandleKeyEvent(const Request& req,
                                       const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) {
        return MakeJsonError(req.cmd, kErrPayload,
                             "缺少键名参数（payload 应为 <键名或键码>）");
    }

    uint32_t code = 0;
    if (!Keyboard::ResolveKeyCode(args[0], &code)) {
        return MakeJsonError(req.cmd, kErrBadArg,
                             "不认识的键: " + args[0] + "。可用: " +
                                 Keyboard::KnownKeyNames());
    }

    // 延迟初始化：键盘设备是有副作用的（会在系统里多一个输入设备），
    // 没用到就不该建。touch 那边是启动时就建，因为它总是要用。
    if (keyboard_ == nullptr) {
        keyboard_ = std::make_unique<Keyboard>();
    }
    if (!keyboard_->ready()) {
        std::string err;
        if (!keyboard_->Init(&err)) {
            return MakeJsonError(req.cmd, kErrUnsupported, err);
        }
    }

    const bool longPress = (req.flags & kFlagKeyLongPress) != 0;
    std::string error;
    if (!keyboard_->Key(code, longPress, &error)) {
        return MakeJsonError(req.cmd, kErrInjected, error);
    }

    json::Writer w;
    w.Obj().Field("ok", true).Field("key", args[0])
           .Field("keyCode", code).Field("longPress", longPress)
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleClipboard(const Request& req,
                                        const std::vector<std::string>& args) {
    const std::string op = args.empty() ? "get" : args[0];

    ClipOps& clip = ClipOps::Instance();
    std::string error;
    if (!clip.Init(&error)) {
        return MakeJsonError(req.cmd, kErrUnsupported, error);
    }

    if (op == "get" || op == "info") {
        ClipInfo info;
        bool empty = false;
        if (!clip.Get(&info, &empty, &error)) {
            return MakeJsonError(req.cmd, kErrInternal, error);
        }
        json::Writer w;
        w.Obj().Field("ok", true).Field("has", !empty);
        if (!empty && op == "get") {
            w.Field("text", info.text);
        }
        if (!empty && op == "info") {
            w.Field("text", info.text);
        }
        w.EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    if (op == "set") {
        if (args.size() < 2) {
            return MakeJsonError(req.cmd, kErrPayload,
                                 "set 需要文本参数（payload: set\\0<文本>）");
        }
        if (!clip.Set(args[1], &error)) {
            return MakeJsonError(req.cmd, kErrPermission, error);
        }
        json::Writer w;
        w.Obj().Field("ok", true).Field("length", static_cast<uint64_t>(args[1].size()))
         .EndObj();
        return MakeJsonReply(req.cmd, w.str());
    }

    return MakeJsonError(req.cmd, kErrBadArg,
                         "未知操作: " + op + "（可用 get|set|info）");
}

// ── v5：设备电源 ────────────────────────────────────────────────────────────

ReplyPacket Dispatcher::HandlePower(const Request& req,
                                    const std::vector<std::string>& args) {
    const std::string what = args.empty() ? "reboot" : args[0];

    // 走 `svc power reboot|shutdown`（= PowerManager.reboot/shutdown）。
    //
    // 为什么不用 `reboot` 二进制：svc 走的是 PowerManager，它会先做
    // 正常的关机流程（通知应用、卸载文件系统），而 `reboot` 是直接
    // 让 init 重启。产品环境要前者。
    // 后者作为退路 —— 有些精简 ROM 没有 svc。
    std::vector<std::string> argv;
    bool isShutdown = false;

    if (what == "shutdown" || what == "poweroff") {
        argv = {"/system/bin/svc", "power", "shutdown"};
        isShutdown = true;
    } else if (what == "reboot") {
        argv = {"/system/bin/svc", "power", "reboot"};
    } else if (what.compare(0, 7, "reboot-") == 0) {
        // reboot-recovery / reboot-bootloader / reboot-sideload
        argv = {"/system/bin/svc", "power", "reboot", what.substr(7)};
    } else {
        return MakeJsonError(req.cmd, kErrBadArg,
                             "未知的电源操作: " + what +
                                 "（可用 reboot|shutdown|reboot-recovery|"
                                 "reboot-bootloader|reboot-sideload）");
    }

    // 先把应答发出去再真正执行 —— 不然设备已经开始关机，
    // 客户端只会看到连接被重置，无从判断命令是否被受理。
    //
    // 做法：fork 一个子进程延迟 500ms 再执行。父进程立刻返回。
    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("action", what)
        .Field("method", "svc power")
        .Field("note", isShutdown
                           ? "设备将在约 0.5 秒后开始关机"
                           : "设备将在约 0.5 秒后重启")
     .EndObj();

    const pid_t pid = fork();
    if (pid == 0) {
        // 子进程：脱离父进程，延迟后执行
        setsid();
        usleep(500 * 1000);

        CommandResult r;
        if (!RunCommand(argv, 30000, 4096, &r, nullptr) || r.exitCode != 0) {
            // svc 不可用就退回 reboot 二进制
            std::vector<std::string> fallback;
            if (isShutdown) {
                fallback = {"/system/bin/reboot", "-p"};
            } else {
                fallback = {"/system/bin/reboot"};
                if (what.compare(0, 7, "reboot-") == 0) {
                    fallback.push_back(what.substr(7));
                }
            }
            RunCommand(fallback, 30000, 4096, &r, nullptr);
        }
        _exit(0);
    }

    ALOGW("收到电源请求: %s（子进程 pid=%d 将在 0.5s 后执行）", what.c_str(), pid);
    return MakeJsonReply(req.cmd, w.str());
}

// ── v7：屏幕方向 ────────────────────────────────────────────────────────────

namespace {

// `cmd window user-rotation` 的输出形如 "lock 0" / "free"。
bool ParseUserRotation(const std::string& s, int* rot, bool* isFree) {
    if (s.find("free") != std::string::npos) {
        *isFree = true;
        return true;
    }
    const size_t p = s.find("lock");
    if (p == std::string::npos) return false;
    for (size_t i = p + 4; i < s.size(); ++i) {
        if (s[i] >= '0' && s[i] <= '9') {
            *rot = s[i] - '0';
            *isFree = false;
            return true;
        }
    }
    return false;
}

// `cmd window size` 输出两行：
//     Physical size: 720x1280
//     Override size: 1280x720      ← 只有存在覆盖时才有这一行
//
// 取覆盖值优先 —— 那才是**当前实际生效**的尺寸。只看 Physical 的话，
// 改过尺寸之后会读到面板原始分辨率，据此算横屏尺寸就会算错。
bool ParseOneSize(const std::string& s, const char* key,
                  uint32_t* w, uint32_t* h) {
    const size_t p = s.find(key);
    if (p == std::string::npos) return false;
    size_t i = p + strlen(key);
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    uint64_t a = 0, b = 0;
    size_t n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { a = a * 10 + (s[i++] - '0'); ++n; }
    if (n == 0 || i >= s.size() || s[i] != 'x') return false;
    ++i; n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { b = b * 10 + (s[i++] - '0'); ++n; }
    if (n == 0 || a == 0 || b == 0 || a > 65535 || b > 65535) return false;
    *w = static_cast<uint32_t>(a);
    *h = static_cast<uint32_t>(b);
    return true;
}

// 从 dumpsys window 里读**真实**方向（0..3）。
//
// ⚠️ 必须读这个，不能只读 `cmd window user-rotation`。
//    后者报的是**设置项**，写什么它就读出什么 —— 永远等于我们刚设的值，
//    证明不了屏幕真的转了。实测踩过：设置项 lock 1，而 mRotation 仍是
//    ROTATION_0，于是接口报 applied=true 而画面纹丝不动。
//
// dumpsys window 在这台设备上只有 32KB，mRotation 在第 104 行，24ms。
// 返回 -1 表示读不到。
int ParseRealRotation(const std::string& s) {
    static const char kKey[] = "mRotation=ROTATION_";
    const size_t p = s.find(kKey);
    if (p == std::string::npos) return -1;
    const size_t i = p + sizeof(kKey) - 1;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1;
    return s[i] - '0';
}

bool ParseWmSize(const std::string& s, uint32_t* w, uint32_t* h) {
    if (ParseOneSize(s, "Override size:", w, h)) return true;
    return ParseOneSize(s, "Physical size:", w, h);
}

// 跑一条 `cmd window ...` 并取回 stdout。
bool RunWindowCmd(const std::vector<std::string>& tail, std::string* out,
                  std::string* error) {
    std::vector<std::string> argv = {"/system/bin/cmd", "window"};
    argv.insert(argv.end(), tail.begin(), tail.end());
    CommandResult r;
    if (!RunCommand(argv, 8000, 64 * 1024, &r, error)) return false;
    if (r.exitCode != 0) {
        if (error) {
            *error = "cmd window 退出码 " + std::to_string(r.exitCode) +
                     (r.err.empty() ? "" : ("：" + r.err));
        }
        return false;
    }
    if (out) *out = r.out;
    return true;
}

}  // namespace

ReplyPacket Dispatcher::HandleRotate(const Request& req,
                                     const std::vector<std::string>& args) {
    const std::string what = args.empty() ? "status" : args[0];

    // 每一步都以**读回的值**为准，绝不假定命令生效了。
    //
    // 这不是过度防御：实测自编的 x86_64 ROM 上 `cmd window user-rotation
    // lock 1` 返回成功、设置项也写进去了，但 mRotation 死活不动 ——
    // 那台设备根本没有旋转支持。假装成功的话，用户会看到一个
    // 转了 90° 的按钮和一张没转的画面，然后怀疑是服务坏了。
    // ⚠️ 两套尺寸，别混：
    //
    //    w/h        `wm size` 报的**逻辑**尺寸（有覆盖时读 Override size）
    //    realW/realH 抓帧拿到的**真实**显示尺寸，也就是客户端在画面上看到的
    //
    //    在面板原生横屏的模拟器上实测过：`wm size 720x1280` 只改了
    //    `mOverrideDisplayInfo`（应用可见区域），**真实 framebuffer 还是
    //    1280x720**。拿 w/h 判成败会得出"转成竖屏了"的错误结论 ——
    //    判成败、报给客户端的尺寸、重建注入器的依据，一律用 realW/realH。
    auto snapshot = [&](int* rot, bool* isFree, uint32_t* w, uint32_t* h,
                        int* actual, uint32_t* realW, uint32_t* realH) {
        std::string e;
        std::string out;
        if (RunWindowCmd({"user-rotation"}, &out, &e)) {
            ParseUserRotation(out, rot, isFree);
        }
        out.clear();
        if (RunWindowCmd({"size"}, &out, &e)) {
            ParseWmSize(out, w, h);
        }
        // 真实方向单独读一次 —— 见 ParseRealRotation 上面那段。
        CommandResult r;
        if (RunCommand({"/system/bin/dumpsys", "window"}, 8000, 256 * 1024,
                       &r, &e) && r.exitCode == 0) {
            *actual = ParseRealRotation(r.out);
        }
        std::vector<DisplayInfo> displays;
        std::string de;
        if (capture_ != nullptr && capture_->ListDisplays(&displays, &de) &&
            !displays.empty()) {
            *realW = displays.front().width;
            *realH = displays.front().height;
        }
    };

    int      rot = 0;
    bool     isFree = true;
    uint32_t w = 0, h = 0;
    int      actual = -1;
    uint32_t realW = 0, realH = 0;
    snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
    // 读不到就退回逻辑尺寸，至少不是 0
    if (realW == 0 || realH == 0) { realW = w; realH = h; }

    // 显示几何变了之后，把注入器的坐标范围跟过去。
    //
    // ⚠️ 不跟会怎样：坐标范围是 uinput 设备创建时定死的，客户端按它发坐标。
    //    转屏后显示变成 1280x720 而范围还是 720x1280 —— 客户端按屏幕像素
    //    发 x=1000，超出 0..719 被内核**钳到右边缘**，落点全错。
    //    实测过：只测屏幕正中央看不出来（两个空间在中心点重合），
    //    一测偏离中心的位置立刻露馅。
    auto syncInjector = [&]() {
        const ServiceState& st = ServiceState::Instance();
        // 用户用 --touch-range / touch-width 显式指定过就不动它
        const bool explicitRange = st.GetConfig().touchWidth > 0;
        std::string e;
        // 按**真实**尺寸重建 —— 触控空间必须和客户端看到的画面一致
        if (!ServiceState::Instance().RebuildInjectorForDisplay(realW, realH,
                                                                explicitRange,
                                                                &e)) {
            ALOGW("重建注入设备失败（触控范围可能和显示尺寸不一致）: %s",
                  e.c_str());
        }
    };

    auto reply = [&](bool applied, const std::string& method,
                     int requested, const std::string& note) {
        json::Writer jw;
        jw.Obj()
            .Field("ok", true)
            .Field("requested", static_cast<int64_t>(requested))
            .Field("applied", applied)
            .Field("method", method)
            // rotation = 设置项；actualRotation = 系统实际的 mRotation。
            // 两者不一致就说明**这台设备转不动**，applied 会是 false。
            .Field("rotation", static_cast<int64_t>(rot))
            .Field("actualRotation", static_cast<int64_t>(actual))
            .Field("free", isFree)
            // 报**真实**（抓帧拿到的）尺寸，不是 wm size 的逻辑尺寸 ——
            // 两者在面板原生横屏 + wm size 覆盖的设备上会分叉，
            // 而客户端关心的是画面到底多大。
            .Field("width", static_cast<int64_t>(realW))
            .Field("height", static_cast<int64_t>(realH))
            .Field("logicalWidth", static_cast<int64_t>(w))
            .Field("logicalHeight", static_cast<int64_t>(h))
            .Field("note", note)
         .EndObj();
        return MakeJsonReply(req.cmd, jw.str());
    };

    if (what == "status") {
        return reply(true, "none", -1,
                     isFree ? "方向未锁定（跟随传感器）" : "方向已锁定");
    }

    if (what == "free") {
        std::string e;
        if (!RunWindowCmd({"user-rotation", "free"}, nullptr, &e)) {
            return MakeJsonError(req.cmd, kErrInternal, e);
        }
        usleep(600 * 1000);
        snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
        if (realW == 0 || realH == 0) { realW = w; realH = h; }
        return reply(isFree, "user-rotation", -1,
                     isFree ? "已解除方向锁定，跟随传感器"
                            : "user-rotation free 没有生效，方向仍被锁定");
    }

    int deg = -1;
    if (what == "0")        deg = 0;
    else if (what == "90")  deg = 90;
    else if (what == "180") deg = 180;
    else if (what == "270") deg = 270;
    else if (what == "portrait")  deg = 0;
    else if (what == "landscape") deg = 90;
    if (deg < 0) {
        return MakeJsonError(req.cmd, kErrBadArg,
                             "未知方向: " + what +
                                 "（可用 0|90|180|270|portrait|landscape|"
                                 "free|status）");
    }
    const int n = deg / 90;

    // ── 第一条路：WindowManager 的正规入口 ──
    std::string err;
    if (RunWindowCmd({"user-rotation", "lock", std::to_string(n)}, nullptr,
                     &err)) {
        usleep(600 * 1000);          // 等窗口管理器把配置变更发下去
        snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
        if (realW == 0 || realH == 0) { realW = w; realH = h; }

        // 两个条件都满足才算真的切过去了：
        //   · 系统实际方向 == 目标（不是设置项 —— 见 ParseRealRotation）
        //   · 显示**几何**也对：0/180 竖直、90/270 横向
        //
        // ⚠️ 只判方向会误判：在不支持旋转的设备上，转 0° 时
        //    actualRotation 本来就是 0，于是"成功"返回，而上一轮
        //    退路设的横屏尺寸**永远不会被还原**。
        //    实测踩到：to=0 之后显示还停在 1280x720。
        const bool wantLandscape = (n == 1 || n == 3);
        const bool geomOk = wantLandscape ? (realW > realH) : (realH >= realW);
        if (actual == n && geomOk) {
            syncInjector();
            return reply(true, "user-rotation", deg,
                         "已通过 WindowManager 锁定方向并生效");
        }
    }

    // ── 第二条路：wm size 交换宽高 ──
    //
    // 有些 ROM 没有旋转支持（见上面那段）。那种设备上换显示尺寸照样
    // 能让应用重新布局成横屏 —— 已实测有效（截图里快捷面板是横向排的）。
    //
    // ⚠️ 它和真旋转**不等价**：mRotation 不变，所以按方向（而不是按尺寸）
    //    判断横竖的应用看不出来；180° 也没法表达。
    //
    // ⚠️ 而且**不是所有设备都吃这一套**。本项目横屏皮肤的模拟器上实测：
    //    `wm size 720x1280` 只写进了 `mOverrideDisplayInfo`（应用可见区域），
    //    真实 framebuffer 仍是 1280x720，抓帧完全没变 —— 面板的 base mode
    //    就是 1280x720，覆盖尺寸要求 1280 高，超出了它。
    //    所以下面判成败一律看**真实**尺寸（realW/realH），
    //    不看 wm size 的读数：后者会被这层覆盖骗过去。
    if (n == 2) {
        return reply(false, "none", deg,
                     "这台设备不支持旋转，而 180° 没法用显示尺寸模拟"
                     "（wm size 只能交换宽高，表达不出上下颠倒）");
    }

    // ── 走 wm size 这条路之前，先把旋转锁解掉 ──
    //
    // ⚠️ 两个机制会互相打架：`cmd window user-rotation lock 1` 把 mRotation
    //    设成 1（内容在面板内转 90°），而 `wm size 1280x720` 是改逻辑尺寸。
    //    同时存在时实测**逻辑尺寸被按回 720x1280**，`wm size` 那个覆盖
    //    根本不生效 —— 抓帧还是竖屏，但接口报的是横屏尺寸。
    //
    //    前面试过 user-rotation 了（没成功才走到这里），所以这里把它
    //    归零，让 wm size 独占生效。
    {
        std::string e;
        RunWindowCmd({"user-rotation", "lock", "0"}, nullptr, &e);
        usleep(200 * 1000);
    }

    if (n == 0) {
        // ⚠️ 这里原来直接 `wm size reset` 然后**无条件** applied=true。
        //
        //    只在**面板原生竖屏**的设备上是对的：reset 回物理尺寸 = 竖屏。
        //    面板原生横屏时（本项目的模拟器皮肤就是 1280x720），
        //    reset 回去**还是横屏** —— 接口报"成功"，画面纹丝不动。
        //    实测：rotate_test 转到 portrait，画布/触控一直是 1280x720，
        //    而 applied=true。这正是本函数开头那段注释里骂过的
        //    "转了 90° 的按钮配一张没转的画面"。
        //
        //    修法：和横屏那条分支对称 —— 先 reset 清掉可能存在的覆盖，
        //    几何不对再显式要一次竖屏尺寸，最后**按几何判定**成功与否。
        std::string e;
        if (!RunWindowCmd({"size", "reset"}, nullptr, &e)) {
            return MakeJsonError(req.cmd, kErrInternal, e);
        }
        usleep(400 * 1000);
        snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
        if (realW == 0 || realH == 0) { realW = w; realH = h; }

        std::string triedSize;   // 试过的覆盖尺寸，失败时写进 note
        if (realW > realH) {   // 面板原生就是横的，reset 解决不了
            const std::string psz = std::to_string(realH) + "x" +
                                    std::to_string(realW);
            if (RunWindowCmd({"size", psz}, nullptr, &e)) {
                triedSize = psz;
                usleep(400 * 1000);
                snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
                if (realW == 0 || realH == 0) { realW = w; realH = h; }
            }
        }

        // 竖屏几何：高 >= 宽。**看真实尺寸**，不看 wm size ——
        // wm size 那层覆盖在本项目的横屏模拟器上不改变 framebuffer。
        const bool ok = (realH >= realW);
        if (ok) {
            syncInjector();
        } else if (!triedSize.empty()) {
            // 覆盖压不出竖屏，别把没用的覆盖留在设备上 ——
            // 留着会让 wm size / dumpsys 的读数跟真实画面长期不一致，
            // 下次来查的人会被它带偏。
            std::string e2;
            RunWindowCmd({"size", "reset"}, nullptr, &e2);
            usleep(300 * 1000);
            snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
            if (realW == 0 || realH == 0) { realW = w; realH = h; }
        }
        return reply(ok, "wm-size", deg,
                     ok ? "这台设备不支持旋转（mRotation 不动），"
                          "已用 wm size 还原成竖屏尺寸"
                        : ("这台设备不光转不了（mRotation 不动），"
                           "wm size 也压不出竖屏 —— 试过 " +
                           (triedSize.empty() ? std::string("（尺寸没设进去）")
                                              : triedSize) +
                           "，面板物理尺寸 " + std::to_string(realW) + "x" +
                           std::to_string(realH) + " 没变：覆盖尺寸改的只是"
                           "应用可见区域，抓帧拿到的还是这个方向。"
                           "要竖屏只能换竖屏的模拟器皮肤"));
    }

    // 横屏：从**物理**尺寸算，别用当前（可能已经被覆盖过的）尺寸
    uint32_t pw = 0, ph = 0;
    {
        std::string out, e;
        if (RunWindowCmd({"size"}, &out, &e) &&
            ParseOneSize(out, "Physical size:", &pw, &ph)) {
            // 用到了
        }
    }
    if (pw == 0 || ph == 0) { pw = w; ph = h; }
    if (pw == 0 || ph == 0) {
        return MakeJsonError(req.cmd, kErrInternal, "读不到显示尺寸");
    }
    const uint32_t lw = std::max(pw, ph);
    const uint32_t lh = std::min(pw, ph);
    const std::string sz = std::to_string(lw) + "x" + std::to_string(lh);

    std::string e;
    if (!RunWindowCmd({"size", sz}, nullptr, &e)) {
        return MakeJsonError(req.cmd, kErrInternal, e);
    }
    usleep(400 * 1000);
    snapshot(&rot, &isFree, &w, &h, &actual, &realW, &realH);
    if (realW == 0 || realH == 0) { realW = w; realH = h; }

    const bool ok = (realW == lw && realH == lh);
    if (ok) syncInjector();
    return reply(ok, "wm-size", deg,
                 ok ? ("这台设备不支持旋转（mRotation 不变），已改用 wm size "
                       "把显示尺寸设成 " + sz + "，应用会按横屏重新布局")
                    : ("wm size " + sz + " 没有生效，画面仍是 " +
                       std::to_string(realW) + "x" + std::to_string(realH)));
}

// ── v6：服务开关 / 运行状态 / 历史日志 ─────────────────────────────────────

ReplyPacket Dispatcher::HandleServiceSwitch(
        const Request& req, const std::vector<std::string>& args) {
    const std::string op = args.empty() ? "status" : args[0];
    ServiceState& st = ServiceState::Instance();

    if (op == "on") {
        st.SetServing(true);
    } else if (op == "off") {
        st.SetServing(false);
    } else if (op != "status") {
        return MakeJsonError(req.cmd, kErrBadArg,
                             "未知操作: " + op + "（可用 on|off|status）");
    }

    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("serving", st.Serving())
        .Field("note", st.Serving()
                           ? "服务对外可用"
                           : "服务已关闭对外能力（进程仍在运行，可用本接口重新开启）")
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleRunningApps(const Request& req) {
    // 数据源用 `dumpsys activity lru`。
    //
    // 为什么不用 `ps`：ps 给的是 UID（u0_a105）不是包名，要额外查
    // PackageManager 才能对上；而且 ps 里进程一堆内核线程，噪音大。
    // lru 输出直接是 "<pid>:<包名>/<uid>"，还带前台/可见/缓存的状态，
    // 正好是"应用运行状态"要的东西。
    std::vector<std::string> argv = {
        "/system/bin/dumpsys", "activity", "lru",
    };
    CommandResult r;
    std::string err;
    if (!RunCommand(argv, 15000, 1u << 20, &r, &err) || r.exitCode != 0) {
        return MakeJsonError(req.cmd, kErrInternal,
                             "取运行状态失败: " + (err.empty() ? r.err : err));
    }

    // 先拿系统应用清单，用来给每个进程打 system 标记。
    //
    // lru 的输出里没有这个信息，而"只看第三方应用"是排障时最常见的需求 ——
    // 二十多个进程里大部分是系统组件。用 uid 猜（< 10000 算系统）不靠谱：
    // com.android.settings 的 app id 恰好是 1000，但它明显是系统应用，
    // 而某些预装应用的 id 又大于 10000。所以直接问 PackageManager。
    std::vector<std::string> sysArgv = {
        "/system/bin/pm", "list", "packages", "-s",
    };
    CommandResult sysRes;
    std::vector<std::string> sysPkgs;
    if (RunCommand(sysArgv, 15000, 1u << 20, &sysRes, nullptr) &&
        sysRes.exitCode == 0) {
        size_t sp = 0;
        while (sp < sysRes.out.size()) {
            const size_t e = sysRes.out.find('\n', sp);
            std::string l = sysRes.out.substr(
                    sp, e == std::string::npos ? std::string::npos : e - sp);
            sp = (e == std::string::npos) ? sysRes.out.size() : e + 1;
            // "package:com.android.settings" 或 "package:com.x -> /path"
            const size_t c = l.find(':');
            if (c == std::string::npos) continue;
            std::string name = l.substr(c + 1);
            const size_t sp2 = name.find_first_of(" \t\r");
            if (sp2 != std::string::npos) name = name.substr(0, sp2);
            if (!name.empty()) sysPkgs.push_back(name);
        }
    }

    json::Writer w;
    w.Obj().Field("ok", true);
    w.Key("apps").Arr();

    // 行形如：
    //   #21: fg     TOP  LCMN 32373:com.autod.controller/u0a105 act:activities
    //   #17: cch+ 5 CEM  ---- 1683:com.android.permissioncontroller/u0a102
    size_t pos = 0;
    uint32_t count = 0;
    const std::string& out = r.out;
    while (pos < out.size() && count < 300) {
        const size_t eol = out.find('\n', pos);
        const std::string line = out.substr(
                pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? out.size() : eol + 1;

        // 只认 "#<数字>:" 开头的行。
        //
        // ⚠️ dumpsys 的输出是**带缩进的**（"    #21: fg TOP ..."），
        //    早先直接判断 line[0] == '#' 于是一行都匹配不上，
        //    接口老老实实返回了 0 个进程 —— 这种"格式对但没数据"
        //    的 bug 最难发现，因为不报错。
        size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos || line[lead] != '#') continue;
        const size_t colon = line.find(':', lead);
        if (colon == std::string::npos) continue;

        // "#17: cch+ 5 CEM  ---- 1683:pkg/uid"
        const std::string rest = line.substr(colon + 1);
        const size_t digits = rest.find_first_not_of(" \t");
        if (digits == std::string::npos) continue;
        const size_t pidEnd = rest.find(':', digits);
        if (pidEnd == std::string::npos) continue;

        // pid 是**冒号前那一串的最后一个 token**。
        //
        // 冒号前是 "fg     TOP  LCMN 32373" —— 直接取整串会带上状态词，
        // 然后"必须是纯数字"的检查就把它全滤掉了，接口老实地返回 0 个。
        // 这种"格式对但解析错"的 bug 不报错，只能靠人对着真实输出核。
        const std::string before = rest.substr(digits, pidEnd - digits);
        size_t pe = before.find_last_not_of(" \t");
        if (pe == std::string::npos) continue;
        size_t pb = before.find_last_of(" \t");
        const std::string pidStr =
                before.substr(pb == std::string::npos ? 0 : pb + 1, pe - (pb == std::string::npos ? 0 : pb + 1) + 1);
        if (pidStr.empty() ||
            pidStr.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }

        // 状态取**第一个** token（adj：fg / vis / cch / prcp / psvc）。
        //
        // 取最后一个会拿到标志位（LCMN / ----），那个对"应用在不在前台"
        // 没有意义。adj 才是 Android 用来描述进程优先级的东西。
        std::string state;
        {
            size_t sb = before.find_first_not_of(" \t");
            if (sb != std::string::npos) {
                size_t se = before.find_first_of(" \t", sb);
                state = before.substr(sb, se == std::string::npos
                                              ? std::string::npos : se - sb);
            }
        }

        // pid 之后是 "pkg/uid"，再往后是可选的 " act:..."
        std::string tail = rest.substr(pidEnd + 1);
        const size_t sp = tail.find(' ');
        if (sp != std::string::npos) tail = tail.substr(0, sp);
        const size_t slash = tail.rfind('/');
        const std::string proc = (slash == std::string::npos) ? tail : tail.substr(0, slash);
        const std::string uid = (slash == std::string::npos) ? "" : tail.substr(slash + 1);
        if (proc.empty()) continue;

        // "com.android.webview:webview_service" 这种是子进程。
        // 包名取冒号前那段（结束进程时用的是包名），完整进程名另存一个字段。
        const size_t sub = proc.find(':');
        const std::string pkg =
                (sub == std::string::npos) ? proc : proc.substr(0, sub);

        // 系统应用判定要**三条一起看**，任何单独一条都有盲区：
        //
        //   1. 包名在 `pm list packages -s` 里 —— 权威，但覆盖不到
        //      没有对应包的进程（system_server 的进程名就叫 "system"，
        //      android.process.acore 的进程名也不是包名）
        //   2. uid 不是 u0aNN 形式（纯数字，如 1000）—— 可靠，但
        //      launcher3 这类预装应用用的是 app uid（u0a90），会漏
        //   3. 进程名里带 ".process." —— 那是框架的共享进程命名法，
        //      取它前面那段当包名再查一次
        std::string basePkg = pkg;
        const size_t pp = basePkg.find(".process.");
        if (pp != std::string::npos) basePkg = basePkg.substr(0, pp);

        const bool byUid  = !uid.empty() && uid[0] != 'u';
        const bool byName = (basePkg == "android") || (basePkg == "system");
        const bool byList = std::find(sysPkgs.begin(), sysPkgs.end(), basePkg)
                                != sysPkgs.end();
        const bool isSys = byUid || byName || byList;

        w.Obj()
            .Field("package", pkg)
            .Field("process", proc)
            .Field("pid", static_cast<int64_t>(strtol(pidStr.c_str(), nullptr, 10)))
            .Field("uid", uid)
            .Field("state", state)
            .Field("system", isSys)
         .EndObj();
        ++count;
    }
    w.EndArr();
    w.Field("count", static_cast<int64_t>(count));
    w.EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

ReplyPacket Dispatcher::HandleLogFile(const Request& req) {
    const std::string path = LogBuffer::Instance().HistoryPath();
    const std::string text = LogBuffer::Instance().ReadHistory();

    json::Writer w;
    w.Obj()
        .Field("ok", true)
        .Field("path", path)
        .Field("bytes", static_cast<int64_t>(text.size()))
        .Field("maxBytes", static_cast<int64_t>(LogBuffer::kHistoryMaxBytes))
        .Field("text", text)
     .EndObj();
    return MakeJsonReply(req.cmd, w.str());
}

}  // namespace autod
