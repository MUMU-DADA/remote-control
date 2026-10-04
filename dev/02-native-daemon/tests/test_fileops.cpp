// test_fileops.cpp —— 文件管理的**路径边界**
//
// 为什么单独测这个：`ResolveInside` 是唯一的越界防线，而它有两个时刻
// 特别容易出错 —— 放宽边界的时候（比如支持 /sdcard 存储根），和
// 有人觉得"这个检查太严了"顺手松一下的时候。
//
// 它不碰设备：用真实临时目录 + 真实的软链接来测，
// 所以主机上就能跑，也不依赖 /sdcard。
//
// ResolveInside 和上传原子落盘使用显式临时根目录测试；不依赖设备上的 /sdcard。

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

#include "fileops.h"
#include "test_util.h"

using namespace remote_control;
using remote_control_test::Check;

namespace {

const char* kRoot = "/tmp/remote-control-fileops-test/storage";
const char* kDl   = "/tmp/remote-control-fileops-test/storage/Download";

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
    std::string nulEscape = std::string(kRoot) + "/Download/..";
    nulEscape.push_back('\0');
    nulEscape += "a/..";
    nulEscape.push_back('\0');
    nulEscape += "b";
    r = Resolve(nulEscape);
    Check(!r.ok, "含 NUL 的路径不能借系统调用截断绕过边界");
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

bool Upload(const std::string& name, const std::string& data,
            std::string* path, std::string* error,
            const std::string& directory = kDl) {
    return FileOps::UploadToStorage(kDl, kRoot, directory, name, -1,
                                    data.data(), data.size(), path, error);
}

std::string ReadFile(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string data;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        data.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return data;
}

void TestUpload() {
    printf("\n[7] 文件上传与原子落盘\n");
    std::string path, error;
    const std::string staleTemp =
            std::string(kDl) + "/.remote-control-upload-2147483647-0";
    const int staleFd = open(staleTemp.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                             0600);
    if (staleFd >= 0) close(staleFd);
    const std::string payload("hello\0world", 11);
    const bool uploaded = Upload("hello.bin", payload, &path, &error);
    Check(uploaded && path == std::string(kDl) + "/hello.bin" &&
              ReadFile(path) == payload,
          "二进制内容按原字节写入并返回绝对路径: %s", error.c_str());
    Check(staleFd >= 0 && access(staleTemp.c_str(), F_OK) != 0,
          "上传前回收已退出进程遗留的目标临时文件");

    error.clear();
    const bool duplicate = Upload("hello.bin", "replacement", &path, &error);
    Check(!duplicate && error.rfind("目标已存在:", 0) == 0 &&
              ReadFile(std::string(kDl) + "/hello.bin") == payload,
          "同名上传拒绝覆盖且保留旧内容");

    error.clear();
    const bool empty = Upload("empty.dat", "", &path, &error);
    struct stat emptyStat{};
    const bool emptyFile = stat((std::string(kDl) + "/empty.dat").c_str(),
                                &emptyStat) == 0 && emptyStat.st_size == 0;
    Check(empty && emptyFile, "零字节文件可正常上传");

    for (const std::string& invalid : {std::string(""), std::string("."),
                                        std::string(".."), std::string("a/b"),
                                        std::string("a\\b"),
                                        std::string("bad\nname")}) {
        error.clear();
        const bool ok = Upload(invalid, "x", &path, &error);
        Check(!ok && error.rfind("文件名", 0) == 0,
              "拒绝非 basename 或控制字符文件名");
    }

    error.clear();
    const bool outside = Upload("escape.bin", "x", &path, &error,
                                "/tmp/remote-control-fileops-test/outside");
    Check(!outside, "目标目录超出共享存储根时拒绝");

    std::string nulEscape = std::string(kRoot) + "/Download/..";
    nulEscape.push_back('\0');
    nulEscape += "a/..";
    nulEscape.push_back('\0');
    nulEscape += "b";
    const std::string escapedFile = "/tmp/remote-control-fileops-test/escape.bin";
    unlink(escapedFile.c_str());
    error.clear();
    const bool escaped = Upload("escape.bin", "x", &path, &error, nulEscape);
    Check(!escaped && access(escapedFile.c_str(), F_OK) != 0,
          "含 NUL 的多层 .. 上传路径不能越出共享存储");

    const std::string link = std::string(kRoot) + "/upload-link";
    unlink(link.c_str());
    if (symlink(kDl, link.c_str()) == 0) {
        error.clear();
        const bool throughLink = Upload("linked.bin", "x", &path, &error,
                                        link);
        Check(!throughLink, "上传目标目录含软链接时拒绝");
    }

    char spoolPath[] = "/tmp/remote-control-upload-spool-XXXXXX";
    const int spoolFd = mkstemp(spoolPath);
    bool spoolReady = false;
    if (spoolFd >= 0) {
        const std::string spoolData(180000, 's');
        size_t offset = 0;
        while (offset < spoolData.size()) {
            const ssize_t n = write(spoolFd, spoolData.data() + offset,
                                    spoolData.size() - offset);
            if (n <= 0) break;
            offset += static_cast<size_t>(n);
        }
        spoolReady = offset == spoolData.size();
        error.clear();
        const bool copied = spoolReady &&
            FileOps::UploadToStorage(kDl, kRoot, kDl, "spooled.bin", spoolFd,
                                     nullptr, spoolData.size(), &path, &error);
        Check(copied && ReadFile(path) == spoolData,
              "大正文从 spool fd 流式复制到目标目录: %s", error.c_str());
        error.clear();
        const bool shortSource = spoolReady &&
            FileOps::UploadToStorage(kDl, kRoot, kDl, "short.bin", spoolFd,
                                     nullptr, 1, &path, &error);
        Check(!shortSource, "spool 正文长度不符时拒绝发布");
        close(spoolFd);
        unlink(spoolPath);
    } else {
        Check(false, "创建 spool 测试文件: %s", strerror(errno));
    }

    bool first = false, second = false;
    std::string firstPath, secondPath;
    std::string firstError, secondError;
    const std::string firstData(65536, 'a');
    const std::string secondData(65536, 'b');
    std::thread a([&] {
        first = Upload("raced.bin", firstData, &firstPath, &firstError);
    });
    std::thread b([&] {
        second = Upload("raced.bin", secondData, &secondPath, &secondError);
    });
    a.join();
    b.join();
    const std::string racedData = ReadFile(std::string(kDl) + "/raced.bin");
    Check(first != second &&
              (racedData == firstData || racedData == secondData),
          "并发同名上传只发布一个完整文件");
}

}  // namespace

int main() {
    printf("文件管理路径边界测试\n");

    RmTree("/tmp/remote-control-fileops-test");
    if (mkdir("/tmp/remote-control-fileops-test", 0755) != 0) { /* 可能已存在 */ }
    if (mkdir(kRoot, 0755) != 0) { /* 可能已存在 */ }
    if (mkdir(kDl, 0755) != 0) { /* 可能已存在 */ }

    TestResolve();
    TestSymlink();
    TestUpload();

    RmTree("/tmp/remote-control-fileops-test");

    // 用公共 Summary()，不要自己拼 —— 这里原来就是手写的，于是踩了两个坑：
    //   ① gChecks 没被上报给 `make run` 的总数核对（少一套，README 对不上）；
    //   ② "全部通过" 打在了 gFailed 判断**之前** —— 有失败时会先打"全部通过"
    //      再打失败行，自相矛盾。
    return remote_control_test::Summary("文件路径边界");
}
