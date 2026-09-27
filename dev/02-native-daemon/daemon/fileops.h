// fileops.h — 下载目录与文件操作
//
// ⚠️ 安全边界：本模块的所有路径参数都来自客户端，而 autod 以 root 运行。
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

namespace autod {

struct FileEntry {
    std::string name;
    std::string path;      // 相对下载目录的路径
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
    // 把客户端给的 input 解析成 root 内的绝对路径。
    // 拒绝：绝对路径、含 ".." 逃逸、以及**软链接逃逸**（对已存在的部分做 realpath）。
    static bool ResolveInside(const std::string& root, const std::string& input,
                              std::string* out, std::string* error);

    // 从 URL 推断文件名，去掉 query/fragment，并做安全化
    static std::string FilenameFromUrl(const std::string& url);

    // 把文件名安全化：只保留 basename，剔除路径分隔符与控制字符
    static std::string SanitizeFilename(const std::string& name);

  private:
    // 让下载目录里的新文件出现在系统"下载"列表里
    void NotifyMediaScanner(const std::string& absPath);

    std::string root_;        // 下载目录绝对路径，无尾斜杠
    HttpClient  http_;
    bool        loggedOnce_ = false;   // Init 会被反复调用，日志只打一次
};

}  // namespace autod
