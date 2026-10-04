// fileops.cpp — 下载目录与文件操作

#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include "remote_control_log.h"
#include "subprocess.h"

#if !defined(SYS_renameat2) && defined(__NR_renameat2)
#define SYS_renameat2 __NR_renameat2
#endif

namespace remote_control {
namespace {

constexpr int64_t kDefaultMaxDownload = 512LL << 20;   // 512 MB
constexpr int     kDefaultTimeoutSec  = 300;
constexpr size_t  kMaxPathComponents  = 256;
constexpr size_t  kMaxDeleteDepth     = 256;

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string DirName(const std::string& p) {
    const size_t s = p.rfind('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return p.substr(0, s);
}

// path 是不是 base（或 base 自己）。两边都要求无尾斜杠。
bool IsUnder(const std::string& path, const std::string& base) {
    if (base.empty()) return false;
    if (path == base) return true;
    return path.size() > base.size() &&
           path.compare(0, base.size(), base) == 0 &&
           path[base.size()] == '/';
}

// 相对 root 的路径表示；不在 root 内返回空
std::string RelativeTo(const std::string& root, const std::string& abs) {
    if (abs == root) return ".";
    if (abs.size() > root.size() + 1 && abs.compare(0, root.size(), root) == 0 &&
        abs[root.size()] == '/') {
        return abs.substr(root.size() + 1);
    }
    return {};
}

// The old implementation validated a path with realpath(), then used the
// path string in fopen/rename/rmdir.  A different process could replace a
// checked parent directory with a symlink in between those operations.  All
// mutating operations below therefore walk from an already-open directory fd;
// O_NOFOLLOW rejects symlink components and the final operation is *at(2).
bool RelativeParts(const std::string& base, const std::string& abs,
                   std::vector<std::string>* parts) {
    parts->clear();
    if (abs == base) return true;
    if (!IsUnder(abs, base)) return false;
    const std::string rel = abs.substr(base.size() + 1);
    size_t i = 0;
    while (i < rel.size()) {
        size_t slash = rel.find('/', i);
        if (slash == std::string::npos) slash = rel.size();
        const std::string part = rel.substr(i, slash - i);
        if (part.empty() || part == "." || part == "..") return false;
        parts->push_back(part);
        if (parts->size() > kMaxPathComponents) return false;
        i = slash + 1;
    }
    return true;
}

int OpenDirNoFollow(const std::string& path) {
    if (path.empty() || path[0] != '/') return -1;
    // Ancestors only anchor openat; O_PATH needs traversal permission without
    // asking to list rootfs or other directories outside the allowed storage.
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t i = 1;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        if (slash == i) {
            i = slash + 1;
            continue;
        }
        const std::string part = path.substr(i, slash - i);
        const int next = openat(fd, part.c_str(),
                                O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            close(fd);
            return -1;
        }
        close(fd);
        fd = next;
        i = slash + 1;
    }
    // fdopendir and the final storage operation need a readable directory.
    const int readable = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int savedErrno = errno;
    close(fd);
    errno = savedErrno;
    return readable;
}

int OpenPathDirNoFollow(const std::string& base, const std::string& abs,
                        std::string* error) {
    std::vector<std::string> parts;
    if (!RelativeParts(base, abs, &parts)) {
        if (error) *error = "路径不在允许目录内: " + abs;
        return -1;
    }
    int fd = OpenDirNoFollow(base);
    if (fd < 0) {
        if (error) *error = "打开目录失败: " + std::string(strerror(errno));
        return -1;
    }
    for (const std::string& part : parts) {
        const int next = openat(fd, part.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = "打开目录 " + part + " 失败: " + strerror(savedErrno);
            return -1;
        }
        close(fd);
        fd = next;
    }
    return fd;
}

int OpenParentNoFollow(const std::string& base, const std::string& abs,
                       std::string* leaf, std::string* error) {
    std::vector<std::string> parts;
    if (!RelativeParts(base, abs, &parts) || parts.empty()) {
        if (error) *error = "目标必须是允许目录中的子项: " + abs;
        return -1;
    }
    *leaf = parts.back();
    parts.pop_back();
    int fd = OpenDirNoFollow(base);
    if (fd < 0) {
        if (error) *error = "打开目录失败: " + std::string(strerror(errno));
        return -1;
    }
    for (const std::string& part : parts) {
        const int next = openat(fd, part.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = "打开目录 " + part + " 失败: " + strerror(savedErrno);
            return -1;
        }
        close(fd);
        fd = next;
    }
    return fd;
}

bool MkdirPAt(const std::string& base, const std::string& abs,
              std::string* error) {
    std::vector<std::string> parts;
    if (!RelativeParts(base, abs, &parts)) {
        if (error) *error = "路径不在允许目录内: " + abs;
        return false;
    }
    int fd = OpenDirNoFollow(base);
    if (fd < 0) {
        if (error) *error = "打开目录失败: " + std::string(strerror(errno));
        return false;
    }
    for (const std::string& part : parts) {
        if (mkdirat(fd, part.c_str(), 0775) != 0 && errno != EEXIST) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = "mkdir " + part + " 失败: " + strerror(savedErrno);
            return false;
        }
        struct stat st{};
        if (fstatat(fd, part.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode)) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = part + " 已存在且不是目录" +
                                  (savedErrno ? (": " + std::string(strerror(savedErrno))) : "");
            return false;
        }
        const int next = openat(fd, part.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            const int savedErrno = errno;
            close(fd);
            if (error) *error = "打开目录 " + part + " 失败: " + strerror(savedErrno);
            return false;
        }
        close(fd);
        fd = next;
    }
    close(fd);
    return true;
}

bool RemoveTreeAt(int parentFd, const std::string& name, std::string* error,
                  size_t depth = 0) {
    if (depth > kMaxDeleteDepth) {
        if (error) *error = "目录层级超过上限";
        return false;
    }
    struct stat st{};
    if (fstatat(parentFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (error) *error = "路径不存在: " + name;
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlinkat(parentFd, name.c_str(), 0) != 0) {
            if (error) *error = "删除 " + name + " 失败: " + strerror(errno);
            return false;
        }
        return true;
    }

    const int childFd = openat(parentFd, name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (childFd < 0) {
        if (error) *error = "打开目录 " + name + " 失败: " + strerror(errno);
        return false;
    }
    DIR* dir = fdopendir(childFd);
    if (dir == nullptr) {
        close(childFd);
        if (error) *error = "打开目录 " + name + " 失败: " + strerror(errno);
        return false;
    }
    bool ok = true;
    errno = 0;
    while (ok) {
        dirent* ent = readdir(dir);
        if (ent == nullptr) break;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        ok = RemoveTreeAt(dirfd(dir), ent->d_name, error, depth + 1);
    }
    const int readErrno = errno;
    closedir(dir);
    if (!ok) return false;
    if (readErrno != 0) {
        if (error) *error = "读取目录 " + name + " 失败: " + strerror(readErrno);
        return false;
    }
    if (unlinkat(parentFd, name.c_str(), AT_REMOVEDIR) != 0) {
        if (error) *error = "删除目录 " + name + " 失败: " + strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

// ── 路径约束 ────────────────────────────────────────────────────────────────
std::string FileOps::NormalizeAlias(const std::string& input) const {
    for (const auto& a : aliases_) {
        if (input == a) return storageRoot_;
        if (input.size() > a.size() &&
            input.compare(0, a.size(), a) == 0 && input[a.size()] == '/') {
            return storageRoot_ + input.substr(a.size());
        }
    }
    return input;
}

bool FileOps::ResolveInside(const std::string& root,
                            const std::string& storageRoot,
                            const std::string& input,
                            std::string* out, std::string* error) {
    if (out == nullptr) return false;
    if (root.empty() || root[0] != '/') {
        if (error) *error = "下载目录未初始化";
        return false;
    }
    // 边界：有存储根就用存储根，没有就退回下载目录（功能变窄但不会变宽）
    const std::string& bound = storageRoot.empty() ? root : storageRoot;

    std::string rest = input;
    // full 是已经拼好的绝对前缀；相对路径从 root 起，绝对路径从边界起
    std::string base = root;

    // 绝对路径：只要落在**存储边界**之内就收。
    //
    // 以前这里只认下载目录，于是 /sdcard/DCIM 这种路径直接被拒 ——
    // 而"app 能读到的那个根目录"本来就该能管。边界仍然是硬的：
    // 出了 bound 一律拒绝，下面还有软链接兜底。
    if (!rest.empty() && rest[0] == '/') {
        if (rest == bound) {
            *out = bound;
            return true;
        }
        if (!IsUnder(rest, bound)) {
            if (error) {
                *error = "路径不在允许范围内: " + input + "（只能是 " + bound +
                         " 下的路径）";
            }
            return false;
        }
        // 落在下载目录里 → 相对下载目录；否则相对存储根
        if (IsUnder(rest, root)) {
            base = root;
            rest = rest.substr(root.size());
        } else {
            base = bound;
            rest = rest.substr(bound.size());
        }
    }

    // 逐段规范化。遇到 ".." 且栈已空 = 试图逃逸，直接拒绝。
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= rest.size()) {
        size_t slash = rest.find('/', i);
        if (slash == std::string::npos) slash = rest.size();
        const std::string seg = rest.substr(i, slash - i);
        if (seg.empty() || seg == ".") {
            // 跳过
        } else if (seg == "..") {
            if (parts.empty()) {
                if (error) *error = "路径越界（..）: " + input;
                return false;
            }
            parts.pop_back();
        } else {
            parts.push_back(seg);
            if (parts.size() > kMaxPathComponents) {
                if (error) *error = "路径层级超过上限: " + input;
                return false;
            }
        }
        i = slash + 1;
    }

    std::string full = base;
    for (const auto& p : parts) full = JoinPath(full, p);
    if (full.empty()) full = base;

    // 软链接检查：对**已存在的最深祖先**做 realpath，确认解析后仍在 root 内。
    //
    // 不做这一步的话，下载目录里一个指向 /data 的软链接就能让
    // ../ 检查形同虚设 —— 因为文件系统层面它确实"在"下载目录下。
    {
        char rootReal[PATH_MAX];
        if (realpath(bound.c_str(), rootReal) == nullptr) {
            // 边界本身消失或暂时不可解析时必须失败关闭。跳过检查会把
            // symlink 防线变成 fail-open，并且让后续 fopen/rename 看到
            // 一个与 Init 时不同的文件系统拓扑。
            if (error) {
                *error = "无法解析存储边界 " + bound + ": " + strerror(errno);
            }
            return false;
        }
        std::string probe = full;
        while (!probe.empty() && probe != "/") {
            char real[PATH_MAX];
            if (realpath(probe.c_str(), real) != nullptr) {
                const std::string r(real);
                const std::string rr(rootReal);
                if (!IsUnder(r, rr)) {
                    if (error) {
                        *error = "路径经软链接解析后落在允许范围之外: " + input;
                    }
                    return false;
                }
                break;
            }
            probe = DirName(probe);
        }
    }

    *out = full;
    return true;
}

// ── 文件名处理 ──────────────────────────────────────────────────────────────
std::string FileOps::SanitizeFilename(const std::string& name) {
    // 只取最后一段，去掉任何路径成分
    size_t slash = name.find_last_of('/');
    std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);

    std::string out;
    out.reserve(base.size());
    for (unsigned char c : base) {
        if (c == '\\' || c < 0x20 || c == 0x7f) continue;   // 控制字符与反斜杠
        out += static_cast<char>(c);
    }
    // 前导点会变成隐藏文件；"." ".." 更是危险
    while (!out.empty() && out[0] == '.') out.erase(0, 1);
    if (out.size() > 200) out.resize(200);
    return out;
}

// URL 百分号解码。
//
// 不做的话 "my%20file.zip" 会原样落盘成 "my%20file.zip" —— 文件能用，
// 但名字是错的，用户在文件管理器里看到会以为是乱码。
// "+" 不转空格：那只是 application/x-www-form-urlencoded 的约定，
// 路径段里 "+" 就是加号本身。
std::string PercentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string FileOps::FilenameFromUrl(const std::string& url) {
    std::string s = url;
    // 去 fragment、去 query
    const size_t hash = s.find('#');
    if (hash != std::string::npos) s = s.substr(0, hash);
    const size_t q = s.find('?');
    if (q != std::string::npos) s = s.substr(0, q);
    // 去 scheme://host
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        const size_t slash = s.find('/', scheme + 3);
        s = (slash == std::string::npos) ? std::string() : s.substr(slash + 1);
    }
    return SanitizeFilename(PercentDecode(s));
}

// ── 生命周期 ────────────────────────────────────────────────────────────────
FileOps::FileOps() = default;
FileOps::~FileOps() = default;

bool FileOps::Init(std::string* error) {
    // 下载目录：Android 上就是 /sdcard/Download（/sdcard 是软链接）。
    // 先探存在的，避免把软链接路径写进 root_（后面的软链接检查会用它做基准）。
    const char* candidates[] = {
        "/storage/emulated/0/Download",
        "/sdcard/Download",
        "/data/media/0/Download",
        nullptr,
    };
    for (int i = 0; candidates[i] != nullptr; ++i) {
        struct stat st{};
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            char canonical[PATH_MAX];
            if (realpath(candidates[i], canonical) != nullptr) {
                root_ = canonical;
                break;
            }
        }
    }
    if (root_.empty()) {
        if (error) *error = "找不到下载目录（试过 /storage/emulated/0/Download 等）";
        ALOGW("fileops: %s", error ? error->c_str() : "");
        return false;
    }

    // 共享存储根 —— **app 能读到的那个根目录**。
    //
    // 它是下载目录的父目录，也是绝对路径的允许边界。挑存在的那个，
    // 理由同上：别把软链接写进来当基准。
    //
    // ⚠️ 这里刻意**不**退到 "/" —— 那等于把整个文件系统开放出去，
    //    而 /data、/system 里全是我们不该碰的东西。探不到就只用下载目录。
    const char* storageCandidates[] = {
        "/storage/emulated/0",
        "/sdcard",
        "/data/media/0",
        nullptr,
    };
    for (int i = 0; storageCandidates[i] != nullptr; ++i) {
        struct stat st{};
        if (stat(storageCandidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            char canonical[PATH_MAX];
            if (realpath(storageCandidates[i], canonical) != nullptr) {
                storageRoot_ = canonical;
                break;
            }
        }
    }
    // 兜底：用下载目录的父目录。真探不到就退化成"只能操作下载目录"，
    // 功能变窄但不会变危险。
    if (storageRoot_.empty()) {
        const size_t slash = root_.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            storageRoot_ = root_.substr(0, slash);
        }
    }

    // 别名：realpath 确认过的才收
    aliases_.clear();
    if (!storageRoot_.empty()) {
        char canon[PATH_MAX];
        if (realpath(storageRoot_.c_str(), canon) != nullptr) {
            const char* aliasCandidates[] = {"/storage/emulated/0", "/sdcard",
                                              "/data/media/0", nullptr};
            for (int i = 0; aliasCandidates[i] != nullptr; ++i) {
                char a[PATH_MAX];
                if (realpath(aliasCandidates[i], a) != nullptr &&
                    strcmp(a, canon) == 0) {
                    aliases_.emplace_back(aliasCandidates[i]);
                }
            }
        }
    }

    std::string httpErr;
    if (!http_.Init(&httpErr)) {
        // 下载不可用不是致命错误 —— 文件操作仍然能用
        ALOGW("fileops: %s", httpErr.c_str());
    }

    if (!loggedOnce_) {
        // 同 AppOps：Init 会被反复调用，日志只打一次
        ALOGI("文件后端就绪：下载目录 %s，存储根 %s，libcurl %s",
              root_.c_str(), storageRoot_.c_str(),
              http_.Available() ? http_.version().c_str() : "不可用");
        loggedOnce_ = true;
    }
    return true;
}

// ── 下载 ────────────────────────────────────────────────────────────────────
bool FileOps::Download(const std::string& url, const std::string& filename,
                       const std::string& subdir, int64_t maxBytes,
                       int timeoutSec, std::string* savedRelPath, int64_t* bytes,
                       std::string* error) {
    if (bytes) *bytes = 0;
    if (!http_.Available()) {
        if (error) {
            *error = "设备上没有 libcurl，服务端无法下载。"
                     "可让客户端下载后用 SaveToDownloads 推送内容。";
        }
        return false;
    }

    std::string name = SanitizeFilename(filename);
    if (name.empty()) name = FilenameFromUrl(url);
    if (name.empty()) {
        if (error) *error = "无法确定文件名，请在 filename 里指定";
        return false;
    }

    // 目标目录
    std::string dirAbs = root_;
    if (!subdir.empty()) {
        // Download 的 subdir 语义是下载目录下的子目录。ResolveInside
        // 也接受共享存储根下的绝对路径，不能直接把它用于这里，
        // 否则调用方可借此把任意外部 URL 写入 Download 之外的目录，
        // 且返回的 savedRelPath 还会变成空字符串。
        if (subdir[0] == '/') {
            if (error) *error = "subdir 必须是下载目录下的相对路径";
            return false;
        }
        std::string subAbs;
        if (!ResolveInside(root_, storageRoot_, subdir, &subAbs, error)) return false;
        if (!MkdirPAt(storageRoot_.empty() ? root_ : storageRoot_, subAbs, error)) {
            return false;
        }
        dirAbs = subAbs;
    }

    // 目标文件：再走一次约束，确保拼出来的路径仍合法
    const std::string relCandidate =
        (subdir.empty() ? name : (subdir + "/" + name));
    std::string destAbs;
    if (!ResolveInside(root_, storageRoot_, relCandidate, &destAbs, error)) return false;

    const int64_t limit = maxBytes > 0 ? maxBytes : kDefaultMaxDownload;
    const int timeout = timeoutSec > 0 ? timeoutSec : kDefaultTimeoutSec;

    int64_t written = 0;
    if (!http_.DownloadToFile(url, destAbs, limit, timeout, &written, error)) {
        return false;
    }

    NotifyMediaScanner(destAbs);

    if (savedRelPath) *savedRelPath = RelativeTo(root_, destAbs);
    if (bytes) *bytes = written;
    return true;
}

void FileOps::NotifyMediaScanner(const std::string& absPath) {
    // 新文件要让系统"下载"应用和文件管理器看到，得让 MediaScanner 扫一下。
    // 走 broadcast 是标准做法（DownloadManager 内部也是这么做的）。
    CommandResult r;
    RunCommand({"/system/bin/am", "broadcast",
                "-a", "android.intent.action.MEDIA_SCANNER_SCAN_FILE",
                "-d", "file://" + absPath},
               10000, 4096, &r, nullptr);
    // 失败不影响下载本身，只影响它多快出现在列表里
}

// ── 文件操作 ────────────────────────────────────────────────────────────────
bool FileOps::List(const std::string& relPath, std::vector<FileEntry>* out,
                   std::string* error) {
    out->clear();
    std::string abs;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(relPath), &abs, error)) return false;

    const std::string& bound = storageRoot_.empty() ? root_ : storageRoot_;
    const int dirFd = OpenPathDirNoFollow(bound, abs, error);
    if (dirFd < 0) return false;
    DIR* d = fdopendir(dirFd);
    if (d == nullptr) {
        const int savedErrno = errno;
        close(dirFd);
        if (error) {
            if (savedErrno == ENOENT || savedErrno == ENOTDIR) {
                *error = "路径不存在: " + relPath;
            } else {
                *error = "无法访问路径: " + relPath + " (" +
                         strerror(savedErrno) + ")";
            }
        }
        return false;
    }
    dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        struct stat st{};
        if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        const std::string childAbs = JoinPath(abs, ent->d_name);

        FileEntry e;
        e.name  = ent->d_name;
        e.isDir = S_ISDIR(st.st_mode);
        e.size  = e.isDir ? 0 : static_cast<int64_t>(st.st_size);
        e.mtime = static_cast<int64_t>(st.st_mtime);
        // 绝对路径：加了存储根之后"相对谁"不再唯一，
        // 客户端拿它回传时必须能唯一指向一个文件
        e.path  = childAbs;
        out->push_back(std::move(e));
    }
    closedir(d);

    // 目录在前，然后按名字排 —— 让客户端拿到稳定顺序
    std::sort(out->begin(), out->end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return a.name < b.name;
    });
    return true;
}

bool FileOps::Stat(const std::string& relPath, FileEntry* out, std::string* error) {
    *out = FileEntry{};
    std::string abs;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(relPath), &abs, error)) return false;

    struct stat st{};
    const std::string& bound = storageRoot_.empty() ? root_ : storageRoot_;
    if (abs == bound) {
        const int fd = OpenPathDirNoFollow(bound, abs, error);
        if (fd < 0) return false;
        const bool ok = fstat(fd, &st) == 0;
        const int savedErrno = errno;
        close(fd);
        if (!ok) {
            if (error) *error = "无法访问路径: " + relPath + " (" +
                                  strerror(savedErrno) + ")";
            return false;
        }
    } else {
        std::string leaf;
        const int parentFd = OpenParentNoFollow(bound, abs, &leaf, error);
        if (parentFd < 0) return false;
        const bool ok = fstatat(parentFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
        const int savedErrno = errno;
        close(parentFd);
        if (!ok) {
            if (error) {
                if (savedErrno == ENOENT || savedErrno == ENOTDIR) {
                    *error = "路径不存在: " + relPath;
                } else {
                    *error = "无法访问路径: " + relPath + " (" +
                             strerror(savedErrno) + ")";
                }
            }
            return false;
        }
    }
    out->isDir = S_ISDIR(st.st_mode);
    out->size  = out->isDir ? 0 : static_cast<int64_t>(st.st_size);
    out->mtime = static_cast<int64_t>(st.st_mtime);
    out->path  = abs;
    out->name  = out->path == "." ? "/" : out->path.substr(out->path.find_last_of('/') + 1);
    return true;
}

bool FileOps::Mkdir(const std::string& relPath, bool parents, std::string* error) {
    std::string abs;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(relPath), &abs, error)) return false;
    if (abs == root_) return true;   // 根目录已存在

    const std::string& bound = storageRoot_.empty() ? root_ : storageRoot_;
    if (parents) return MkdirPAt(bound, abs, error);
    std::string leaf;
    const int parentFd = OpenParentNoFollow(bound, abs, &leaf, error);
    if (parentFd < 0) return false;
    const bool ok = mkdirat(parentFd, leaf.c_str(), 0775) == 0;
    const int savedErrno = errno;
    if (!ok && savedErrno == EEXIST) {
        struct stat st{};
        if (fstatat(parentFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(st.st_mode)) {
            close(parentFd);
            return true;
        }
    }
    close(parentFd);
    if (!ok && error) *error = "mkdir 失败: " + std::string(strerror(savedErrno));
    return ok;
}

bool FileOps::Delete(const std::string& relPath, bool recursive,
                     std::string* error) {
    std::string abs;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(relPath), &abs, error)) return false;

    // 不允许删下载目录或共享存储根本身 —— 那会把整个存储树端掉，
    // 不会是调用方想要的；绝对路径和 /sdcard 别名最终都会落到这里。
    if (abs == root_ || (!storageRoot_.empty() && abs == storageRoot_)) {
        if (error) *error = (abs == storageRoot_ && abs != root_)
                                 ? "拒绝删除共享存储根本身"
                                 : "拒绝删除下载目录本身";
        return false;
    }

    const std::string& bound = storageRoot_.empty() ? root_ : storageRoot_;
    std::string leaf;
    const int parentFd = OpenParentNoFollow(bound, abs, &leaf, error);
    if (parentFd < 0) return false;
    struct stat st{};
    if (fstatat(parentFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const int savedErrno = errno;
        close(parentFd);
        if (error) *error = "路径不存在: " + relPath + " (" + strerror(savedErrno) + ")";
        return false;
    }
    if (S_ISDIR(st.st_mode) && !recursive) {
        // 非递归只删空目录，避免误删一整棵树
        const bool ok = unlinkat(parentFd, leaf.c_str(), AT_REMOVEDIR) == 0;
        const int savedErrno = errno;
        close(parentFd);
        if (!ok && error) {
            *error = std::string("目录非空或无法删除（需要 recursive）: ") +
                     strerror(savedErrno);
        }
        return ok;
    }
    const bool ok = RemoveTreeAt(parentFd, leaf, error);
    close(parentFd);
    return ok;
}

bool FileOps::Rename(const std::string& fromRel, const std::string& toRel,
                     std::string* error) {
    std::string fromAbs, toAbs;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(fromRel), &fromAbs, error)) return false;
    if (!ResolveInside(root_, storageRoot_, NormalizeAlias(toRel), &toAbs, error)) return false;
    if (fromAbs == root_ || toAbs == root_ ||
        (!storageRoot_.empty() &&
         (fromAbs == storageRoot_ || toAbs == storageRoot_))) {
        if (error) {
            if (!storageRoot_.empty() &&
                (fromAbs == storageRoot_ || toAbs == storageRoot_)) {
                *error = "不能重命名共享存储根本身";
            } else {
                *error = "不能重命名下载目录本身";
            }
        }
        return false;
    }
    const bool samePath = fromAbs == toAbs;

    const std::string& bound = storageRoot_.empty() ? root_ : storageRoot_;
    std::string fromLeaf, toLeaf;
    const int fromParent = OpenParentNoFollow(bound, fromAbs, &fromLeaf, error);
    if (fromParent < 0) return false;
    const int toParent = OpenParentNoFollow(bound, toAbs, &toLeaf, error);
    if (toParent < 0) {
        close(fromParent);
        if (error && error->empty()) *error = "目标目录不存在: " + DirName(toRel);
        return false;
    }
    struct stat st{};
    if (fstatat(fromParent, fromLeaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const int savedErrno = errno;
        close(fromParent);
        close(toParent);
        if (error) *error = "源路径不存在: " + fromRel + " (" + strerror(savedErrno) + ")";
        return false;
    }
    if (samePath) {
        close(fromParent);
        close(toParent);
        return true;
    }
    // Do not silently replace an existing file or directory.  Callers can
    // delete it explicitly, which avoids accidental data loss and also makes
    // the operation atomic with respect to the opened parent directories.
    if (fstatat(toParent, toLeaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        close(fromParent);
        close(toParent);
        if (error) *error = "目标已存在: " + toRel;
        return false;
    }
    const int targetErrno = errno;
    if (targetErrno != ENOENT) {
        close(fromParent);
        close(toParent);
        if (error) *error = "无法检查目标: " + std::string(strerror(targetErrno));
        return false;
    }
    // renameat() replaces an existing destination.  Use renameat2 with
    // RENAME_NOREPLACE so the existence check above remains race safe.
    constexpr unsigned int kRenameNoReplace = 1u;
#if defined(SYS_renameat2)
    const long renameResult = syscall(SYS_renameat2, fromParent, fromLeaf.c_str(),
                                      toParent, toLeaf.c_str(), kRenameNoReplace);
#else
    const long renameResult = -1;
    errno = ENOTSUP;
#endif
    const bool ok = renameResult == 0;
    const int savedErrno = errno;
    close(fromParent);
    close(toParent);
    if (!ok && error) *error = "重命名失败: " + std::string(strerror(savedErrno));
    return ok;
}

}  // namespace remote_control
