// test_fileops.cpp —— 文件管理的**路径边界**
//
// 为什么单独测这个：`ResolveInside` 是唯一的越界防线，而它有两个时刻
// 特别容易出错 —— 放宽边界的时候（比如支持 /sdcard 存储根），和
// 有人觉得"这个检查太严了"顺手松一下的时候。
//
// 它不碰设备：用真实临时目录 + 真实的软链接来测，
// 所以主机上就能跑，也不依赖 /sdcard。
//
// ⚠️ 只测 ResolveInside 这个静态函数，不测 List/Mkdir 那些 ——
//    那些要真文件系统，而这台构建机上没有 /sdcard。

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <string>

#include "fileops.h"
#include "test_util.h"

using namespace autod;
using autodtest::Check;

namespace {

const char* kRoot = "/tmp/autod-fileops-test/storage";
const char* kDl   = "/tmp/autod-fileops-test/storage/Download";

void RmTree(const std::string& p) {
    std::string cmd = "rm -rf '" + p + "'";
    if (system(cmd.c_str()) != 0) { /* 清理失败不影响结论 */ }
}

// ⚠️ 下面两个包装**先算完再返回**，结果都在返回值里。
//
// 不这么做就会踩 test_util.h 里写明的那个坑：
//     Check(Resolve(x, &out, &err) && out == 想要的值, "...%s", out.c_str())
// 是错的 —— C++ 实参求值顺序未指定，out.c_str() 可能在 Resolve 之前
// 就被求值，于是**断言是对的、打印出来却是乱码**。第一版就是这么写的，
// 结果 18 项全过但每条诊断信息都是垃圾。
struct Resolved {
    bool        ok = false;
    std::string path;
    std::string error;
};

Resolved Resolve(const std::string& input) {
    Resolved r;
    r.ok = FileOps::ResolveInside(kDl, kRoot, input, &r.path, &r.error);
    return r;
}

void TestResolve() {
    printf("\n[1] 相对路径 → 相对下载目录\n");
    Resolved r = Resolve("a.txt");
    Check(r.ok && r.path == std::string(kDl) + "/a.txt", "a.txt → %s",
          r.path.c_str());
    r = Resolve("sub/b.txt");
    Check(r.ok && r.path == std::string(kDl) + "/sub/b.txt", "sub/b.txt → %s",
          r.path.c_str());
    r = Resolve(".");
    Check(r.ok && r.path == kDl, "`.` → 下载目录本身: %s", r.path.c_str());
    r = Resolve("");
    Check(r.ok && r.path == kDl, "空串 → 下载目录本身: %s", r.path.c_str());

    printf("\n[2] 绝对路径：下载目录内\n");
    r = Resolve(std::string(kDl) + "/x.txt");
    Check(r.ok && r.path == std::string(kDl) + "/x.txt",
          "下载目录内的绝对路径: %s", r.path.c_str());

    printf("\n[3] 绝对路径：存储根内（下载目录之外）→ 放行\n");
    // 本次新增的能力：以前这里一律被拒
    r = Resolve(std::string(kRoot) + "/DCIM/p.jpg");
    Check(r.ok && r.path == std::string(kRoot) + "/DCIM/p.jpg",
          "存储根下、下载目录外: %s", r.path.c_str());
    r = Resolve(kRoot);
    Check(r.ok && r.path == kRoot, "存储根本身: %s", r.path.c_str());

    printf("\n[4] 越界必须被拒\n");
    r = Resolve("/data/local/tmp");
    Check(!r.ok, "/data/local/tmp 被拒 → %s", r.error.c_str());
    r = Resolve("/system/bin");
    Check(!r.ok, "/system/bin 被拒 → %s", r.error.c_str());
    r = Resolve("/");
    Check(!r.ok, "/ 被拒 → %s", r.error.c_str());
    r = Resolve("/etc/passwd");
    Check(!r.ok, "/etc/passwd 被拒 → %s", r.error.c_str());
    // 前缀相似但不是子目录
    r = Resolve(std::string(kRoot) + "-evil/x");
    Check(!r.ok, "同前缀不同目录（storage-evil）被拒 → %s", r.error.c_str());

    printf("\n[5] `..` 逃逸\n");
    r = Resolve("../../etc");
    Check(!r.ok, "相对 ../../etc 被拒 → %s", r.error.c_str());
    r = Resolve("..");
    Check(!r.ok, "相对 .. 被拒 → %s", r.error.c_str());
    r = Resolve(std::string(kRoot) + "/../outside");
    Check(!r.ok, "绝对 .../storage/../outside 被拒 → %s", r.error.c_str());
    // 但"退到存储根又回来"是合法的
    r = Resolve("sub/../a.txt");
    Check(r.ok && r.path == std::string(kDl) + "/a.txt",
          "sub/../a.txt 规范化后仍在范围内: %s", r.path.c_str());
}

void TestSymlink() {
    printf("\n[6] 软链接逃逸\n");
    const std::string link = std::string(kRoot) + "/escape";
    RmTree(link);
    if (symlink("/etc", link.c_str()) != 0) {
        printf("  （建不了软链接，跳过）\n");
        return;
    }
    Resolved r = Resolve(link + "/passwd");
    Check(!r.ok, "穿过指向 /etc 的软链接被拒 → %s", r.error.c_str());

    const std::string inner = std::string(kRoot) + "/inner";
    RmTree(inner);
    if (symlink(kDl, inner.c_str()) == 0) {
        r = Resolve(inner + "/ok.txt");
        Check(r.ok, "指向范围内目标的软链接放行: %s", r.path.c_str());
    }
}

}  // namespace

int main() {
    printf("文件管理路径边界测试\n");

    RmTree("/tmp/autod-fileops-test");
    if (mkdir("/tmp/autod-fileops-test", 0755) != 0) { /* 可能已存在 */ }
    if (mkdir(kRoot, 0755) != 0) { /* 可能已存在 */ }
    if (mkdir(kDl, 0755) != 0) { /* 可能已存在 */ }

    TestResolve();
    TestSymlink();

    RmTree("/tmp/autod-fileops-test");

    printf("\n\033[1;32m全部通过\033[0m  (%d 项检查)\n", autodtest::gChecks);
    if (autodtest::gFailed > 0) {
        printf("\033[1;31m%d / %d 项失败\033[0m\n", autodtest::gFailed,
               autodtest::gChecks);
        return 1;
    }
    return 0;
}
