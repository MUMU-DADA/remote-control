// frame_hub.h — 共享抓帧
//
// ## 解决什么问题
//
// 原来每个流式客户端各自调一次 `Dispatcher::Handle(Capture)`：
//
//     客户端A ──抓帧──┐
//     客户端B ──抓帧──┼─ 全局锁 → 3 次抓帧，2 份白做
//     客户端C ──抓帧──┘
//
// 三个客户端看的是**同一块屏幕**，抓出来的帧完全一样。实测总抓帧
// 吞吐被锁住，然后被 N 个客户端瓜分。
//
// 现在改成一个抓帧线程 + 一份共享帧：
//
//     抓帧线程 ──→ 最新帧（带序号）
//     客户端A ──读最新帧── 降采样/编码（各自并行）
//     客户端B ──读最新帧──
//
// ## 抓帧节奏：按「所有订阅者的最高需求」，而不是按请求触发
//
// 第一版是按需触发（消费者请求时才抓）。那样**省 CPU，但延迟很差**：
//
//     消费者请求 → 这时才开始抓帧 → 等一整个抓帧周期（23~60ms）
//                → 编码 → 发回
//
// 每帧都要从零等一次抓帧，而且拿到的画面已经是一个周期之前的。
// 实测很卡 —— 这正是用户反馈的"改了自适应之后变卡了"。
//
// 现在改成**按时抓帧**：
//
//   - 抓帧线程按 `max(所有订阅者的目标帧率)` 持续抓
//   - 消费者来了**直接拿最新帧**，延迟接近 0
//   - 客户端改帧率 → 重算节奏（跟着最高需求走）
//   - 没有订阅者 → 停线程，一次都不抓
//
// 既满足"消费端只要 10 帧就别抓 30 帧"，又没有按需触发的延迟。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace autod {

class Dispatcher;

// 一帧。**不可变** —— 发布之后只读，多个消费者共享同一个对象。
//
// ⚠️ 里面**不拷贝像素**：`data` 是抓帧那条 memfd 的只读映射，
//    由 shared_ptr 的删除器负责 munmap + close。一帧 1080p 是 8MB，
//    每个消费者拷一份的话，三个客户端就是 24MB/帧的纯浪费。
struct SharedFrame {
    const uint8_t* data   = nullptr;   // mmap 的只读映射
    size_t         size   = 0;         // 映射长度（字节）
    uint32_t width        = 0;
    uint32_t height       = 0;
    uint32_t stride       = 0;         // **像素**，不是字节
    uint32_t pixelFormat  = 0;         // Android PixelFormat：1=RGBA_8888，5=BGRA_8888
    uint64_t seq          = 0;         // 单调递增，从 1 开始
    int64_t  capturedAtMs = 0;         // 单调时钟

    // 显存格式归一化成 RGBA 需要转换吗（BGRA 要）
    bool needsBgraSwap() const { return pixelFormat == 5; }
};
using FramePtr = std::shared_ptr<const SharedFrame>;

class FrameHub {
  public:
    static FrameHub& Instance();

    // 注入 Dispatcher（抓帧要走它，以复用那把操作锁）。
    // 在 main() 里调一次。
    void Configure(Dispatcher* dispatcher);

    // ── 订阅 ──
    //
    // 第一个订阅者会启动抓帧线程；最后一个离开会停掉它。
    // 用 RAII：持有 Sub 就代表"我要看画面"。
    //
    // ⚠️ 每个订阅者要**上报自己的目标帧率** —— 抓帧线程按所有订阅者里
    //    最高的那个跑。这是"按需求抓帧"的正确形态：
    //
    //      错的（按请求触发）：消费者请求 → 这时才开始抓 → 等一整个
    //                          抓帧周期 → 拿到的画面已经旧了一个周期。
    //                          实测很卡，因为延迟 = 抓帧耗时。
    //
    //      对的（按时抓帧）：抓帧线程持续按最高需求跑，消费者来了直接
    //                        拿最新帧，延迟接近 0。没人在看就停，
    //                        最高需求降下来就跟着降。
    class Sub {
      public:
        ~Sub();
        Sub(Sub&& other) noexcept;
        Sub& operator=(Sub&&) = delete;
        Sub(const Sub&) = delete;
        Sub& operator=(const Sub&) = delete;

        // 客户端改了帧率就调它（服务端会重算抓帧节奏）。
        void SetFps(int fps);

        // 客户端改了降采样宽度就调它。
        //
        // 抓帧按**所有订阅者里最大的 maxWidth** 来 ——
        // SurfaceFlinger 的 DisplayCaptureArgs.width 是**源头降采样**
        // （合成阶段就只处理这么多像素），所以按最大的抓一次就能
        // 服务所有人，各自再缩到自己要的尺寸。
        //
        // 按最小的抓会让要全尺寸的客户端拿到糊图；按最大的抓只是让
        // 小尺寸客户端多缩一次 —— 缩放开销远小于多抓几倍像素。
        // 0 = 不降采样（原始分辨率）。
        void SetMaxWidth(uint32_t w);

        // 自报家门：谁在拉、用什么格式、走哪条传输。
        //
        // 抓帧节奏由**最高需求**决定，所以只要有一个客户端挂着 60fps，
        // 整个进程就一直在满速抓。光看 maxFps=60 没法知道是谁在拉 ——
        // 只能去猜、去挨个关客户端。把来源列出来，"为什么还在抓帧"
        // 就变成一眼的事。
        //
        // 参数都可以为空（比如从 socket 来的订阅没有 peer 地址）。
        void Describe(std::string peer, std::string format,
                      std::string transport);

        // 等到比 afterSeq 更新的一帧。
        //
        // 已经有更新的帧 → **立刻返回**（正常情况：抓帧线程已经备好了）
        // 否则等，最多 timeoutMs。timeoutMs < 0 表示一直等。
        //
        // 超时返回 nullptr，*outLastSeq 是当前最新序号（调用方据此判断
        // "是我没跟上"还是"根本没在抓"）。
        FramePtr WaitNext(uint64_t afterSeq, int timeoutMs, uint64_t* outLastSeq);

      private:
        friend class FrameHub;
        Sub() = default;
        uint64_t id_       = 0;
        int      fps_      = 0;
        uint32_t maxWidth_ = 0;
        bool     active_   = false;
    };

    // 返回 nullptr 表示抓帧不可用（没有 Dispatcher / 启动失败）。
    // error 里是原因。fps 是这个订阅者的目标帧率。
    std::unique_ptr<Sub> Subscribe(int fps, uint32_t maxWidth,
                                   std::string* error);

    // ── 取帧 ──

    // 最新一帧，不阻塞。没有就返回 nullptr。
    FramePtr Latest() const;

    struct Stats {
        bool     running       = false;   // 抓帧线程在跑吗
        int      subscribers   = 0;
        int      maxFps        = 0;       // 当前按多少 fps 在抓（0 = 没需求）
        uint32_t captureWidth  = 0;       // 当前按多少宽抓（0 = 原始分辨率）
        uint64_t frames        = 0;       // 一共抓了多少帧
        uint64_t lastSeq       = 0;
        int64_t  lastCaptureMs = 0;       // 最近一次抓帧耗时
        uint64_t served        = 0;       // 取帧直接命中（没等）的次数
        uint64_t misses        = 0;       // 没等到新帧（超时）的次数
    };
    Stats GetStats() const;

    // 一个订阅者的自述 —— 给 /params 用。
    struct SubscriberInfo {
        uint64_t    id       = 0;
        int         fps      = 0;
        uint32_t    maxWidth = 0;
        int64_t     ageMs    = 0;   // 订阅了多久
        std::string peer;           // "ip:port"，可能为空
        std::string format;         // jpeg / webp / png / h264
        std::string transport;      // "ws" / "mjpeg"
        bool        isMaxFps = false;  // 是不是它把抓帧节奏顶上来的
    };

    // 当前所有订阅者。按 id 升序（即订阅先后）。
    std::vector<SubscriberInfo> ListSubscribers() const;

  private:
    FrameHub() = default;
    ~FrameHub();
    FrameHub(const FrameHub&) = delete;
    FrameHub& operator=(const FrameHub&) = delete;

    // 抓一帧（会走 Dispatcher，即持那把操作锁）。
    // 静态成员是因为抓帧线程的 lambda 需要调它。
    // seq 由调用方分配后传进来 —— CaptureOnce 是静态的，拿不到 impl_。
    static FramePtr CaptureOnce(Dispatcher* dispatcher, uint64_t seq,
                                uint32_t targetWidth);

    struct Impl;
    Impl* impl_ = nullptr;   // 懒创建，避免静态初始化顺序问题
};

}  // namespace autod
