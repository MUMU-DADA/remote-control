// Host-side tests for the version-slot REST update contract.

#include "rest_api.h"
#include "sha256.h"
#include "test_util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

using namespace remote_control;
using remote_control_test::Check;

namespace {

std::string TempDir() {
    char path[] = "/tmp/remote-control-update-test-XXXXXX";
    char* result = mkdtemp(path);
    return result == nullptr ? std::string() : std::string(result);
}

HttpRequest UploadRequest(const std::string& body, const std::string& sha,
                          const char* contentType = "application/octet-stream") {
    HttpRequest req;
    req.method = "POST";
    req.path = "/api/v1/update";
    req.body = body;
    req.bodySize = body.size();
    req.headers.emplace_back("content-type", contentType);
    if (!sha.empty()) req.headers.emplace_back("x-remote-control-sha256", sha);
    return req;
}

HttpRequest JsonRequest(const char* path, const std::string& body = {}) {
    HttpRequest req;
    req.method = "POST";
    req.path = path;
    req.body = body;
    req.bodySize = body.size();
    req.headers.emplace_back("content-type", "application/json");
    return req;
}

void RemoveTree(const std::string& root) {
    const std::string releases = root + "/releases";
    if (DIR* dir = opendir(releases.c_str())) {
        while (dirent* ent = readdir(dir)) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            const std::string file = releases + "/" + ent->d_name + "/remote-control";
            unlink(file.c_str());
            rmdir((releases + "/" + ent->d_name).c_str());
        }
        closedir(dir);
    }
    rmdir(releases.c_str());
    unlink((root + "/current").c_str());
    unlink((root + "/previous").c_str());
    unlink((root + "/last-good").c_str());
    rmdir(root.c_str());
}

void TestUpdateUploadAndApply() {
    printf("\n\033[1;34m[1] REST 更新上传、应用与回滚\033[0m\n");
    const std::string root = TempDir();
    if (root.empty()) {
        Check(false, "创建更新测试目录");
        return;
    }
    // ELF magic is intentionally sufficient for this host contract test; a
    // real device additionally validates that the selected ABI can execute.
    const std::string payload = std::string("\x7f", 1) +
            "ELF\x02\x01\x01\0host-test";
    const std::string sha = Sha256Hex(payload.data(), payload.size());
    setenv("REMOTE_CONTROL_UPDATE_SUPPORTED", "1", 1);

    RestApi api(nullptr, root);
    std::atomic<int> restarts{0};
    api.SetRestartHook([&]() { ++restarts; });

    HttpResponse upload = api.Handle(UploadRequest(payload, sha));
    Check(upload.status == 201 && upload.body.find("\"staged\":true") != std::string::npos,
          "ELF 上传成功后仅暂存（HTTP %d）", upload.status);

    HttpRequest status;
    status.method = "GET";
    status.path = "/api/v1/update";
    HttpResponse listed = api.Handle(status);
    Check(listed.status == 200 && listed.body.find(sha) != std::string::npos,
          "状态接口列出已暂存 SHA-256");

    HttpResponse apply = api.Handle(JsonRequest("/api/v1/update/apply",
                                                "{\"sha256\":\"" + sha + "\"}"));
    Check(apply.status == 202,
          "apply 原子切换并返回 202（HTTP %d）", apply.status);
    Check(access((root + "/current").c_str(), F_OK) == 0,
          "apply 创建 current 指针");
    if (apply.onComplete) apply.onComplete();
    api.JoinRestartThread();
    Check(restarts.load() == 1, "响应完成后触发一次 restart hook（实际 %d）",
          restarts.load());

    // apply schedules process exit; a real rollback request arrives after the
    // supervisor starts a fresh daemon instance, so use a new API object here.
    RestApi apiAfterRestart(nullptr, root);
    apiAfterRestart.SetRestartHook([&]() { ++restarts; });
    HttpResponse rollback = apiAfterRestart.Handle(JsonRequest("/api/v1/update/rollback"));
    Check(rollback.status == 202 && rollback.body.find("rollback") != std::string::npos,
          "rollback 返回 202");
    if (rollback.onComplete) rollback.onComplete();
    apiAfterRestart.JoinRestartThread();
    Check(restarts.load() == 2, "rollback 响应完成后触发 restart hook");

    RemoveTree(root);
    unsetenv("REMOTE_CONTROL_UPDATE_SUPPORTED");
}

void TestUpdateRejectsInvalidInput() {
    printf("\n\033[1;34m[2] REST 更新输入校验\033[0m\n");
    const std::string root = TempDir();
    if (root.empty()) {
        Check(false, "创建校验测试目录");
        return;
    }
    RestApi api(nullptr, root);
    HttpResponse missingType = api.Handle(UploadRequest("bad", "", ""));
    Check(missingType.status == 415, "缺少有效 Content-Type 返回 415");
    HttpResponse notElf = api.Handle(UploadRequest("not-elf", ""));
    Check(notElf.status == 400, "非 ELF 上传返回 400");
    setenv("REMOTE_CONTROL_UPDATE_SUPPORTED", "0", 1);
    HttpResponse apply = api.Handle(JsonRequest("/api/v1/update/apply",
                                                "{\"sha256\":\"" +
                                                std::string(64, '0') + "\"}"));
    Check(apply.status == 503, "未由 launcher 管理时拒绝 apply（503）");
    unsetenv("REMOTE_CONTROL_UPDATE_SUPPORTED");
    RemoveTree(root);
}

}  // namespace

int main() {
    printf("\033[1m=== REST update regression tests ===\033[0m\n");
    TestUpdateUploadAndApply();
    TestUpdateRejectsInvalidInput();
    return remote_control_test::Summary("REST 更新");
}
