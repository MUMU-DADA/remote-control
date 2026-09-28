// capture_screencap.cpp —— 截图后端：调用设备自带的 screencap
//
// 用途：让 autod 可以在**没有 AOSP 源码树**的情况下编译运行。
// 只需要 NDK + 一台 root 的安卓设备。
//
// 原理：exec /system/bin/screencap（不带 -p 参数），解析它写到 stdout 的
// 原始像素流。格式来自 AOSP 的 screencap.cpp：
//
//     offset 0   uint32  width
//     offset 4   uint32  height
//     offset 8   uint32  pixelFormat
//     offset 12  uint32  colorSpace
//     offset 16  像素数据，逐行紧密排列，每行 width * bytesPerPixel 字节
//
// 代价（对比 capture_surfaceflinger.cpp）：
//   每次截图要 fork + exec 一个进程，约 100-300 ms（SF 直连只要 20-35 ms）
//   而且拿不到 sourceCrop / 降采样这些优化手段
//
// 定位：**让服务能尽早在真机上跑起来**。AOSP 树就绪后换成 SurfaceFlinger 后端。

#include "memfd_util.h"
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "autod_log.h"

namespace autod {
namespace {

// 测试用：可以让它指向一个假的 screencap 脚本，在开发机上验证解析逻辑
constexpr const char* kDefaultScreencapPath = "/system/bin/screencap";

const char* ScreencapPath() {
    const char* env = getenv("AUTOD_SCREENCAP_PATH");
    return (env && *env) ? env : kDefaultScreencapPath;
}

uint32_t BytesPerPixel(uint32_t format) {
    switch (format) {
        case 1:  return 4;   // RGBA_8888
        case 2:  return 4;   // RGBX_8888
        case 3:  return 3;   // RGB_888
        case 4:  return 2;   // RGB_565
        case 5:  return 4;   // BGRA_8888
        case 22: return 8;   // RGBA_FP16
        case 43: return 4;   // RGBA_1010102
        default: return 0;   // 未知
    }
}

// read() 可能被信号打断或只返回一部分，必须循环读满
bool ReadFull(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;   // 对端提前关闭
        got += static_cast<size_t>(n);
    }
    return true;
}

std::string ErrnoString(const char* what) {
    return std::string(what) + " 失败: " + strerror(errno) +
           " (errno=" + std::to_string(errno) + ")";
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

Frame::~Frame() { Reset(); }

Frame::Frame(Frame&& other) noexcept { *this = std::move(other); }

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        Reset();
        fd     = other.fd;
        size   = other.size;
        width  = other.width;
        height = other.height;
        stride = other.stride;
        format = other.format;
        other.fd = -1;
    }
    return *this;
}

void Frame::Reset() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    size = 0;
    width = height = stride = format = 0;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool Capture::Init(std::string* error) {
    // 提前确认 screencap 可执行，避免第一次请求才发现
    const char* path = ScreencapPath();
    if (access(path, X_OK) != 0) {
        if (error) {
            *error = std::string("screencap 不可执行: ") + path +
                     " (" + strerror(errno) + ")";
        }
        return false;
    }
    resolved_ = true;

    // 探针抓一帧，拿到真实分辨率。
    //
    // 为什么值得多花这一次（100-300ms）：screencap 后端拿不到显示枚举，
    // 而触控范围必须与真实分辨率一致，否则坐标会整体偏移。
    // 不探的话用户必须手工 --touch-range，很容易忘、也很难自己发现。
    Frame probe;
    std::string probeErr;
    if (Grab(&probe, &probeErr)) {
        lastWidth_  = probe.width;
        lastHeight_ = probe.height;
        ALOGI("探针抓帧得到分辨率 %ux%u", lastWidth_, lastHeight_);
    } else {
        ALOGW("探针抓帧失败（%s）—— 触控范围需要手工指定 --touch-range",
              probeErr.c_str());
    }

    ALOGI("截图后端就绪，使用 %s", path);
    return true;
}

void Capture::SetDisplayId(uint64_t id) { requestedDisplayId_ = id; }

void Capture::Shutdown() { resolved_ = false; }

bool Capture::ResolveDisplay(std::string* error) {
    // screencap 后端拿不到显示列表，只能给一个占位值。
    // 真实分辨率在第一次 Grab() 时才知道。
    activeDisplayId_ = requestedDisplayId_ ? requestedDisplayId_ : 1;
    resolved_ = true;
    (void)error;
    return true;
}

bool Capture::ListDisplays(std::vector<DisplayInfo>* out,
                           std::string* error) const {
    // screencap 不提供显示枚举，用 Init() 里探针抓帧缓存下来的尺寸。
    // 探针失败时是 0，调用方会回退到默认触控范围并给出提示。
    out->clear();
    DisplayInfo info;
    info.id     = requestedDisplayId_ ? requestedDisplayId_ : 1;
    info.width  = lastWidth_;
    info.height = lastHeight_;
    out->push_back(info);

    if (error && lastWidth_ == 0) {
        *error = "拿不到显示尺寸（探针抓帧失败），请用 --touch-range 指定触控范围";
    }
    return true;
}

bool Capture::Grab(Frame* out, std::string* error) {
    const char* path = ScreencapPath();

    int pipeFd[2];
    if (pipe(pipeFd) != 0) {
        if (error) *error = ErrnoString("pipe");
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = ErrnoString("fork");
        close(pipeFd[0]);
        close(pipeFd[1]);
        return false;
    }

    if (pid == 0) {
        // 子进程：stdout 接到管道，stderr 丢掉
        if (dup2(pipeFd[1], STDOUT_FILENO) < 0) _exit(126);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipeFd[0]);
        close(pipeFd[1]);
        execl(path, path, static_cast<char*>(nullptr));
        _exit(127);   // exec 失败
    }

    // 父进程
    close(pipeFd[1]);

    bool ok = false;
    Frame frame;
    void* mapBase = nullptr;
    uint64_t mapSize = 0;

    auto cleanup = [&]() {
        if (mapBase && mapSize) munmap(mapBase, mapSize);
        close(pipeFd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
    };

    // --- 1. 读 16 字节头 ---
    uint32_t header[4] = {0, 0, 0, 0};
    if (!ReadFull(pipeFd[0], header, sizeof(header))) {
        if (error) {
            *error = std::string("读 screencap 头部失败，可能 ")
                     + path + " 执行出错（需要 shell 或 root 权限）";
        }
        cleanup();
        return false;
    }

    const uint32_t width  = header[0];
    const uint32_t height = header[1];
    const uint32_t format = header[2];
    // header[3] 是 colorspace，这里不透传

    if (width == 0 || height == 0) {
        if (error) {
            *error = "screencap 返回了非法尺寸 " + std::to_string(width) + "x" +
                     std::to_string(height);
        }
        cleanup();
        return false;
    }

    const uint32_t bpp = BytesPerPixel(format);
    if (bpp == 0) {
        if (error) {
            *error = "screencap 返回了不支持的像素格式: " +
                     std::to_string(format) +
                     "（RGBA_8888=1 RGBX_8888=2 RGB_888=3 RGB_565=4 BGRA_8888=5）";
        }
        cleanup();
        return false;
    }

    // 尺寸上界校验。
    //
    // width/height 来自子进程的输出，不能无条件相信：一旦是垃圾值，
    // rowBytes * height 会溢出回绕成一个很小的数，于是 ftruncate 申请
    // 一小块内存、ReadFull 读少量字节就"成功"了 —— 结果返回一帧尺寸
    // 完全错误的图，调用方还以为是好数据。
    //
    // 16384 已经远超任何真实显示（8K 是 7680），留足余量。
    constexpr uint32_t kMaxDimension = 16384;
    if (width > kMaxDimension || height > kMaxDimension) {
        if (error) {
            *error = "screencap 返回的尺寸超出合理范围: " +
                     std::to_string(width) + "x" + std::to_string(height) +
                     "（上限 " + std::to_string(kMaxDimension) + "）";
        }
        cleanup();
        return false;
    }

    const uint64_t rowBytes  = static_cast<uint64_t>(width) * bpp;
    const uint64_t totalSize = rowBytes * height;

    // 双保险：即使上界放过了，也确认乘法没有回绕
    if (totalSize / rowBytes != height || totalSize == 0) {
        if (error) *error = "帧大小计算溢出";
        cleanup();
        return false;
    }

    // --- 2. 建 memfd 并直接把像素读进 mmap（省一次拷贝）---
    frame.fd = MakeMemfd("autod-frame");
    if (frame.fd < 0) {
        if (error) *error = ErrnoString("memfd 创建");
        cleanup();
        return false;
    }
    if (ftruncate(frame.fd, static_cast<off_t>(totalSize)) != 0) {
        if (error) *error = ErrnoString("ftruncate");
        cleanup();
        return false;
    }

    mapSize = totalSize;
    mapBase = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                   frame.fd, 0);
    if (mapBase == MAP_FAILED) {
        if (error) *error = ErrnoString("mmap");
        mapBase = nullptr;
        cleanup();
        return false;
    }

    if (!ReadFull(pipeFd[0], mapBase, totalSize)) {
        if (error) {
            *error = "读像素数据不完整（期望 " + std::to_string(totalSize) +
                     " 字节）";
        }
        cleanup();
        return false;
    }

    // --- 3. 收尾 ---
    int status = 0;
    waitpid(pid, &status, 0);
    close(pipeFd[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error) {
            *error = std::string(path) + " 退出码 " +
                     std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
        munmap(mapBase, mapSize);
        return false;
    }

    munmap(mapBase, mapSize);

    frame.size   = totalSize;
    frame.width  = width;
    frame.height = height;
    frame.stride = width;      // screencap 输出的行是紧密排列的
    frame.format = format;

    // 更新缓存，供 ListDisplays() 使用
    lastWidth_  = width;
    lastHeight_ = height;

    *out = std::move(frame);
    ok = true;
    return ok;
}

// ---------------------------------------------------------------------------
// 后端标识
// ---------------------------------------------------------------------------

const char* Capture::BackendName() { return "screencap(exec 设备自带命令)"; }

}  // namespace autod
