// service_state.cpp — 服务的运行时状态与自身控制

#include "config_file.h"
#include "service_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "remote_control_log.h"
#include "capture.h"
#include "inject.h"
#include "json_writer.h"
#include "keyboard.h"
#include "log_buffer.h"
#include "protocol.h"

namespace remote_control {
namespace {

int64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 解析 "1080x2400" / "1080X2400" / "1080*2400"
bool ParseSize(const std::string& s, uint32_t* w, uint32_t* h) {
    const size_t sep = s.find_first_of("xX*, ");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= s.size()) return false;
    char* end = nullptr;
    const long a = strtol(s.substr(0, sep).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || a <= 0) return false;
    const long b = strtol(s.substr(sep + 1).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || b <= 0) return false;
    *w = static_cast<uint32_t>(a);
    *h = static_cast<uint32_t>(b);
    return true;
}

bool ParseBool(const std::string& s, bool* out) {
    if (s == "1" || s == "true" || s == "yes" || s == "on")  { *out = true;  return true; }
    if (s == "0" || s == "false" || s == "no" || s == "off") { *out = false; return true; }
    return false;
}

std::string CommandName(uint32_t cmd) {
    switch (static_cast<Cmd>(cmd)) {
        case Cmd::Info:          return "Info";
        case Cmd::Capture:       return "Capture";
        case Cmd::Tap:           return "Tap";
        case Cmd::Swipe:         return "Swipe";
        case Cmd::TouchDown:     return "TouchDown";
        case Cmd::TouchMove:     return "TouchMove";
        case Cmd::TouchUp:       return "TouchUp";
        case Cmd::KeyEvent:      return "KeyEvent";
        case Cmd::ListApps:      return "ListApps";
        case Cmd::AppInfo:       return "AppInfo";
        case Cmd::LaunchApp:     return "LaunchApp";
        case Cmd::KillApp:       return "KillApp";
        case Cmd::ForegroundApp: return "ForegroundApp";
        case Cmd::InstallApp:    return "InstallApp";
        case Cmd::Download:      return "Download";
        case Cmd::FileOp:        return "FileOp";
        case Cmd::Describe:      return "Describe";
        case Cmd::GetConfig:     return "GetConfig";
        case Cmd::SetConfig:     return "SetConfig";
        case Cmd::SelfTest:      return "SelfTest";
        case Cmd::Stats:         return "Stats";
        case Cmd::Log:           return "Log";
        case Cmd::Shutdown:      return "Shutdown";
        case Cmd::Restart:       return "Restart";
        // v4
        case Cmd::LongPress:     return "LongPress";
        case Cmd::Drag:          return "Drag";
        case Cmd::DoubleTap:     return "DoubleTap";
        case Cmd::Clipboard:     return "Clipboard";
        // v5
        case Cmd::Power:         return "Power";
        // v6
        case Cmd::ServiceSwitch: return "ServiceSwitch";
        case Cmd::RunningApps:   return "RunningApps";
        case Cmd::LogFile:       return "LogFile";
        default:                 return "Cmd" + std::to_string(cmd);
    }
}

}  // namespace

ServiceState& ServiceState::Instance() {
    static ServiceState s;
    return s;
}

uint32_t ServiceState::ProtocolVersion() { return kProtocolVersion; }

// ── 配置 ────────────────────────────────────────────────────────────────────
void ServiceState::SetInitialConfig(const Config& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_       = c;
    socketMode_   = c.socketMode;
    startTimeMs_  = MonotonicMs();
}

ServiceState::Config ServiceState::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Config c = config_;
    c.socketMode = socketMode_;
    return c;
}

void ServiceState::SetBackends(Capture* capture, Injector* injector) {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_  = capture;
    injector_ = injector;
}

void ServiceState::SetInjectorConfig(const InjectorConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    injectorConfig_ = cfg;
}

bool ServiceState::RebuildInjectorForDisplay(uint32_t w, uint32_t h,
                                             bool explicitRange,
                                             std::string* error) {
    if (w == 0 || h == 0) return true;          // 没尺寸信息，不动
    if (explicitRange) return true;             // 用户指定过，尊重它

    Injector* inj = nullptr;
    InjectorConfig cur;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inj = injector_;
        cur = injectorConfig_;
    }
    if (inj == nullptr) return true;
    if (cur.touchWidth == w && cur.touchHeight == h) return true;   // 已经一致

    InjectorConfig cfg = cur;
    cfg.touchWidth  = w;
    cfg.touchHeight = h;
    if (!inj->Init(cfg, error)) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        injectorConfig_ = cfg;
    }
    ALOGI("显示尺寸变为 %ux%u，已重建注入设备（坐标范围跟进）", w, h);
    return true;
}

InjectorConfig ServiceState::GetInjectorConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return injectorConfig_;
}

void ServiceState::SetSocketChmodHook(void (*hook)(uint32_t)) {
    std::lock_guard<std::mutex> lock(mutex_);
    socketChmodHook_ = hook;
}

uint32_t ServiceState::CurrentSocketMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return socketMode_;
}

// ── JSON 输出 ───────────────────────────────────────────────────────────────

std::string ServiceState::ConfigJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Writer w;
    w.Obj()
        .Field("socketPath", config_.socketPath)
        .Field("usingInitSocket", config_.usingInitSocket)
        .Field("initSocketName", config_.initSocketName)
        .Field("socketMode", std::to_string(socketMode_))
        .Field("displayId", config_.displayId)
        .Field("touchWidth", config_.touchWidth)
        .Field("touchHeight", config_.touchHeight)
        .Field("verbose", config_.verbose)
        .Field("dropUid", config_.dropUid)
        .Field("dropGid", config_.dropGid)
     .EndObj();
    return w.str();
}

std::string ServiceState::RuntimeJson() const {
    // 先复制出指针与配置，再在锁外采集 —— 采集要调后端，可能慢，
    // 拿着锁去调外部对象是死锁的经典配方。
    Capture*  cap = nullptr;
    Injector* inj = nullptr;
    bool      verbose = false;
    int64_t   started = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cap = capture_;
        inj = injector_;
        verbose = config_.verbose;
        started = startTimeMs_;
    }

    json::Writer w;
    w.Obj()
        .Field("pid", static_cast<int64_t>(getpid()))
        .Field("uid", static_cast<int64_t>(getuid()))
        .Field("gid", static_cast<int64_t>(getgid()))
        .Field("uptimeMs", MonotonicMs() - started)
        .Field("protocolVersion", kProtocolVersion)
        .Field("verbose", verbose);

    if (cap != nullptr) {
        w.Key("capture").Obj().Field("backend", Capture::BackendName());
        std::vector<DisplayInfo> displays;
        std::string err;
        if (cap->ListDisplays(&displays, &err) && !displays.empty()) {
            w.Field("displayCount", static_cast<uint64_t>(displays.size()));
            w.Key("displays").Arr();
            for (const auto& d : displays) {
                w.Obj().Field("id", d.id).Field("width", d.width)
                                       .Field("height", d.height)
                                       .Field("refreshHz", d.refreshHz)
                 .EndObj();
            }
            w.EndArr();
            w.Field("primaryWidth", displays.front().width)
             .Field("primaryHeight", displays.front().height);
        } else {
            w.Field("displayCount", static_cast<uint64_t>(0));
            w.Field("error", err);
        }
        w.EndObj();
    } else {
        w.Key("capture").Obj().Field("backend", "not initialized").EndObj();
    }

    if (inj != nullptr) {
        w.Key("inject").Obj().Field("backend", inj->BackendName());
        w.EndObj();
    } else {
        w.Key("inject").Obj().Field("backend", "not initialized").EndObj();
    }

    // 键盘是**延迟创建**的（第一次 POST /key 才建），所以这里报的是
    // 进程级状态，跟 capture/inject 的实例后端名不完全是一回事。
    w.Key("keyboard").Obj()
        .Field("backend", Keyboard::BackendName())
        .Field("ready", Keyboard::AnyReady())
     .EndObj();

    w.EndObj();
    return w.str();
}

void ServiceState::PersistAuthToken(const std::string& token) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = servingConfigPath_;
    }
    if (path.empty()) return;

    // 读-改-写：只动 token 一个字段，别把别的配置项冲掉
    PersistedConfig cfg;
    std::string err;
    if (!ConfigFile::Load(path, &cfg, &err)) {
        // 文件可能还不存在 —— 用默认值继续，Save 会创建它
        cfg = PersistedConfig{};
    }
    cfg.token = token;
    cfg.auth  = !token.empty();
    if (!ConfigFile::Save(path, cfg, &err)) {
        ALOGW("令牌写回 %s 失败: %s", path.c_str(), err.c_str());
    } else {
        ALOGI("令牌已写入 %s", path.c_str());
    }
}

std::string ServiceState::AuthToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return authToken_;
}

void ServiceState::SetAuthToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    authToken_ = token;
}

std::string ServiceState::StatsJson(
        const std::function<void(json::Writer&)>& extra) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Writer w;
    w.Obj()
        .Field("uptimeMs", MonotonicMs() - startTimeMs_)
        .Field("requests", requests_)
        .Field("errors", errors_)
        .Field("logDropped", LogBuffer::Instance().DroppedCount())
        .Key("byCommand").Obj();
    for (int i = 0; i < 64; ++i) {
        if (byCommand_[i] == 0) continue;
        w.Field(CommandName(static_cast<uint32_t>(i)).c_str(), byCommand_[i]);
    }
    w.EndObj();                 // 关 byCommand
    if (extra) extra(w);        // 调用方追加（FrameHub 等）
    w.EndObj();                 // 关外层
    return w.str();
}

// ── 热改配置 ────────────────────────────────────────────────────────────────
ServiceState::ApplyResult ServiceState::Apply(
        const std::vector<std::pair<std::string, std::string>>& kv) {
    ApplyResult result;

    for (const auto& item : kv) {
        const std::string& key = item.first;
        const std::string& val = item.second;

        // ── 能立刻生效的 ──

        if (key == "verbose" || key == "log-level") {
            bool on = false;
            LogLevel lv = LogLevel::kInfo;
            if (key == "log-level") {
                if (val == "debug")      lv = LogLevel::kDebug;
                else if (val == "info")  lv = LogLevel::kInfo;
                else if (val == "warn")  lv = LogLevel::kWarn;
                else if (val == "error") lv = LogLevel::kError;
                else {
                    result.rejected.emplace_back(key, "级别必须是 debug|info|warn|error");
                    continue;
                }
                on = (lv == LogLevel::kDebug);
            } else {
                if (!ParseBool(val, &on)) {
                    result.rejected.emplace_back(key, "需要 true/false");
                    continue;
                }
                lv = on ? LogLevel::kDebug : LogLevel::kInfo;
            }
            LogBuffer::Instance().SetMinLevel(lv);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                config_.verbose = on;
            }
            result.applied.push_back(key);
            ALOGI("配置变更: %s = %s", key.c_str(), val.c_str());
            continue;
        }

        if (key == "display") {
            char* end = nullptr;
            const unsigned long long id = strtoull(val.c_str(), &end, 0);
            if (end == nullptr || *end != '\0') {
                result.rejected.emplace_back(key, "需要数字（支持 0x 前缀）");
                continue;
            }
            Capture* cap = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cap = capture_;
                config_.displayId = id;
            }
            if (cap != nullptr) {
                cap->SetDisplayId(id);
                // 立刻解析一次，把"这个 id 到底能不能用"当场反馈给调用方，
                // 而不是等他下次截图才发现不行
                std::string err;
                if (!cap->ResolveDisplay(&err)) {
                    result.rejected.emplace_back(key, "已设置但解析失败: " + err);
                    continue;
                }
            }
            result.applied.push_back(key);
            ALOGI("配置变更: display = %s", val.c_str());
            continue;
        }

        if (key == "socket-mode") {
            const unsigned long m = strtoul(val.c_str(), nullptr, 8);
            if (m == 0 || m > 0777) {
                result.rejected.emplace_back(key, "需要八进制权限位（如 0660/0666）");
                continue;
            }
            void (*hook)(uint32_t) = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                socketMode_ = static_cast<uint32_t>(m);
                config_.socketMode = socketMode_;
                hook = socketChmodHook_;
            }
            // 真正作用到已经在监听的 socket 文件上 ——
            // 不然"改成功了"只是改了个数字，用户下次连接仍然是老权限
            if (hook != nullptr) hook(static_cast<uint32_t>(m));
            result.applied.push_back(key);
            ALOGI("配置变更: socket-mode = %s", val.c_str());
            continue;
        }

        if (key == "touch-range" || key == "touch-width" || key == "touch-height") {
            uint32_t w = 0;
            uint32_t h = 0;
            if (key == "touch-range") {
                if (!ParseSize(val, &w, &h)) {
                    result.rejected.emplace_back(key, "需要 WxH 形式，如 1080x2400");
                    continue;
                }
            } else {
                char* end = nullptr;
                const long v = strtol(val.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || v <= 0) {
                    result.rejected.emplace_back(key, "需要正整数");
                    continue;
                }
                InjectorConfig cur = GetInjectorConfig();
                if (key == "touch-width") { w = static_cast<uint32_t>(v); h = cur.touchHeight; }
                else                      { h = static_cast<uint32_t>(v); w = cur.touchWidth;  }
            }

            Injector* inj = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                inj = injector_;
            }
            InjectorConfig cfg = GetInjectorConfig();
            cfg.touchWidth  = w;
            cfg.touchHeight = h;
            if (inj != nullptr) {
                // 改坐标范围要重建 uinput 设备：ABS 范围是设备创建时定死的，
                // ioctl 改不了。Injector::Init 先打开新设备，再释放旧设备。
                std::string err;
                if (!inj->Init(cfg, &err)) {
                    result.rejected.emplace_back(key, "重建注入设备失败: " + err);
                    continue;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                config_.touchWidth  = w;
                config_.touchHeight = h;
                injectorConfig_ = cfg;
            }
            result.applied.push_back(key);
            ALOGI("配置变更: %s = %s（已重建注入设备）", key.c_str(), val.c_str());
            continue;
        }

        // ── 需要重启的 ──
        //
        // 这几项在进程生命周期内无法改变：socket 一旦 bind 就定了；
        // 降权一旦做下去就回不去（setuid 不可逆）。
        // **如实报告而不是假装成功** —— 假装成功会让调用方以为改了，
        // 而实际行为没变，这种不一致最难排查。
        if (key == "socket" || key == "init-socket" || key == "uid" || key == "gid") {
            result.requiresRestart.push_back(key);
            continue;
        }

        else if (key == "auth") {
            // 开启/关闭鉴权。**热改** —— 不用重启进程。
            //
            // 开启时若还没有令牌就生成一个（和启动时同一套逻辑）：
            // 生成失败就明确拒绝，绝不降级成弱令牌。
            const bool wantAuth = (val == "1" || val == "true" || val == "on");
            // ⚠️ 走加锁的访问器 —— Apply 本身**不持锁**，
            //    直接碰 authToken_ 是数据竞争（HttpServer 那边在读）。
            if (wantAuth) {
                if (AuthToken().empty()) {
                    const std::string fresh = ConfigFile::GenerateToken();
                    if (fresh.empty()) {
                        result.rejected.emplace_back(
                                key, "拿不到安全的随机数，拒绝开启鉴权"
                                     "（用弱令牌比明说没开更危险）");
                        continue;
                    }
                    SetAuthToken(fresh);
                    result.applied.emplace_back("auth");
                    result.applied.emplace_back("token");

                    // ⚠️ 必须写回配置文件。
                    //
                    //    不写的话：令牌只活在内存里，重启就没了，
                    //    而用户**没有任何地方能看到它** ——
                    //    等于把自己锁在门外（下一个请求就 401，
                    //    却不知道令牌是什么）。
                    PersistAuthToken(fresh);
                } else {
                    result.applied.emplace_back("auth");
                }
            } else {
                SetAuthToken("");
                PersistAuthToken("");
                result.applied.emplace_back("auth");
            }
        } else if (key == "token") {
            // 直接设一个指定令牌。空串 = 关掉鉴权。
            SetAuthToken(val);
            PersistAuthToken(val);
            result.applied.emplace_back("token");
        }
        else {
            result.rejected.emplace_back(key, "未知配置项");
        }
    }

    return result;
}

std::string ServiceState::ApplyResultJson(const ApplyResult& r) const {
    json::Writer w;
    w.Obj().Field("ok", true)
        .Key("applied").Arr();
    for (const auto& k : r.applied) w.Val(k);
    w.EndArr()
        .Key("requiresRestart").Arr();
    for (const auto& k : r.requiresRestart) w.Val(k);
    w.EndArr()
        .Key("rejected").Arr();
    for (const auto& kv : r.rejected) {
        w.Obj().Field("key", kv.first).Field("reason", kv.second).EndObj();
    }
    w.EndArr();
    w.Field("changed", r.AnyChange());
    w.EndObj();
    return w.str();
}

// ── 统计 ────────────────────────────────────────────────────────────────────
void ServiceState::CountRequest(uint32_t cmd, uint32_t status) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++requests_;
    if (status != kOk) ++errors_;
    if (cmd < 64) ++byCommand_[cmd];
}

// ── 生命周期 ────────────────────────────────────────────────────────────────
void ServiceState::RequestShutdown(bool restart) {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    restart_  = restart;
}

// ── 服务对外开关 ────────────────────────────────────────────────────────────
//
// 软开关：关掉之后进程照跑，只是不再对外提供服务。
// 做真停进程的话就没人能开回来了 —— 网页打不开、接口不通。

bool ServiceState::Serving() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serving_;
}

void ServiceState::SetServing(bool on) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serving_ == on) return;
        serving_ = on;
        servingConfigPathSnapshot_ = servingConfigPath_;
    }
    ALOGW("服务对外开关: %s", on ? "开" : "关（进程继续运行，仅停止对外服务）");

    // 持久化，让它跨重启。写失败不影响本次生效 —— 开关本身是运行时的。
    const std::string path = servingConfigPathSnapshot_;
    if (!path.empty()) {
        PersistedConfig cfg;
        std::string err;
        if (ConfigFile::Load(path, &cfg, &err)) {
            cfg.enabled = on;
            std::string serr;
            if (!ConfigFile::Save(path, cfg, &serr)) {
                ALOGW("服务开关写回 %s 失败: %s", path.c_str(), serr.c_str());
            }
        }
    }
}

void ServiceState::SetServingPersistPath(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    servingConfigPath_ = configPath;
}

bool ServiceState::ShutdownRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

bool ServiceState::RestartRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_;
}

int64_t ServiceState::UptimeMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return MonotonicMs() - startTimeMs_;
}

}  // namespace remote_control
