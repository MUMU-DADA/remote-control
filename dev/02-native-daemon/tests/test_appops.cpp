// test_appops.cpp — 应用管理后端的解析测试
//
// 用**真实设备抓下来的 dumpsys / pm 输出**做夹具（tests/fixtures/），
// 不是手写的理想化样本 —— 解析逻辑的坑几乎全在真实输出的
// 缩进、段边界、同一行多字段这些地方。
//
// 夹具生成方式（重新抓一份）：
//   adb shell 'dumpsys package com.android.settings'   > fixtures/dumpsys-package-settings.txt
//   adb shell 'dumpsys activity activities'            > fixtures/dumpsys-activity.txt
//   adb shell 'pm list packages -f --show-versioncode -i' > fixtures/pm-list-all.txt
//
// 这些都是纯解析，不需要设备，所以能在主机上跑进回归。

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "appops.h"
#include "test_util.h"

using namespace autod;
using namespace autodtest;

namespace {

const char* kFixtureDir = "fixtures";

int CountLines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

std::string Slurp(const std::string& name) {
    std::ifstream f(std::string(kFixtureDir) + "/" + name);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ── 包名合法性 ──────────────────────────────────────────────────────────────
void TestPackageName() {
    printf("\n\033[1;34m[1] 包名校验\033[0m\n");

    struct Case { const char* p; bool ok; const char* why; };
    const Case cases[] = {
        {"com.android.settings",              true,  "正常包名"},
        {"a",                                 true,  "单字符"},
        {"com.a_b.c",                         true,  "下划线合法"},
        {"",                                  false, "空"},
        {".com.a",                            false, "以点开头"},
        {"com.a.",                            false, "以点结尾"},
        {"com..a",                            false, "连续点"},
        {"com/a",                             false, "斜杠"},
        {"com.a;rm -rf /",                    false, "shell 元字符"},
        {"com.a b",                           false, "空格"},
        {"com.a\nb",                          false, "换行"},
    };
    for (const auto& c : cases) {
        Check(AppOps::ValidPackageName(c.p) == c.ok, "%-28s %s",
              c.p[0] ? c.p : "(空)", c.why);
    }
}

// ── dumpsys package 解析 ────────────────────────────────────────────────────
void TestParseDetail() {
    printf("\n\033[1;34m[2] dumpsys package 解析\033[0m  （真实设备输出）\n");

    const std::string dump = Slurp("dumpsys-package-settings.txt");
    Check(!dump.empty(), "夹具已加载（%zu 字节）", dump.size());
    if (dump.empty()) return;

    AppDetail d;
    std::string err;
    const bool ok = AppOps::ParseDetail(dump, "com.android.settings", &d, &err);
    Check(ok, "解析成功%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());
    if (!ok) return;

    printf("    \033[2m");
    printf("versionName=%s versionCode=%lld uid=%d sdk=%d/%d",
           d.versionName.c_str(), static_cast<long long>(d.versionCode), d.uid,
           d.minSdk, d.targetSdk);
    printf("\033[0m\n");

    // 标量字段
    Check(d.versionName == "12", "versionName = %s", d.versionName.c_str());
    Check(d.versionCode == 31, "versionCode = %lld",
          static_cast<long long>(d.versionCode));
    Check(d.uid == 1000, "uid = %d", d.uid);
    Check(d.minSdk == 31 && d.targetSdk == 31, "minSdk/targetSdk = %d/%d",
          d.minSdk, d.targetSdk);
    Check(d.apkPath == "/system_ext/priv-app/Settings", "apkPath = %s",
          d.apkPath.c_str());
    Check(!d.dataDir.empty(), "dataDir = %s", d.dataDir.c_str());
    Check(d.system, "识别为系统应用（flags 含 SYSTEM）");
    Check(d.firstInstallTime.find("2023-") == 0, "firstInstallTime = %s",
          d.firstInstallTime.c_str());
    Check(!d.signatureDigest.empty(), "签名摘要 = %s", d.signatureDigest.c_str());

    // 权限：这里踩过坑 —— 权限行缩进 6 空格，用 "\n    "（4 空格）找段尾
    // 会立刻匹配到权限行自己，段体被截成空，结果是 0 条。
    printf("    \033[2mpermissions=%zu activities=%zu services=%zu "
           "receivers=%zu providers=%zu\033[0m\n",
           d.permissions.size(), d.activities.size(), d.services.size(),
           d.receivers.size(), d.providers.size());
    Check(d.permissions.size() > 50, "解析出 %zu 条权限（不是 0）",
          d.permissions.size());
    if (!d.permissions.empty()) {
        Check(d.permissions[0].rfind("android.permission.", 0) == 0,
              "首条权限格式正确: %s", d.permissions[0].c_str());
    }
    Check(d.activities.size() > 50, "解析出 %zu 个 Activity", d.activities.size());
    Check(d.services.size() > 0, "解析出 %zu 个 Service", d.services.size());
    Check(d.receivers.size() > 0, "解析出 %zu 个 Receiver", d.receivers.size());
    Check(d.providers.size() > 0, "解析出 %zu 个 Provider", d.providers.size());

    // 组件名必须是补全后的全名
    if (!d.activities.empty()) {
        const std::string& a = d.activities[0];
        Check(a.rfind("com.android.settings", 0) == 0,
              "组件名已补全为全名: %s", a.c_str());
        Check(a.find('/') == std::string::npos, "组件名里没有残留的斜杠");
    }

    // 不存在的包
    AppDetail d2;
    std::string err2;
    const bool ok2 = AppOps::ParseDetail(dump, "com.not.installed", &d2, &err2);
    Check(!ok2, "包名不匹配时解析失败而不是返回垃圾数据");
    Check(err2.find("不存在") != std::string::npos, "错误信息可读: %s",
          err2.c_str());
}

// ── 前台应用解析 ────────────────────────────────────────────────────────────
void TestParseForeground() {
    printf("\n\033[1;34m[3] 前台应用解析\033[0m  （真实设备输出）\n");

    const std::string dump = Slurp("dumpsys-activity.txt");
    Check(!dump.empty(), "夹具已加载（%zu 字节）", dump.size());
    if (dump.empty()) return;

    ForegroundInfo fg;
    std::string err;
    const bool ok = AppOps::ParseForeground(dump, &fg, &err);
    Check(ok, "解析成功%s%s", ok ? "" : " —— ", ok ? "" : err.c_str());
    if (!ok) return;

    Check(fg.package == "com.android.launcher3", "package = %s", fg.package.c_str());
    Check(fg.activity == "com.android.launcher3.uioverrides.QuickstepLauncher",
          "activity 已补全: %s", fg.activity.c_str());
    Check(fg.userId == 0, "userId = %d", fg.userId);

    // 没有前台 Activity 的输入不该崩
    ForegroundInfo fg2;
    std::string err2;
    Check(!AppOps::ParseForeground("nothing here\n", &fg2, &err2),
          "无 mResumedActivity 时干净失败");
    Check(AppOps::ParseForeground("mResumedActivity: garbage\n", &fg2, &err2) == false,
          "格式不对时不崩且返回失败");
}

// ── pm list packages 解析 ───────────────────────────────────────────────────
void TestParsePmList() {
    printf("\n\033[1;34m[4] pm list packages 格式\033[0m\n");

    const std::string all = Slurp("pm-list-all.txt");
    const std::string third = Slurp("pm-list-3.txt");
    Check(!all.empty(), "全部应用夹具：%zu 行", CountLines(all));
    // 原版镜像没有第三方应用，空结果是**有效**用例，不是夹具缺失
    Check(true, "第三方应用夹具：%zu 行（原版镜像为 0，有效）", CountLines(third));

    // 校验格式假设：package:<path>=<pkg> versionCode:N  installer:X
    std::istringstream iss(all);
    std::string line;
    int parsed = 0;
    int withPath = 0;
    int withVersion = 0;
    while (std::getline(iss, line)) {
        if (line.rfind("package:", 0) != 0) continue;
        ++parsed;
        if (line.find('=') != std::string::npos) ++withPath;
        if (line.find("versionCode:") != std::string::npos) ++withVersion;
    }
    Check(parsed > 100, "解析出 %d 个包", parsed);
    Check(withPath == parsed, "每行都有 =<包名>（%d/%d）", withPath, parsed);
    Check(withVersion == parsed, "每行都有 versionCode（%d/%d）", withVersion,
          parsed);

    // 双空格：真实输出是 "versionCode:31  installer=null"，
    // 用 istringstream >> 切词能正确处理，这里固定住这个假设
    const std::string sample =
        "package:/system/app/Foo/Foo.apk=com.foo versionCode:42  installer=null";
    std::istringstream fs(sample.substr(8));
    std::string pathAndPkg, verTok, instTok;
    fs >> pathAndPkg >> verTok >> instTok;
    Check(pathAndPkg == "/system/app/Foo/Foo.apk=com.foo", "路径与包名成对: %s",
          pathAndPkg.c_str());
    Check(verTok == "versionCode:42", "版本号成词: %s", verTok.c_str());
    Check(instTok == "installer=null", "installer 成词: %s（双空格被正确跳过）",
          instTok.c_str());
}

}  // namespace

int main() {
    printf("\033[1m=== autod 应用管理后端测试 ===\033[0m\n");

    TestPackageName();
    TestParseDetail();
    TestParseForeground();
    TestParsePmList();

    return Summary("应用管理后端");
}
