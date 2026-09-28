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
// 吞吐被锁在 ~4.2 次/秒，然后被 N 个客户端瓜分 —— 1 个客户端 3.5fps，
// 3 个客户端各 1.4fps。
//
// 现在改成一个抓帧线程 + 一份共享帧：
//
//     抓帧线程 ──→ 最新帧（带序号）
//     客户端A ──读最新帧── 降采样/编码（各自并行）
//     客户端B ──读最新帧──
//
// ## 启停：**没有需求的时候完全不抓帧**
//
// 抓帧是这件事里最贵的一步（SurfaceFlinger 23ms，screencap 120ms+）。
// 没人在看的时候还每秒抓 30 次是纯浪费电。
//
// 所以用**按需抓帧**而不是固定频率：
//
//   - 第一个订阅者到来 → 启线程
//   - 最后一个订阅者离开 → 停线程并 join
//   - 线程平时阻塞在条件变量上，**只有消费者要新帧时才抓**
//
// 这比"固定 30fps 抓帧、消费者各自取"更省：消费者要 5fps 就只抓 5 次/秒。

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
    class Sub {
      public:
        ~Sub();
        Sub(Sub&& other) noexcept;
        Sub& operator=(Sub&&) = delete;
        Sub(const Sub&) = delete;
        Sub& operator=(const Sub&) = delete;

      private:
        friend class FrameHub;
        Sub() = default;
        bool active_ = false;
    };

    // 返回 nullptr 表示抓帧不可用（没有 Dispatcher / 启动失败）。
    // error 里是原因。
    std::unique_ptr<Sub> Subscribe(std::string* error);

    // ── 取帧 ──

    // 最新一帧，不阻塞。没有就返回 nullptr。
    FramePtr Latest() const;

    // 等到比 afterSeq 更新的一帧。
    //
    // 内部逻辑：
    //   - 已经有更新的帧 → 立刻返回（多个消费者共享同一次抓帧）
    //   - 否则请求一次抓帧并等待，最多 timeoutMs
    //   - 超时返回 nullptr，*outLastSeq 是本线程上次看到的序号
    //     （调用方据此判断"是我没跟上"还是"根本没在抓"）
    //
    // timeoutMs < 0 表示一直等。
    FramePtr WaitNext(uint64_t afterSeq, int timeoutMs, uint64_t* outLastSeq);

    struct Stats {
        bool     running        = false;   // 抓帧线程在跑吗
        int      subscribers    = 0;
        uint64_t frames         = 0;       // 一共抓了多少帧
        uint64_t lastSeq        = 0;
        int64_t  lastCaptureMs  = 0;       // 最近一次抓帧耗时
        uint64_t sharedHits     = 0;       // 多少次"直接拿到别人抓的帧"
        uint64_t waits          = 0;       // 多少次真的触发了抓帧
    };
    Stats GetStats() const;

  private:
    FrameHub() = default;
    ~FrameHub();
    FrameHub(const FrameHub&) = delete;
    FrameHub& operator=(const FrameHub&) = delete;

    // 抓一帧（会走 Dispatcher，即持那把操作锁）。
    // 静态成员是因为抓帧线程的 lambda 需要调它。
    // seq 由调用方分配后传进来 —— CaptureOnce 是静态的，拿不到 impl_。
    static FramePtr CaptureOnce(Dispatcher* dispatcher, uint64_t seq);

    struct Impl;
    Impl* impl_ = nullptr;   // 懒创建，避免静态初始化顺序问题
};

}  // namespace autod
