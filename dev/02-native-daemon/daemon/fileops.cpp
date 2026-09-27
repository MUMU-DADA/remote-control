// fileops.cpp — 下载目录与文件操作

#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include "autod_log.h"
#include "subprocess.h"

namespace autod {
namespace {

constexpr int64_t kDefaultMaxDownload = 512LL << 20;   // 512 MB
constexpr int     kDefaultTimeoutSec  = 300;

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

// 递归建目录（mkdir -p）
bool MkdirP(const std::string& path, std::string* error) {
    if (path.empty() || path == "/") return true;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        if (error) *error = path + " 已存在且不是目录";
        return false;
    }
    if (!MkdirP(DirName(path), error)) return false;
    if (mkdir(path.c_str(), 0775) != 0 && errno != EEXIST) {
        if (error) *error = "mkdir " + path + " 失败: " + strerror(errno);
        return false;
    }
    return true;
}

// 递归删除
bool RemoveRecursive(const std::string& path, std::string* error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (error) *error = "lstat " + path + " 失败: " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path.c_str()) != 0) {
            if (error) *error = "删除 " + path + " 失败: " + strerror(errno);
            return false;
        }
        return true;
    }
    DIR* d = opendir(path.c_str());
    if (d == nullptr) {
        if (error) *error = "打开目录 " + path + " 失败: " + strerror(errno);
        return false;
    }
    dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!RemoveRecursive(JoinPath(path, ent->d_name), error)) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    if (rmdir(path.c_str()) != 0) {
        if (error) *error = "删除目录 " + path + " 失败: " + strerror(errno);
        return false;
    }
    return true;
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

}  // namespace

// ── 路径约束 ────────────────────────────────────────────────────────────────
bool FileOps::ResolveInside(const std::string& root, const std::string& input,
                            std::string* out, std::string* error) {
    if (out == nullptr) return false;
    if (root.empty() || root[0] != '/') {
        if (error) *error = "下载目录未初始化";
        return false;
    }

    std::string rest = input;

    // 绝对路径：只接受已经在下载目录里的（客户端常用 list 返回的 path 直接回传）
    if (!rest.empty() && rest[0] == '/') {
        if (rest == root) {
            *out = root;
            return true;
        }
        if (rest.size() > root.size() && rest.compare(0, root.size(), root) == 0 &&
            rest[root.size()] == '/') {
            rest = rest.substr(root.size() + 1);
        } else {
            if (error) {
                *error = "路径不在下载目录内: " + input + "（只能是 " + root +
                         " 下的相对路径）";
            }
            return false;
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
        }
        i = slash + 1;
    }

    std::string full = root;
    for (const auto& p : parts) full = JoinPath(full, p);
    if (full.empty()) full = root;

    // 软链接检查：对**已存在的最深祖先**做 realpath，确认解析后仍在 root 内。
    //
    // 不做这一步的话，下载目录里一个指向 /data 的软链接就能让
    // ../ 检查形同虚设 —— 因为文件系统层面它确实"在"下载目录下。
    {
        char rootReal[PATH_MAX];
        if (realpath(root.c_str(), rootReal) != nullptr) {
            std::string probe = full;
            while (!probe.empty() && probe != "/") {
                char real[PATH_MAX];
                if (realpath(probe.c_str(), real) != nullptr) {
                    const std::string r(real);
                    const std::string rr(rootReal);
                    if (r != rr && !(r.size() > rr.size() &&
                                     r.compare(0, rr.size(), rr) == 0 &&
                                     r[rr.size()] == '/')) {
                        if (error) {
                            *error = "路径经软链接解析后落在下载目录之外: " + input;
                        }
                        return false;
                    }
                    break;
                }
                probe = DirName(probe);
            }
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
            root_ = candidates[i];
            break;
        }
    }
    if (root_.empty()) {
        if (error) *error = "找不到下载目录（试过 /storage/emulated/0/Download 等）";
        ALOGW("fileops: %s", error ? error->c_str() : "");
        return false;
    }

    std::string httpErr;
    if (!http_.Init(&httpErr)) {
        // 下载不可用不是致命错误 —— 文件操作仍然能用
        ALOGW("fileops: %s", httpErr.c_str());
    }

    ALOGI("文件后端就绪：下载目录 %s，libcurl %s", root_.c_str(),
          http_.Available() ? http_.version().c_str() : "不可用");
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
        std::string subAbs;
        if (!ResolveInside(root_, subdir, &subAbs, error)) return false;
        if (!MkdirP(subAbs, error)) return false;
        dirAbs = subAbs;
    }

    // 目标文件：再走一次约束，确保拼出来的路径仍合法
    const std::string relCandidate =
        (subdir.empty() ? name : (subdir + "/" + name));
    std::string destAbs;
    if (!ResolveInside(root_, relCandidate, &destAbs, error)) return false;

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
    if (!ResolveInside(root_, relPath, &abs, error)) return false;

    DIR* d = opendir(abs.c_str());
    if (d == nullptr) {
        if (error) *error = "打开目录失败: " + relPath + " (" + strerror(errno) + ")";
        return false;
    }
    dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        const std::string childAbs = JoinPath(abs, ent->d_name);
        struct stat st{};
        if (lstat(childAbs.c_str(), &st) != 0) continue;

        FileEntry e;
        e.name  = ent->d_name;
        e.isDir = S_ISDIR(st.st_mode);
        e.size  = e.isDir ? 0 : static_cast<int64_t>(st.st_size);
        e.mtime = static_cast<int64_t>(st.st_mtime);
        e.path  = RelativeTo(root_, childAbs);
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
    if (!ResolveInside(root_, relPath, &abs, error)) return false;

    struct stat st{};
    if (lstat(abs.c_str(), &st) != 0) {
        if (error) *error = "路径不存在: " + relPath;
        return false;
    }
    out->isDir = S_ISDIR(st.st_mode);
    out->size  = out->isDir ? 0 : static_cast<int64_t>(st.st_size);
    out->mtime = static_cast<int64_t>(st.st_mtime);
    out->path  = RelativeTo(root_, abs);
    out->name  = out->path == "." ? "/" : out->path.substr(out->path.find_last_of('/') + 1);
    return true;
}

bool FileOps::Mkdir(const std::string& relPath, bool parents, std::string* error) {
    std::string abs;
    if (!ResolveInside(root_, relPath, &abs, error)) return false;
    if (abs == root_) return true;   // 根目录已存在

    struct stat st{};
    if (stat(abs.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        if (error) *error = relPath + " 已存在且不是目录";
        return false;
    }
    if (parents) return MkdirP(abs, error);
    if (mkdir(abs.c_str(), 0775) != 0) {
        if (error) *error = "mkdir 失败: " + std::string(strerror(errno));
        return false;
    }
    return true;
}

bool FileOps::Delete(const std::string& relPath, bool recursive,
                     std::string* error) {
    std::string abs;
    if (!ResolveInside(root_, relPath, &abs, error)) return false;

    // 不允许删下载目录本身 —— 那是把整个目录端掉，不会是调用方想要的
    if (abs == root_) {
        if (error) *error = "拒绝删除下载目录本身";
        return false;
    }

    struct stat st{};
    if (lstat(abs.c_str(), &st) != 0) {
        if (error) *error = "路径不存在: " + relPath;
        return false;
    }
    if (S_ISDIR(st.st_mode) && !recursive) {
        // 非递归只删空目录，避免误删一整棵树
        if (rmdir(abs.c_str()) != 0) {
            if (error) {
                *error = std::string("目录非空或无法删除（需要 recursive）: ") +
                         strerror(errno);
            }
            return false;
        }
        return true;
    }
    return RemoveRecursive(abs, error);
}

bool FileOps::Rename(const std::string& fromRel, const std::string& toRel,
                     std::string* error) {
    std::string fromAbs, toAbs;
    if (!ResolveInside(root_, fromRel, &fromAbs, error)) return false;
    if (!ResolveInside(root_, toRel, &toAbs, error)) return false;
    if (fromAbs == root_ || toAbs == root_) {
        if (error) *error = "不能重命名下载目录本身";
        return false;
    }

    struct stat st{};
    if (stat(fromAbs.c_str(), &st) != 0) {
        if (error) *error = "源路径不存在: " + fromRel;
        return false;
    }
    // 目标父目录必须存在
    if (stat(DirName(toAbs).c_str(), &st) != 0) {
        if (error) *error = "目标目录不存在: " + DirName(toRel);
        return false;
    }
    if (rename(fromAbs.c_str(), toAbs.c_str()) != 0) {
        if (error) *error = "重命名失败: " + std::string(strerror(errno));
        return false;
    }
    return true;
}

}  // namespace autod
