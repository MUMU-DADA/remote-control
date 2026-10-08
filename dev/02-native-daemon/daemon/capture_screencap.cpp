// capture_screencap.cpp —— 截图后端：调用设备自带的 screencap
//
// 用途：让 remote-control 可以在**没有 AOSP 源码树**的情况下编译运行。
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
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "remote_control_log.h"

namespace remote_control {
namespace {

// 测试用：可以让它指向一个假的 screencap 脚本，在开发机上验证解析逻辑
constexpr const char* kDefaultScreencapPath = "/system/bin/screencap";

// 一帧的总时限。
//
// 正常一次抓帧 100–300 ms（本机实测）；15 秒是"显然出事了"的量级 ——
// 定得太短会在慢设备/大分辨率上误杀正常的抓帧，太长则服务会僵住很久。
// 关键不是数值本身，而是**必须有个上限**：没有它，一次挂住就是永久僵死。
//
// 可用 REMOTE_CONTROL_SCREENCAP_TIMEOUT_MS 覆盖：测试要验"挂住的子进程
// 会被超时打断"，而为此真等 15 秒既慢又没必要。
constexpr int kDefaultFrameTimeoutMs = 15000;

int FrameTimeoutMs() {
    const char* env = getenv("REMOTE_CONTROL_SCREENCAP_TIMEOUT_MS");
    if (env && *env) {
        const long v = strtol(env, nullptr, 10);
        if (v > 0 && v < 600000) return static_cast<int>(v);
    }
    return kDefaultFrameTimeoutMs;
}

const char* ScreencapPath() {
    const char* env = getenv("REMOTE_CONTROL_SCREENCAP_PATH");
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

// ── 带截止时间的读满 ────────────────────────────────────────────────────────
//
// ⚠️ 为什么不能直接用 read()：`/system/bin/screencap` 是**会挂住**的 ——
//    SurfaceFlinger 卡住、或它在等一个永远不来的 fence 时，read() 会无限期
//    阻塞。而 Grab() 全程持有 mutex_，于是整个抓帧路径一起冻住：
//    /info、/capture、画面流全部不再应答，**而且没有任何恢复途径**。
//
//    实测症状（NDK/screencap 后端在权限不足的环境里）：
//    `netstat` 显示端口仍在 LISTEN，但连接全卡在 CLOSE_WAIT，
//    所有请求超时，进程还活着 —— 看起来像"服务没起来"，很难查。
//
//    所以每次读都要有截止时间；超时由调用方杀子进程。
//    这里**故意不提供**不带截止时间的版本 —— 留着一个"方便"的裸 read，
//    早晚会有人顺手用回去，而那正是这次要根除的东西。
int64_t NowMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool ReadFullUntil(int fd, void* buf, size_t len, int64_t deadlineMs) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        const int64_t left = deadlineMs - NowMs();
        if (left <= 0) return false;   // 超时
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        // 每次最多等 200ms：这样 deadline 的判断能及时生效，
        // 也不会因为一次 poll 睡太久而拖慢正常路径。
        const int slice = static_cast<int>(left < 200 ? left : 200);
        const int pr = poll(&pfd, 1, slice > 0 ? slice : 1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) continue;   // 本轮没数据，回到循环顶部判截止时间
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

// 等子进程退出，最多等 timeoutMs；超时就 SIGKILL。
//
// 为什么连收尾也要限时：子进程可能已经把整帧写完、却卡在退出路径上
// （比如它自己的 atexit 里等什么东西）。那种情况下裸 waitpid 一样会挂住。
//
// 返回 true 表示取到了退出状态（statusOut 有效）；false 表示超时被杀。
bool ReapWithTimeout(pid_t pid, int timeoutMs, int* statusOut) {
    const int64_t deadline = NowMs() + timeoutMs;
    for (;;) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (statusOut) *statusOut = status;
            return true;
        }
        if (r < 0 && errno != EINTR) return false;   // 已经被收过了
        if (NowMs() >= deadline) break;
        usleep(5000);
    }
    ALOGW("screencap: 子进程 %d 未在 %d ms 内退出，SIGKILL", static_cast<int>(pid),
          timeoutMs);
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (statusOut) *statusOut = status;
    return false;
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

void Capture::SetDisplayId(uint64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    requestedDisplayId_ = id;
}

void Capture::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    resolved_ = false;
}

bool Capture::ResolveDisplay(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // screencap 后端拿不到显示列表，只能给一个占位值。
    // 真实分辨率在第一次 Grab() 时才知道。
    activeDisplayId_ = requestedDisplayId_ ? requestedDisplayId_ : 1;
    resolved_ = true;
    (void)error;
    return true;
}

bool Capture::ListDisplays(std::vector<DisplayInfo>* out,
                           std::string* error) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

// 把整屏帧裁成请求的那一块，产出**新的** memfd 帧。
//
// ⚠️ 这是"事后裁"，与 SurfaceFlinger 后端的 sourceCrop 差在源头：
//    那边 SF 只合成这一块，管道里根本没有多余的像素；这边整屏已经
//    通过管道传过来了。这是 screencap(exec) 后端的固有代价 ——
//    它的定位就是"没有 AOSP 树时也能跑"，而 screencap 没有裁剪参数。
//
//    值不值得做：还是值得。裁剪之后 memfd 变小、mmap 变小、编码与网络
//    传输全都跟着变小，省的是后面每一环；前面那一次管道传输省不掉而已。
bool CropFrame(Frame* frame, const CaptureOptions& opt, std::string* error) {
    const uint32_t bpp = BytesPerPixel(frame->format);
    if (bpp == 0) {
        if (error) *error = "不支持的像素格式，无法裁剪: " +
                            std::to_string(frame->format);
        return false;
    }

    const int64_t x = opt.cropX, y = opt.cropY;
    const int64_t w = opt.cropW, h = opt.cropH;

    // 越界就**明确报错**，不静默钳到边界 —— 钳过之后返回的是另一块区域，
    // 调用方却以为拿到的是自己请求的那块。宁可让它失败。
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > frame->width || y + h > frame->height) {
        if (error) {
            *error = "裁剪区域 " + std::to_string(x) + "," + std::to_string(y) +
                     " " + std::to_string(w) + "x" + std::to_string(h) +
                     " 超出屏幕 " + std::to_string(frame->width) + "x" +
                     std::to_string(frame->height);
        }
        return false;
    }

    const uint64_t srcStrideBytes = static_cast<uint64_t>(frame->stride) * bpp;
    const uint64_t dstStrideBytes = static_cast<uint64_t>(w) * bpp;
    const uint64_t dstSize        = dstStrideBytes * h;

    Frame cropped;
    cropped.fd = MakeMemfd("remote-control-frame");
    if (cropped.fd < 0) {
        if (error) *error = ErrnoString("memfd 创建");
        return false;
    }
    if (ftruncate(cropped.fd, static_cast<off_t>(dstSize)) != 0) {
        if (error) *error = ErrnoString("ftruncate");
        return false;
    }

    void* src = mmap(nullptr, frame->size, PROT_READ, MAP_SHARED, frame->fd, 0);
    if (src == MAP_FAILED) {
        if (error) *error = ErrnoString("mmap(源帧)");
        return false;
    }
    void* dst = mmap(nullptr, dstSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     cropped.fd, 0);
    if (dst == MAP_FAILED) {
        if (error) *error = ErrnoString("mmap(裁剪帧)");
        munmap(src, frame->size);
        return false;
    }

    // 按行拷：源帧的行有 stride（可能 > width），不能整块 memcpy
    const auto* sp = static_cast<const uint8_t*>(src);
    auto* dp = static_cast<uint8_t*>(dst);
    for (uint32_t r = 0; r < static_cast<uint32_t>(h); ++r) {
        memcpy(dp + r * dstStrideBytes,
               sp + (y + r) * srcStrideBytes + x * bpp,
               dstStrideBytes);
    }

    munmap(src, frame->size);
    munmap(dst, dstSize);

    close(frame->fd);
    cropped.width  = static_cast<uint32_t>(w);
    cropped.height = static_cast<uint32_t>(h);
    cropped.stride = static_cast<uint32_t>(w);
    cropped.format = frame->format;
    cropped.size   = dstSize;
    *frame = std::move(cropped);
    return true;
}

bool Capture::Grab(Frame* out, std::string* error, const CaptureOptions& opt) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

    // 整帧的截止时间：从 fork 之后开始算。
    const int64_t deadline = NowMs() + FrameTimeoutMs();

    auto cleanup = [&]() {
        if (mapBase && mapSize) munmap(mapBase, mapSize);
        close(pipeFd[0]);
        // 子进程还没退就杀掉 —— 详情见 ReapWithTimeout 的注释。
        //
        // ⚠️ 这里给的时间**要短**：走到 cleanup 说明读已经失败或超时了，
        //    再优雅地等它两秒毫无意义，只会让调用方多等两秒。
        //    （实测：0.7 秒的读超时 + 2 秒的收尾等待 = 一次失败要 2.7 秒。）
        ReapWithTimeout(pid, 200, nullptr);
    };

    // --- 1. 读 16 字节头 ---
    uint32_t header[4] = {0, 0, 0, 0};
    if (!ReadFullUntil(pipeFd[0], header, sizeof(header), deadline)) {
        if (error) {
            *error = std::string("读 screencap 头部失败或超时（")
                     + std::to_string(FrameTimeoutMs()) + " ms），可能 "
                     + path + " 执行出错（需要 shell 或 root 权限）或已挂住";
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
    frame.fd = MakeMemfd("remote-control-frame");
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

    if (!ReadFullUntil(pipeFd[0], mapBase, totalSize, deadline)) {
        if (error) {
            *error = "读像素数据不完整或超时（期望 " + std::to_string(totalSize) +
                     " 字节）";
        }
        cleanup();
        return false;
    }

    // --- 3. 收尾 ---
    int status = 0;
    const bool reaped = ReapWithTimeout(pid, 2000, &status);
    close(pipeFd[0]);

    if (!reaped) {
        if (error) *error = std::string(path) + " 抓帧超时未退出（已强杀）";
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

    // screencap 没有裁剪参数，只能事后裁。见 CropFrame 的说明。
    if (opt.hasCrop() && !CropFrame(&frame, opt, error)) {
        return false;
    }

    *out = std::move(frame);
    ok = true;
    return ok;
}

bool Capture::Grab(Frame* out, std::string* error) {
    CaptureOptions opt;
    // ⚠️ targetWidth 在这个后端里**被忽略**：screencap 没有尺寸参数，
    //    而软件缩放需要重采样器（不属于"能跑起来"这个后端的目标）。
    //    裁剪不受影响 —— 那个是逐行拷贝，不需要重采样。
    opt.targetWidth = 0;
    return Grab(out, error, opt);
}

// ---------------------------------------------------------------------------
// 后端标识
// ---------------------------------------------------------------------------

const char* Capture::BackendName() { return "screencap(exec 设备自带命令)"; }

}  // namespace remote_control
