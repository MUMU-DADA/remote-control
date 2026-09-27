// http_client.h — 用 libcurl 下载（运行时 dlopen）
//
// 为什么用 dlopen 而不是链接：
//   - NDK 里**没有** curl 的头文件和库，直接链接的话 NDK 构建就没有下载能力
//   - 而设备上 /system/lib64/libcurl.so 是有的（VNDK 的一部分），实测可加载
//   所以运行期探测、有就用，两种构建共用一条代码路径。
//
// 为什么必须用 curl 而不是自己写 HTTP：
//   - 现在几乎全是 HTTPS，自己实现要带 TLS 库，而 NDK 里同样没有
//   - 重定向、超时、断点、代理这些细节，curl 都处理好了
//
// 设备上没有 curl 时返回明确的错误，由上层决定怎么降级。

#pragma once

#include <cstdint>
#include <string>

namespace autod {

class HttpClient {
  public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // 尝试加载 libcurl。失败时 error 里写清楚原因，对象仍可用（Available()==false）。
    bool Init(std::string* error);

    bool Available() const { return handle_ != nullptr; }

    // 载入的 libcurl 版本，仅用于日志/诊断
    const std::string& version() const { return version_; }

    // 下载到文件。
    //
    // 限制（都是刻意的）：
    //   - 只允许 http/https，跟随重定向但也只允许 http/https
    //     （否则一个 302 到 file:// 就能读本地文件）
    //   - maxBytes 上限，超了立刻中止并删除半截文件
    //   - 总超时 timeoutSec
    //
    // 失败时会删掉目标文件，不留半截产物。
    bool DownloadToFile(const std::string& url,
                        const std::string& destPath,
                        int64_t maxBytes,
                        int timeoutSec,
                        int64_t* writtenBytes,
                        std::string* error);

  private:
    void* handle_ = nullptr;      // dlopen 句柄
    void* curl_   = nullptr;      // CURL* （easy handle），每次下载新建
    std::string version_;
    bool globalInited_ = false;
};

}  // namespace autod
