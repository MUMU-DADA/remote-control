// appops.h — 应用管理后端
//
// 通过 exec 系统命令实现（pm / am / cmd / dumpsys），和 screencap 后端同样的理由：
//   - 不需要 JNI/Java 运行时，能同时编进 AOSP（Soong）和 NDK 两种环境
//   - 行为与 `adb shell` 下看到的一致，出问题好对照
//
// 分工会明确：**标签和图标不在这里取**。
//   `pm list packages` 给不出应用标签，逐个 `dumpsys package` 对 100+ 应用太慢；
//   而上位应用（Java）一个 PackageManager.getApplicationLabel() 就有了，还带图标。
//   所以 daemon 只返回包名和廉价元数据，展示层交给上位应用。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace autod {

struct AppEntry {
    std::string package;
    std::string apkPath;       // withMetadata 时有效
    std::string installer;     // withMetadata 时有效
    int64_t     versionCode = 0;
    int32_t     uid         = -1;
    bool        system      = false;
};

// 应用清单里的结构化信息（AppInfo 命令的返回）
struct AppDetail {
    std::string package;
    std::string versionName;
    int64_t     versionCode = 0;
    int32_t     uid         = -1;
    int32_t     minSdk      = 0;
    int32_t     targetSdk   = 0;
    std::string apkPath;
    std::string dataDir;
    std::string installer;
    bool        system      = false;
    bool        enabled     = true;
    std::string firstInstallTime;
    std::string lastUpdateTime;
    std::vector<std::string> permissions;    // requested permissions
    std::vector<std::string> activities;     // 已声明的 Activity 全名
    std::vector<std::string> services;
    std::vector<std::string> receivers;
    std::vector<std::string> providers;
    std::string signatureDigest;             // 签名摘要（用于判断是否同一份签名）
};

struct ForegroundInfo {
    std::string package;
    std::string activity;
    int32_t     pid = -1;
    int32_t     userId = 0;
};

class AppOps {
public:
    // 检查依赖的命令在不在。缺哪个会写进 error。
    bool Init(std::string* error);

    // pm list packages [-3] [--show-versioncode] [-f] [-i]
    bool ListApps(bool includeSystem, bool withMetadata,
                  std::vector<AppEntry>* out, std::string* error);

    // dumpsys package <pkg>
    bool Detail(const std::string& package, AppDetail* out, std::string* error);

    // 纯解析版本（不跑命令）—— 主机测试用。
    // 解析逻辑最容易出错（缩进、段边界、字段同行），而它不依赖设备，
    // 抽出来就能拿真实 dumpsys 输出做回归。
    static bool ParseDetail(const std::string& dump, const std::string& package,
                            AppDetail* out, std::string* error);

    // cmd package resolve-activity --brief <pkg> 拿默认 Activity，
    // 再 am start -n <component> 启动。
    // activity 为空时自动解析；解析不到就退回 monkey 的 LAUNCHER 方式。
    bool Launch(const std::string& package, const std::string& activity,
                std::string* launchedComponent, std::string* error);

    // am force-stop <pkg>
    bool Kill(const std::string& package, std::string* error);

    // dumpsys activity activities 里的 mResumedActivity
    bool Foreground(ForegroundInfo* out, std::string* error);

    // 纯解析版本（不跑命令）
    static bool ParseForeground(const std::string& dump, ForegroundInfo* out,
                                std::string* error);

    // pm install [-r] <path>
    // 从输出里解析出 "Success" 与包名。
    bool Install(const std::string& apkPath, bool replace,
                 std::string* installedPackage, std::string* error);

    // 判断包名是否合法（防注入的第二道闸；第一道是不走 shell）
    static bool ValidPackageName(const std::string& p);

    // 某个包当前是否在运行（pid 查询，用于 Kill 后确认）
    bool IsRunning(const std::string& package, int32_t* pid);

  private:
    // Init 会被反复调用（Describe 逐命令判可用性），日志只打一次
    bool loggedOnce_ = false;
};

}  // namespace autod
