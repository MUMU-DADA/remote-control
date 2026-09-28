// fileops.h — 下载目录与文件操作
//
// ⚠️ 安全边界：本模块的所有路径参数都来自客户端，而 remote-control 以 root 运行。
//    一个 "../.." 就能删掉 /data。所以每个入口都强制走 ResolveInside()，
//    把路径约束在下载目录内 —— 这是硬性约束，不是"最好也做一下"。
//
// 下载用 libcurl（运行时 dlopen，见 http_client.h）。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http_client.h"

namespace remote_control {

struct FileEntry {
    std::string name;
    // **绝对路径**。早先是"相对下载目录的路径"，但加了共享存储根之后
    // 相对谁就不明确了 —— 客户端拿它回传时要能唯一指向一个文件。
    std::string path;
    bool        isDir = false;
    int64_t     size  = 0;
    int64_t     mtime = 0;   // Unix 秒
};

class FileOps {
  public:
    FileOps();
    ~FileOps();

    // 探测下载目录与 libcurl。目录不存在会尝试用常见路径。
    bool Init(std::string* error);

    bool        httpAvailable() const { return http_.Available(); }
    std::string downloadDir() const { return root_; }

    // **app 能读到的那个根目录** —— 共享存储根，通常 /storage/emulated/0
    // （/sdcard 是它的软链接）。
    //
    // 和 root_（下载目录）的关系：root_ 在它下面。相对路径仍然相对
    // 下载目录解析（向后兼容），而绝对路径只要落在 storageRoot_ 里就收。
    std::string storageRoot() const { return storageRoot_; }

    // ── 下载 ────────────────────────────────────────────────────────────────
    //
    // url      必须 http/https
    // filename 可为空（从 URL 推断）；只取 basename，不接受路径分隔符
    // subdir   可为空；非空时是下载目录下的子目录（不存在会创建）
    //
    // 成功后触发媒体扫描，这样文件会出现在"下载"应用和文件管理器里。
    bool Download(const std::string& url, const std::string& filename,
                  const std::string& subdir, int64_t maxBytes, int timeoutSec,
                  std::string* savedRelPath, int64_t* bytes, std::string* error);

    // ── 文件操作（路径均相对下载目录）────────────────────────────────────────
    bool List(const std::string& relPath, std::vector<FileEntry>* out,
              std::string* error);
    bool Stat(const std::string& relPath, FileEntry* out, std::string* error);
    bool Mkdir(const std::string& relPath, bool parents, std::string* error);
    bool Delete(const std::string& relPath, bool recursive, std::string* error);
    bool Rename(const std::string& fromRel, const std::string& toRel,
                std::string* error);

    // ── 路径约束（公开出来是为了能单独测）──────────────────────────────────
    //
    // 把客户端给的 input 解析成绝对路径。
    //
    //   · 相对路径 → 相对 root（下载目录）
    //   · 绝对路径 → 必须在 storageRoot 之内
    //
    // 拒绝：逃出边界的 ".."、以及**软链接逃逸**（对已存在的部分做 realpath）。
    static bool ResolveInside(const std::string& root,
                              const std::string& storageRoot,
                              const std::string& input,
                              std::string* out, std::string* error);

    // 从 URL 推断文件名，去掉 query/fragment，并做安全化
    static std::string FilenameFromUrl(const std::string& url);

    // 把文件名安全化：只保留 basename，剔除路径分隔符与控制字符
    static std::string SanitizeFilename(const std::string& name);

  private:
    // 让下载目录里的新文件出现在系统"下载"列表里
    void NotifyMediaScanner(const std::string& absPath);

    // 把 /sdcard、/data/media/0 这类**软链接别名**换算成规范前缀。
    //
    // 客户端天然会写 /sdcard（文档、adb、所有教程都这么写），
    // 而边界是按 /storage/emulated/0 判的 —— 字符串前缀对不上就被拒，
    // 用户看到的是"路径不在允许范围内: /sdcard"，一头雾水。
    //
    // ⚠️ 只换算 Init 时**用 realpath 确认过确实指向同一个存储**的别名，
    //    不盲信名字。
    std::string NormalizeAlias(const std::string& input) const;

    std::string root_;        // 下载目录绝对路径，无尾斜杠
    std::string storageRoot_; // 共享存储根（app 能读到的那个），无尾斜杠
    std::vector<std::string> aliases_;   // 指向同一存储的别名前缀
    HttpClient  http_;
    bool        loggedOnce_ = false;   // Init 会被反复调用，日志只打一次
};

}  // namespace remote_control
