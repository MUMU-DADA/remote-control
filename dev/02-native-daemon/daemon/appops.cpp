// appops.cpp — 应用管理后端实现

#include "appops.h"

#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>

#include "autod_log.h"
#include "subprocess.h"

namespace autod {
namespace {

// ── 文本工具 ────────────────────────────────────────────────────────────────

// 前缀判断。
//
// ⚠️ 不要写 s.compare(0, N, prefix) —— N 要手数，数错不会报错，
//    只会静默地永不匹配（"/system_ext/" 是 12 不是 14，
//    结果系统应用被误判成第三方）。让编译器去数。
bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 在 text 里找 "key="，返回它后面到行尾（或到 delim）的内容
std::string FieldValue(const std::string& text, const char* key,
                       char delim = '\n') {
    const std::string k(key);
    size_t pos = text.find(k);
    if (pos == std::string::npos) return {};
    pos += k.size();
    size_t end = text.find(delim, pos);
    if (end == std::string::npos) end = text.size();
    return Trim(text.substr(pos, end - pos));
}

bool HasFlag(const std::string& flagsField, const char* flag) {
    // flags=[ SYSTEM HAS_CODE ... ] —— 按空白切词精确比对，避免
    // "SYSTEM" 误匹配 "SYSTEM_EXT"
    std::istringstream iss(flagsField);
    std::string tok;
    while (iss >> tok) {
        if (tok == flag) return true;
    }
    return false;
}

// 把 "com.foo/.Bar" 或 "com.foo/com.foo.Bar" 补全成完整类名
std::string ExpandComponent(const std::string& pkg, const std::string& comp) {
    if (comp.empty()) return {};
    if (comp[0] == '.') return pkg + comp;
    // "com.foo.Bar" 已经是全名；但如果它不含包名前缀，Activity 全名是 pkg + "." + comp
    if (comp.compare(0, pkg.size(), pkg) == 0) return comp;
    return pkg + "." + comp;
}

// 从一段文本里抽出所有 "<pkg>/<component>" 形式的组件名
void ExtractComponents(const std::string& text, const std::string& pkg,
                       std::vector<std::string>* out) {
    const std::string needle = pkg + "/";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        size_t start = pos + needle.size();
        size_t end = start;
        while (end < text.size()) {
            const char c = text[end];
            if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' ||
                c == '$') {
                ++end;
            } else {
                break;
            }
        }
        std::string comp = text.substr(start, end - start);
        if (!comp.empty()) {
            const std::string full = ExpandComponent(pkg, comp);
            if (std::find(out->begin(), out->end(), full) == out->end()) {
                out->push_back(full);
            }
        }
        pos = end;
    }
}

// 取 [begin, end) 之间的子串；找不到返回空
std::string Section(const std::string& text, const char* begin, const char* end) {
    size_t b = text.find(begin);
    if (b == std::string::npos) return {};
    b += strlen(begin);
    size_t e = text.find(end, b);
    if (e == std::string::npos) e = text.size();
    return text.substr(b, e - b);
}

}  // namespace

// ── 包名合法性 ──────────────────────────────────────────────────────────────
//
// 不作为安全边界（安全靠"不走 shell"），但能挡住明显的垃圾输入，
// 让报错发生在调用前而不是在 pm 的输出里。
bool AppOps::ValidPackageName(const std::string& p) {
    if (p.empty() || p.size() > 255) return false;
    bool prevDot = true;   // 首字符不能是点
    for (char c : p) {
        if (c == '.') {
            if (prevDot) return false;   // 连续点 / 以点开头
            prevDot = true;
            continue;
        }
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        prevDot = false;
    }
    return !prevDot;   // 不能以点结尾
}

// ── Init ────────────────────────────────────────────────────────────────────
bool AppOps::Init(std::string* error) {
    // pm / am 是 app_process 的脚本，Android 上一定有。
    // 但 --selftest 在非 Android 环境跑时不该直接失败，只提示。
    const char* needed[] = {"/system/bin/pm", "/system/bin/am",
                            "/system/bin/cmd", "/system/bin/dumpsys"};
    std::string missing;
    for (const char* p : needed) {
        if (access(p, X_OK) != 0) {
            if (!missing.empty()) missing += ", ";
            missing += p;
        }
    }
    if (!missing.empty()) {
        if (error) *error = "缺少命令: " + missing;
        ALOGW("appops: %s", error ? error->c_str() : "");
        return false;
    }
    if (!loggedOnce_) {
        // 只打一次。Init 会被反复调用（Describe 对每个应用类命令都要
        // 判定一次可用性），每次都打日志会把它刷成噪音 ——
        // 实测一次 Describe 就打出 9 条一模一样的"后端就绪"。
        ALOGI("应用管理后端就绪 (pm/am/cmd/dumpsys)");
        loggedOnce_ = true;
    }
    return true;
}

// ── ListApps ────────────────────────────────────────────────────────────────
bool AppOps::ListApps(bool includeSystem, bool withMetadata,
                      std::vector<AppEntry>* out, std::string* error) {
    out->clear();

    std::vector<std::string> argv = {"/system/bin/pm", "list", "packages"};
    if (!includeSystem) argv.push_back("-3");
    if (withMetadata) {
        argv.push_back("-f");                 // package:/path/base.apk=com.foo
        argv.push_back("--show-versioncode"); // ... versionCode:42
        argv.push_back("-i");                 // ... installer=com.foo
    }

    CommandResult r;
    std::string runErr;
    if (!RunCommand(argv, 20000, 8u << 20, &r, &runErr)) {
        if (error) *error = runErr;
        return false;
    }
    if (r.exitCode != 0) {
        if (error) *error = "pm list packages 退出码 " + std::to_string(r.exitCode);
        return false;
    }

    std::istringstream iss(r.out);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (!StartsWith(line, "package:")) continue;
        std::string rest = line.substr(8);

        AppEntry e;
        // 带 -f 时第一个字段形如：
        //   package:/data/app/~~RV2tSp9GaJ8wp2iv2AkdIw==/com.foo-xxx==/base.apk=com.foo
        // 后面还有 " versionCode:42  installer=null"。
        //
        // ⚠️ 找分隔等号不能简单用 find 或 rfind：
        //    - find('=')  会切在**路径内部**（Android 11+ 的 /data/app 目录名是
        //                 base64 风格的 ~~xxx==），把 apkPath 当成包名；
        //    - rfind('=') 会切在后面的 installer=null 上。
        //    这个坑只有装上真实的第三方应用才暴露 —— 系统应用路径里没有 '='。
        //
        //    正确规则：分隔等号 = **最后一个 '/' 之后的第一个 '='**。
        //    包名里不含 '/'，所以这个等号一定是路径与包名的分界。
        if (withMetadata) {
            const std::string first = rest.substr(0, rest.find(' '));
            const size_t slash = first.rfind('/');
            const size_t eq = (slash == std::string::npos)
                                  ? first.find('=')
                                  : first.find('=', slash);
            if (eq != std::string::npos) {
                e.apkPath = first.substr(0, eq);
                rest      = first.substr(eq + 1) + rest.substr(first.size());
            }
        }

        // rest 现在是 "com.foo" 或 "com.foo versionCode:42 installer=com.bar"
        std::istringstream fs(rest);
        std::string first;
        fs >> first;
        e.package = first;

        std::string tok;
        while (fs >> tok) {
            if (StartsWith(tok, "versionCode:")) {
                e.versionCode = strtoll(tok.c_str() + strlen("versionCode:"), nullptr, 10);
            } else if (StartsWith(tok, "installer=")) {
                e.installer = tok.substr(strlen("installer="));
            }
        }

        if (!e.package.empty()) out->push_back(std::move(e));
    }

    // 系统 / 第三方没法直接从 pm 的输出判断（不带 -s/-3 时是全部）。
    // 用包名前缀做启发式没有意义，所以 includeSystem 时统一标 system=true
    // 由调用方结合 -3 的结果判断。这里按"是否在 /system 分区"粗判：
    for (auto& e : *out) {
        e.system = !e.apkPath.empty() &&
                   (StartsWith(e.apkPath, "/system/") ||
                    StartsWith(e.apkPath, "/system_ext/") ||
                    StartsWith(e.apkPath, "/vendor/") ||
                    StartsWith(e.apkPath, "/product/") ||
                    StartsWith(e.apkPath, "/apex/"));
    }

    ALOGI("列出 %zu 个应用（includeSystem=%d, withMetadata=%d）", out->size(),
          includeSystem ? 1 : 0, withMetadata ? 1 : 0);
    return true;
}

// ── Detail ──────────────────────────────────────────────────────────────────
bool AppOps::Detail(const std::string& package, AppDetail* out,
                    std::string* error) {
    if (!ValidPackageName(package)) {
        if (error) *error = "非法包名: " + package;
        return false;
    }
    CommandResult r;
    if (!RunCommand({"/system/bin/dumpsys", "package", package}, 15000,
                    16u << 20, &r, nullptr)) {
        if (error) *error = "执行 dumpsys 失败";
        return false;
    }
    return ParseDetail(r.out, package, out, error);
}

bool AppOps::ParseDetail(const std::string& dump, const std::string& package,
                         AppDetail* out, std::string* error) {
    *out = AppDetail{};
    out->package = package;

    if (dump.find("Package [" + package + "]") == std::string::npos) {
        if (error) *error = "包不存在: " + package;
        return false;
    }

    const std::string& t = dump;
    out->versionName = FieldValue(t, "versionName=");
    out->apkPath     = FieldValue(t, "codePath=");
    out->dataDir     = FieldValue(t, "dataDir=");
    out->installer   = FieldValue(t, "installerPackageName=");
    out->firstInstallTime = FieldValue(t, "firstInstallTime=");
    out->lastUpdateTime   = FieldValue(t, "lastUpdateTime=");

    // versionCode=31 minSdk=31 targetSdk=31（同一行三个字段）
    {
        const std::string line = FieldValue(t, "versionCode=");
        out->versionCode = strtoll(line.c_str(), nullptr, 10);
        const size_t m = line.find("minSdk=");
        if (m != std::string::npos) out->minSdk = atoi(line.c_str() + m + 7);
        const size_t g = line.find("targetSdk=");
        if (g != std::string::npos) out->targetSdk = atoi(line.c_str() + g + 10);
    }

    {
        const std::string uidLine = FieldValue(t, "userId=");
        if (!uidLine.empty()) out->uid = atoi(uidLine.c_str());
    }

    // flags=[ SYSTEM HAS_CODE ... ]
    {
        const size_t b = t.find("flags=[");
        if (b != std::string::npos) {
            const size_t e = t.find(']', b);
            const std::string flags = t.substr(b + 7, e - b - 7);
            out->system = HasFlag(flags, "SYSTEM");
        }
    }

    // 签名摘要：signatures=PackageSignatures{... signatures:[b4addb29] ...}
    {
        const size_t b = t.find("signatures:[");
        if (b != std::string::npos) {
            const size_t e = t.find(']', b);
            if (e != std::string::npos) {
                out->signatureDigest = t.substr(b + 12, e - b - 12);
            }
        }
    }

    // requested permissions: 段
    //
    // ⚠️ 不能拿 "\n    "（4 空格）当段尾 —— 权限行本身缩进 6 空格，
    //    也包含 4 空格前缀，于是段体会立刻被截成空。
    //    正确做法是按行扫描：遇到缩进变浅（≤4 空格）的非空行才算段结束。
    {
        std::istringstream iss(t);
        std::string line;
        bool inSection = false;
        while (std::getline(iss, line)) {
            if (!inSection) {
                if (line.find("requested permissions:") != std::string::npos) {
                    inSection = true;
                }
                continue;
            }
            if (Trim(line).empty()) continue;

            // 缩进深度
            size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') ++indent;

            const std::string trimmed = Trim(line);
            if (indent <= 4) break;   // 缩进变浅 = 段结束

            if (StartsWith(trimmed, "android.permission.") ||
                StartsWith(trimmed, "com.android")) {
                out->permissions.push_back(trimmed);
            }
        }
    }

    // 组件：resolver table 是按段组织的，逐段抽
    ExtractComponents(Section(t, "Activity Resolver Table:", "Receiver Resolver Table:"),
                      package, &out->activities);
    ExtractComponents(Section(t, "Receiver Resolver Table:", "Service Resolver Table:"),
                      package, &out->receivers);
    ExtractComponents(Section(t, "Service Resolver Table:", "Provider Resolver Table:"),
                      package, &out->services);
    ExtractComponents(Section(t, "Provider Resolver Table:", "Packages:"),
                      package, &out->providers);

    // enabled：没有 "enabled=false" 且没被禁用
    out->enabled = t.find("enabled=false") == std::string::npos;

    return true;
}

// ── Launch ──────────────────────────────────────────────────────────────────
bool AppOps::Launch(const std::string& package, const std::string& activity,
                    std::string* launchedComponent, std::string* error) {
    if (!ValidPackageName(package)) {
        if (error) *error = "非法包名: " + package;
        return false;
    }

    std::string component;
    if (!activity.empty()) {
        component = activity.find('/') != std::string::npos
                        ? activity
                        : package + "/" + ExpandComponent(package, activity);
    } else {
        // 解析默认 Activity：
        //   cmd package resolve-activity --brief <pkg>
        // 输出最后一行形如 "com.foo/.MainActivity"
        CommandResult r;
        if (RunCommand({"/system/bin/cmd", "package", "resolve-activity",
                        "--brief", package},
                       10000, 1u << 20, &r, nullptr) &&
            !r.out.empty()) {
            std::istringstream iss(r.out);
            std::string line;
            while (std::getline(iss, line)) {
                line = Trim(line);
                if (line.find('/') != std::string::npos) component = line;
            }
        }
    }

    std::vector<std::string> argv;
    if (!component.empty()) {
        argv = {"/system/bin/am", "start", "-n", component};
        // 不加 -W：不等启动完成，避免被慢启动的应用拖住
    } else {
        // 退回 monkey 的 LAUNCHER 方式：不需要知道 Activity 名
        argv = {"/system/bin/monkey", "-p", package, "-c",
                "android.intent.category.LAUNCHER", "1"};
    }

    CommandResult r;
    if (!RunCommand(argv, 15000, 1u << 20, &r, nullptr)) {
        if (error) *error = "启动命令执行失败";
        return false;
    }

    const std::string& combined = r.out + r.err;
    // am start 的失败信号：Error / Exception / does not exist
    if (combined.find("Error:") != std::string::npos ||
        combined.find("Exception") != std::string::npos ||
        combined.find("does not exist") != std::string::npos ||
        combined.find("Unable to find") != std::string::npos) {
        if (error) *error = "启动失败: " + Trim(combined.substr(0, 300));
        return false;
    }
    if (r.exitCode != 0) {
        if (error) *error = "启动命令退出码 " + std::to_string(r.exitCode) + ": " +
                            Trim(combined.substr(0, 300));
        return false;
    }

    if (launchedComponent) {
        *launchedComponent = component.empty() ? (package + " (launcher intent)")
                                               : component;
    }
    return true;
}

// ── Kill ────────────────────────────────────────────────────────────────────
bool AppOps::Kill(const std::string& package, std::string* error) {
    if (!ValidPackageName(package)) {
        if (error) *error = "非法包名: " + package;
        return false;
    }
    CommandResult r;
    if (!RunCommand({"/system/bin/am", "force-stop", package}, 10000,
                    1u << 20, &r, nullptr)) {
        if (error) *error = "force-stop 执行失败";
        return false;
    }
    // am force-stop 成功时没有输出；失败会有 Error:
    const std::string combined = r.out + r.err;
    if (combined.find("Error") != std::string::npos ||
        combined.find("Exception") != std::string::npos) {
        if (error) *error = "force-stop 失败: " + Trim(combined.substr(0, 300));
        return false;
    }
    return true;
}

// ── Foreground ──────────────────────────────────────────────────────────────
bool AppOps::Foreground(ForegroundInfo* out, std::string* error) {
    CommandResult r;
    if (!RunCommand({"/system/bin/dumpsys", "activity", "activities"}, 15000,
                    8u << 20, &r, nullptr)) {
        if (error) *error = "dumpsys activity 执行失败";
        return false;
    }

    ForegroundInfo info;
    if (!ParseForeground(r.out, &info, error)) return false;
    *out = info;

    // pid 需要再起一个进程去查，所以放在解析之外
    CommandResult pr;
    if (RunCommand({"/system/bin/pidof", info.package}, 5000, 4096, &pr, nullptr) &&
        pr.exitCode == 0) {
        out->pid = atoi(Trim(pr.out).c_str());
    }
    return true;
}

bool AppOps::ParseForeground(const std::string& dump, ForegroundInfo* out,
                             std::string* error) {
    *out = ForegroundInfo{};

    //   mResumedActivity: ActivityRecord{75e9137 u0 com.foo/.Bar t8}
    //                                  ^hash  ^userId ^component
    const size_t pos = dump.find("mResumedActivity:");
    if (pos == std::string::npos) {
        // 没有 resumed activity（息屏/开机中）不算错误
        if (error) *error = "当前没有前台 Activity（可能息屏或正在开机）";
        return false;
    }
    const size_t eol = dump.find('\n', pos);
    const std::string line = dump.substr(pos, eol - pos);

    const size_t lb = line.find('{');
    const size_t rb = line.find('}', lb);
    if (lb == std::string::npos || rb == std::string::npos) {
        if (error) *error = "无法解析 mResumedActivity 行";
        return false;
    }
    const std::string body = line.substr(lb + 1, rb - lb - 1);

    std::istringstream iss(body);
    std::string hash, userField, compField;
    iss >> hash >> userField >> compField;

    if (compField.empty()) {
        if (error) *error = "mResumedActivity 里没有组件名";
        return false;
    }
    // u0 → userId 0
    if (userField.size() > 1 && userField[0] == 'u') {
        out->userId = atoi(userField.c_str() + 1);
    }

    const size_t slash = compField.find('/');
    if (slash == std::string::npos) {
        out->package = compField;
        out->activity = compField;
    } else {
        out->package  = compField.substr(0, slash);
        out->activity = ExpandComponent(out->package, compField.substr(slash + 1));
    }

    return true;
}

// ── IsRunning ───────────────────────────────────────────────────────────────
bool AppOps::IsRunning(const std::string& package, int32_t* pid) {
    CommandResult pr;
    if (!RunCommand({"/system/bin/pidof", package}, 5000, 4096, &pr, nullptr)) {
        return false;
    }
    const std::string s = Trim(pr.out);
    if (pr.exitCode != 0 || s.empty()) return false;
    if (pid) *pid = atoi(s.c_str());
    return true;
}

// ── Install ─────────────────────────────────────────────────────────────────
bool AppOps::Install(const std::string& apkPath, bool replace,
                     std::string* installedPackage, std::string* error) {
    struct stat st{};
    if (stat(apkPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (error) *error = "APK 不存在或不是普通文件: " + apkPath;
        return false;
    }
    if (st.st_size == 0) {
        if (error) *error = "APK 是空文件";
        return false;
    }

    std::vector<std::string> argv = {"/system/bin/pm", "install"};
    if (replace) argv.push_back("-r");
    argv.push_back(apkPath);

    CommandResult r;
    // 安装可能很慢（dexopt），给足超时
    if (!RunCommand(argv, 180000, 2u << 20, &r, nullptr)) {
        if (error) *error = "pm install 执行失败";
        return false;
    }
    const std::string combined = r.out + r.err;

    if (combined.find("Success") == std::string::npos) {
        if (error) {
            *error = "安装失败: " + Trim(combined.substr(0, 400));
            // 「空间不足」这个报错特别容易把人带偏 —— 它经常不是真的没空间，
            // 而是**之前失败的安装泄漏了空间**（详见 dispatch.cpp 里的预检注释）。
            // 实测失败一次就少一个 APK 大小的可用空间，`df` 却显示还剩很多
            // 因为那份文件被 system_server 持着（`/proc/<pid>/fd` 里能看到
            // 一个 deleted 的 base.apk）。重试几次之后就真的什么都装不上了。
            if (combined.find("INSTALL_FAILED_INSUFFICIENT_STORAGE") !=
                std::string::npos) {
                *error +=
                    "\n【提示】这个错多半不是真的没空间，而是之前**失败的安装**"
                    "占着空间没释放（每次失败留一份 APK 大小，要等下一次"
                    "成功安装或重启才清）。用 df 看可用空间，若明显够，"
                    "重启设备再试。";
            }
        }
        return false;
    }

    // pm install 输出形如：Success
    // 包名得单独查 —— 用 aapt 不可靠，改为比对安装前后的包列表太慢。
    // 这里的做法：解析 APK 的包名由调用方（dispatch）负责，它有 fd 可以读 manifest；
    // 拿不到就留空，客户端可以自己再列一次。
    if (installedPackage) installedPackage->clear();
    return true;
}

}  // namespace autod
