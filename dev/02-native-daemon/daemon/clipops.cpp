// clipops.cpp — 剪贴板访问实现

#include "clipops.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "remote_control_log.h"
#include "json_parser.h"
#include "subprocess.h"

namespace remote_control {
namespace {

// 以 **shell** 身份运行是刻意的，不是随便选的。
// com.android.shell 持有 READ_CLIPBOARD_IN_BACKGROUND（signature 级），
// 而 root + 包名 "android" 会被 ClipboardService 判定为
// "not in focus nor is it a system service" 而拒绝。实测结论。
constexpr int kShellUid = 2000;
constexpr int kShellGid = 2000;

// 辅助工具的可能位置，按优先级
const char* kToolCandidates[] = {
    "/data/local/tmp/cliptool.jar",
    "/system/etc/remote-control/cliptool.jar",
    "/data/misc/remote-control/cliptool.jar",
    nullptr,
};

const char* kAppProcessCandidates[] = {
    "/system/bin/app_process",
    "/system/bin/app_process64",
    nullptr,
};

std::string FindFirst(const char* const* candidates, bool executable) {
    for (int i = 0; candidates[i] != nullptr; ++i) {
        if (access(candidates[i], executable ? X_OK : R_OK) == 0) {
            return candidates[i];
        }
    }
    return {};
}

}  // namespace

ClipOps& ClipOps::Instance() {
    static ClipOps ops;
    return ops;
}

bool ClipOps::Init(std::string* error) {
    if (available_) return true;

    // 环境变量优先：测试和非常规部署都用得上
    const char* envTool = getenv("REMOTE_CONTROL_CLIPTOOL");
    if (envTool != nullptr && *envTool != '\0' && access(envTool, R_OK) == 0) {
        toolPath_ = envTool;
    } else {
        toolPath_ = FindFirst(kToolCandidates, /*executable=*/false);
    }
    if (toolPath_.empty()) {
        if (error) {
            *error = "找不到 cliptool.jar（试过 /data/local/tmp/cliptool.jar 等）。"
                     "把它推上去，或设 REMOTE_CONTROL_CLIPTOOL 指向它";
        }
        return false;
    }

    if (FindFirst(kAppProcessCandidates, /*executable=*/true).empty()) {
        if (error) *error = "找不到 app_process";
        return false;
    }

    available_ = true;
    ALOGI("剪贴板后端就绪：%s（以 shell uid=%d 运行）", toolPath_.c_str(),
          kShellUid);
    return true;
}

bool ClipOps::RunTool(const std::vector<std::string>& args, std::string* out,
                      int* exitCode, std::string* error) {
    if (!available_ && !Init(error)) return false;

    // app_process 需要 CLASSPATH 指向 dex/jar。
    // 用 env(1) 而不是 setenv —— 子进程的过滤在后端是 root，
    // 不该因为一次 exec 就把环境改掉（RunCommandAs 在 fork 之后 exec 之前
    // 能改，但它没有 env 参数；用 env 更简单也更明确）。
    std::vector<std::string> argv = {
        "/system/bin/env",
        "CLASSPATH=" + toolPath_,
        "/system/bin/app_process",
        "/system/bin",
        toolClass_,
    };
    for (const auto& a : args) argv.push_back(a);

    CommandResult r;
    std::string runErr;
    // 降权到 shell：这是剪贴板访问能通过的关键（见文件头）
    if (!RunCommandAs(argv, kShellUid, kShellGid, 15000, 1u << 20, &r, &runErr)) {
        if (error) *error = runErr;
        return false;
    }

    if (exitCode != nullptr) *exitCode = r.exitCode;
    if (out != nullptr) *out = r.out;

    // 后端的 stderr 里有诊断（uid/pkg/op），只在失败时记下来
    if (r.exitCode != 0 && !r.err.empty() && error != nullptr && error->empty()) {
        *error = r.err;
    }
    return true;
}

bool ClipOps::Get(ClipInfo* outInfo, bool* empty, std::string* error) {
    *outInfo = ClipInfo{};
    if (empty != nullptr) *empty = false;

    std::string out;
    int rc = 0;
    std::string err;
    if (!RunTool({"get"}, &out, &rc, &err)) {
        if (error) *error = err.empty() ? "运行剪贴板工具失败" : err;
        return false;
    }

    // 退出码约定见 ClipTool：
    //   0 = 有内容（已写到 stdout）
    //   4 = 剪贴板为空（**不是错误**）
    if (rc == 4) {
        if (empty != nullptr) *empty = true;
        return true;
    }
    if (rc != 0) {
        if (error) {
            *error = "剪贴板工具返回 " + std::to_string(rc) +
                     (err.empty() ? "" : "：" + err);
        }
        return false;
    }

    outInfo->has  = true;
    outInfo->text = out;
    return true;
}

bool ClipOps::Set(const std::string& text, std::string* error) {
    std::string out;
    int rc = 0;
    std::string err;
    if (!RunTool({"set", text}, &out, &rc, &err)) {
        if (error) *error = err.empty() ? "运行剪贴板工具失败" : err;
        return false;
    }

    // 6 = 写进去了但回读为空 —— 多半是 AppOps 拒绝。
    // 之所以要区分，是因为 ClipboardService 在权限不足时**静默 return**，
    // 不靠回读确认就会报"成功"而实际没写进去。
    if (rc == 6) {
        if (error) {
            *error = "写入剪贴板后回读为空 —— ClipboardService 多半拒绝了"
                     "（它的权限检查失败时不抛异常，只是静默返回）";
        }
        return false;
    }
    if (rc != 0) {
        if (error) {
            *error = "剪贴板工具返回 " + std::to_string(rc) +
                     (err.empty() ? "" : "：" + err);
        }
        return false;
    }
    return true;
}

}  // namespace remote_control
