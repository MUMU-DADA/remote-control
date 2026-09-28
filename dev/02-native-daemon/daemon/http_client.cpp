// http_client.cpp — libcurl 运行时加载与下载

#include "http_client.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "autod_log.h"

namespace autod {
namespace {

// ── libcurl 的最小 ABI 声明 ─────────────────────────────────────────────────
//
// 只声明用到的部分。这些常量和结构在 libcurl 的 ABI 里是稳定的
// （curl 承诺保持 C ABI 兼容），所以硬编码是安全的。
// 不要在这里加"顺手也用得上"的东西 —— 每多一个就多一份 ABI 假设。

using CURL = void;
using CURLcode = int;
using CURLoption = int;

constexpr CURLcode CURLE_OK = 0;

// 来自 curl/curl.h（数值是 ABI 的一部分）
constexpr CURLoption CURLOPT_WRITEDATA        = 10001;
constexpr CURLoption CURLOPT_URL              = 10002;
constexpr CURLoption CURLOPT_USERAGENT        = 10018;
constexpr CURLoption CURLOPT_TIMEOUT          = 13;
constexpr CURLoption CURLOPT_FOLLOWLOCATION   = 52;
constexpr CURLoption CURLOPT_MAXREDIRS        = 68;
constexpr CURLoption CURLOPT_NOSIGNAL         = 99;
constexpr CURLoption CURLOPT_CONNECTTIMEOUT   = 78;
constexpr CURLoption CURLOPT_FAILONERROR      = 45;
// CURLOPT_SSL_VERIFYPEER(64) / VERIFYHOST(81) 不设 = 用 curl 的默认值，
// 也就是**校验证书**。之前把它们定义出来却没用，AOSP 的 -Werror 会
// 因为"定义了未使用"直接编译失败。
constexpr CURLoption CURLOPT_WRITEFUNCTION    = 20011;
constexpr CURLoption CURLOPT_PROTOCOLS        = 181;   // CURLPROTO_*
constexpr CURLoption CURLOPT_REDIR_PROTOCOLS  = 182;
constexpr CURLoption CURLOPT_MAXFILESIZE_LARGE = 30117;

constexpr long CURLPROTO_HTTP  = 1;
constexpr long CURLPROTO_HTTPS = 2;

constexpr int CURLINFO_RESPONSE_CODE = 0x200002;   // CURLINFO_LONG + 2

using write_callback_t = size_t (*)(char*, size_t, size_t, void*);

struct CurlApi {
    CURLcode (*global_init)(long) = nullptr;
    CURL*    (*easy_init)() = nullptr;
    void     (*easy_cleanup)(CURL*) = nullptr;
    CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
    CURLcode (*easy_perform)(CURL*) = nullptr;
    CURLcode (*easy_getinfo)(CURL*, int, ...) = nullptr;
    const char* (*easy_strerror)(CURLcode) = nullptr;
    const char* (*version)() = nullptr;
};

CurlApi g_api;
bool    g_loaded = false;

// 写入回调的数据包
struct Sink {
    FILE*     fp = nullptr;
    int64_t   written = 0;
    int64_t   limit = 0;
    bool      overflow = false;
};

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    Sink* s = static_cast<Sink*>(userdata);
    const size_t total = size * nmemb;
    if (s->limit > 0 && s->written + static_cast<int64_t>(total) > s->limit) {
        s->overflow = true;
        return 0;   // 返回短计数会让 curl 以 CURLE_WRITE_ERROR 中止
    }
    const size_t n = fwrite(ptr, 1, total, s->fp);
    s->written += static_cast<int64_t>(n);
    return n;
}

}  // namespace

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() {
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

bool HttpClient::Init(std::string* error) {
    if (g_loaded) {
        handle_ = g_api.easy_init ? reinterpret_cast<void*>(1) : nullptr;
        version_ = g_api.version ? g_api.version() : "unknown";
        return handle_ != nullptr;
    }

    // 依次试几个名字：设备上是 /system/lib64/libcurl.so。
    // RTLD_NOW 会立刻解析所有符号，依赖缺失能当场发现而不是用到才崩。
    const char* candidates[] = {"libcurl.so", "libcurl_vendor.so", nullptr};
    void* h = nullptr;
    std::string lastErr;
    for (int i = 0; candidates[i] != nullptr; ++i) {
        h = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (h != nullptr) break;
        // ⚠️ dlerror() 只能调一次 —— 它取走错误后会清掉，第二次返回 nullptr。
        //    写成 `dlerror() ? dlerror() : "…"` 在拼接/赋值时就会拿到
        //    nullptr（std::string 会段错误，const char* 则得到一个空指针）。
        //    实测：dlopen 失败时这里直接崩。
        const char* dlErr = dlerror();
        lastErr = dlErr ? dlErr : "unknown";
    }
    if (h == nullptr) {
        if (error) {
            *error = "设备上没有可用的 libcurl（" + lastErr +
                     "）—— 下载能力不可用";
        }
        return false;
    }

    auto sym = [&](const char* name) -> void* {
        return dlsym(h, name);
    };

    g_api.global_init    = reinterpret_cast<CURLcode(*)(long)>(sym("curl_global_init"));
    g_api.easy_init      = reinterpret_cast<CURL*(*)()>(sym("curl_easy_init"));
    g_api.easy_cleanup   = reinterpret_cast<void(*)(CURL*)>(sym("curl_easy_cleanup"));
    g_api.easy_setopt    = reinterpret_cast<CURLcode(*)(CURL*, CURLoption, ...)>(
                               sym("curl_easy_setopt"));
    g_api.easy_perform   = reinterpret_cast<CURLcode(*)(CURL*)>(sym("curl_easy_perform"));
    g_api.easy_getinfo   = reinterpret_cast<CURLcode(*)(CURL*, int, ...)>(
                               sym("curl_easy_getinfo"));
    g_api.easy_strerror  = reinterpret_cast<const char*(*)(CURLcode)>(
                               sym("curl_easy_strerror"));
    g_api.version        = reinterpret_cast<const char*(*)()>(sym("curl_version"));

    if (g_api.easy_init == nullptr || g_api.easy_setopt == nullptr ||
        g_api.easy_perform == nullptr || g_api.easy_cleanup == nullptr) {
        dlclose(h);
        if (error) *error = "libcurl 缺少必需符号（easy_init/setopt/perform）";
        return false;
    }

    // 0L 表示让 curl 自己初始化；Android 上不需要 Win32 那套
    if (g_api.global_init != nullptr) {
        g_api.global_init(0L);
    }

    g_loaded = true;
    handle_  = h;
    version_ = g_api.version ? g_api.version() : "unknown";
    ALOGI("libcurl 已加载: %s", version_.c_str());
    return true;
}

bool HttpClient::DownloadToFile(const std::string& url,
                                const std::string& destPath,
                                int64_t maxBytes,
                                int timeoutSec,
                                int64_t* writtenBytes,
                                std::string* error) {
    if (writtenBytes) *writtenBytes = 0;
    if (!g_loaded) {
        if (error) *error = "libcurl 未加载";
        return false;
    }

    // URL 协议白名单。
    // 不限制的话，一个 302 到 file:// 的重定向就能把本地文件读出来。
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        if (error) *error = "只支持 http/https，收到: " + url.substr(0, 32);
        return false;
    }

    FILE* fp = fopen(destPath.c_str(), "wb");
    if (fp == nullptr) {
        if (error) *error = "无法创建 " + destPath + ": " + strerror(errno);
        return false;
    }

    CURL* curl = g_api.easy_init();
    if (curl == nullptr) {
        fclose(fp);
        if (error) *error = "curl_easy_init 失败";
        return false;
    }

    Sink sink;
    sink.fp    = fp;
    sink.limit = maxBytes;

    g_api.easy_setopt(curl, CURLOPT_URL, url.c_str());
    g_api.easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCb);
    g_api.easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    g_api.easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    g_api.easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    g_api.easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    g_api.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    g_api.easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    g_api.easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    g_api.easy_setopt(curl, CURLOPT_USERAGENT, "autod/2.0");
    // 重定向后也只允许 http/https
    g_api.easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    g_api.easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    if (maxBytes > 0) {
        g_api.easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                          static_cast<long long>(maxBytes));
    }

    const CURLcode rc = g_api.easy_perform(curl);

    long httpCode = 0;
    if (g_api.easy_getinfo != nullptr) {
        g_api.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }

    g_api.easy_cleanup(curl);
    fclose(fp);

    if (rc != CURLE_OK || sink.overflow) {
        unlink(destPath.c_str());   // 不留半截文件
        if (error) {
            if (sink.overflow) {
                *error = "文件超过上限 " + std::to_string(maxBytes) + " 字节，已中止";
            } else {
                *error = std::string("下载失败: ") +
                         (g_api.easy_strerror ? g_api.easy_strerror(rc)
                                              : std::to_string(rc).c_str());
            }
        }
        return false;
    }

    if (writtenBytes) *writtenBytes = sink.written;
    ALOGI("下载完成 %s → %s（%lld 字节, HTTP %ld）", url.c_str(), destPath.c_str(),
          static_cast<long long>(sink.written), httpCode);
    return true;
}

}  // namespace autod
