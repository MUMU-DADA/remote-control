// frame_hub.cpp — 共享抓帧的实现
//
// 并发模型（一把锁 + 一个条件变量）：
//
//   抓帧线程                              消费者
//   ────────                              ──────
//   等「下一个抓帧时刻」
//     maxFps<=0 → 无限等（没需求就不抓）
//     否则等到 nextCaptureAt
//   取 maxFps，排下一次的时刻
//   **放锁** → 抓帧（慢）→ 拿锁
//   latest = 新帧; 唤醒
//                                         拿锁
//                                         有比我要的新帧 → 立刻返回
//                                         没有 → 等到有，或超时
//
// 关键点：
//   1. **抓帧时不持锁** —— 抓帧要 23~120ms，持锁会让所有消费者排队
//   2. **节奏由最高需求决定** —— 消费者不会"等到抓帧完成"，因为抓帧
//      线程一直在按节奏跑，最新帧通常已经备好了（这是延迟的关键）
//   3. **没有订阅者就完全不抓** —— maxFps=0 时线程无限等
//   4. join 必须在**锁外**做 —— 线程退出也要拿这把锁

#include "frame_hub.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "autod_log.h"
#include "dispatch.h"
#include "protocol.h"

namespace autod {

namespace {

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// 抓帧帧率的上限。
//
// 客户端理论上能要到 60fps，但抓帧本身可能只有 15fps（1440x2960 +
// 软件渲染）。把它钳住是为了让 cv 的等待时间不至于小到变成空转。
constexpr int kMaxFps = 60;

}  // namespace

struct FrameHub::Impl {
    mutable std::mutex      mu;
    std::condition_variable cv;

    Dispatcher* dispatcher = nullptr;

    FramePtr latest;
    uint64_t nextSeq = 1;

    // 每个订阅者的目标帧率。用 map 而不是计数，因为要取最大值。
    std::map<uint64_t, int> subFps;
    uint64_t nextSubId = 1;
    int      maxFps    = 0;      // 0 = 没有任何需求

    bool threadRunning = false;
    bool stop          = false;
    std::thread thread;

    // 下一个抓帧时刻（单调时钟毫秒）
    int64_t nextCaptureAt = 0;

    mutable Stats stats;

    // 重算 maxFps（调用方要持锁）。
    //
    // 做成成员而不是自由函数：frame_hub.h 里 Impl 是私有的，
    // 匿名 namespace 的自由函数拿不到它。
    void RecomputeMaxFps() {
        int m = 0;
        for (const auto& kv : subFps) {
            if (kv.second > m) m = kv.second;
        }
        if (m > kMaxFps) m = kMaxFps;
        if (m != maxFps) {
            maxFps = m;
            // 节奏变了，让抓帧线程立刻重新评估
            nextCaptureAt = NowMs();
        }
    }
};

FrameHub& FrameHub::Instance() {
    static FrameHub hub;
    return hub;
}

FrameHub::~FrameHub() {
    delete impl_;
}

void FrameHub::Configure(Dispatcher* dispatcher) {
    if (impl_ == nullptr) impl_ = new Impl();
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->dispatcher = dispatcher;
}

// ── 订阅 ─────────────────────────────────────────────────────────────────────

FrameHub::Sub::~Sub() {
    if (!active_) return;
    Impl* im = Instance().impl_;
    if (im == nullptr) return;

    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->subFps.erase(id_);
        im->RecomputeMaxFps();
        im->stats.subscribers = static_cast<int>(im->subFps.size());
        im->stats.maxFps = im->maxFps;

        if (im->subFps.empty() && im->threadRunning) {
            // 最后一个订阅者走了 —— 停线程。
            //
            // 先把它**移出来**，出了作用域再 join：
            // 抓帧线程退出时也要拿这把锁，锁内 join 会死锁。
            im->stop = true;
            im->cv.notify_all();
            toJoin = std::move(im->thread);
            im->threadRunning = false;
            ALOGI("抓帧线程停止（没有订阅者了）");
        }
    }
    if (toJoin.joinable()) toJoin.join();

    active_ = false;
}

FrameHub::Sub::Sub(Sub&& other) noexcept
      : id_(other.id_), fps_(other.fps_), active_(other.active_) {
    other.active_ = false;
    other.id_ = 0;
}

void FrameHub::Sub::SetFps(int fps) {
    if (fps < 1) fps = 1;
    if (fps > kMaxFps) fps = kMaxFps;
    if (fps == fps_) return;

    Impl* im = Instance().impl_;
    if (im == nullptr) return;
    std::lock_guard<std::mutex> lk(im->mu);
    fps_ = fps;
    if (active_) im->subFps[id_] = fps;
    im->RecomputeMaxFps();
    im->stats.maxFps = im->maxFps;
    // 帧率降下来时，抓帧线程可能正等在一个很早的时刻上 —— 叫醒它重算
    im->cv.notify_all();
}

FramePtr FrameHub::Sub::WaitNext(uint64_t afterSeq, int timeoutMs,
                                 uint64_t* outLastSeq) {
    Impl* im = Instance().impl_;
    if (im == nullptr) return nullptr;

    std::unique_lock<std::mutex> lk(im->mu);
    if (outLastSeq != nullptr) {
        *outLastSeq = im->latest != nullptr ? im->latest->seq : 0;
    }
    if (!im->threadRunning) return nullptr;

    // 已经有更新的帧了 —— 这是**正常路径**，不是特例。
    // 抓帧线程一直在按节奏跑，所以消费者来的时候最新帧通常已经备好。
    if (im->latest != nullptr && im->latest->seq > afterSeq) {
        im->stats.served++;
        return im->latest;
    }

    // 没有更新的帧 —— 等。抓帧线程会按自己的节奏产出，不需要我们催。
    auto hasNew = [im, afterSeq] {
        return im->stop ||
               (im->latest != nullptr && im->latest->seq > afterSeq);
    };
    if (timeoutMs < 0) {
        im->cv.wait(lk, hasNew);
    } else {
        im->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), hasNew);
    }

    if (im->latest != nullptr && im->latest->seq > afterSeq) {
        im->stats.served++;
        return im->latest;
    }
    im->stats.misses++;
    return nullptr;
}

std::unique_ptr<FrameHub::Sub> FrameHub::Subscribe(int fps, std::string* error) {
    if (impl_ == nullptr) impl_ = new Impl();
    Impl* im = impl_;

    std::lock_guard<std::mutex> lk(im->mu);

    if (im->dispatcher == nullptr) {
        if (error) *error = "FrameHub 没有配置 Dispatcher";
        return nullptr;
    }
    if (fps < 1) fps = 1;
    if (fps > kMaxFps) fps = kMaxFps;

    auto sub = std::unique_ptr<Sub>(new Sub());
    sub->id_     = im->nextSubId++;
    sub->fps_    = fps;
    sub->active_ = true;

    im->subFps[sub->id_] = fps;
    im->RecomputeMaxFps();
    im->stats.subscribers = static_cast<int>(im->subFps.size());
    im->stats.maxFps = im->maxFps;

    if (!im->threadRunning) {
        im->stop = false;
        im->nextCaptureAt = NowMs();

        im->thread = std::thread([im]() {
            std::unique_lock<std::mutex> lk2(im->mu);
            while (true) {
                // ── 等到该抓帧的时刻 ──
                while (!im->stop) {
                    if (im->maxFps <= 0) {
                        // 没有任何需求 —— 无限等，**一次都不抓**
                        im->cv.wait(lk2, [im] {
                            return im->stop || im->maxFps > 0;
                        });
                        if (im->stop) break;
                        im->nextCaptureAt = NowMs();
                        continue;
                    }
                    const int64_t now = NowMs();
                    if (now >= im->nextCaptureAt) break;

                    // 等到下一个抓帧时刻，或节奏被改（notify）为止
                    im->cv.wait_for(lk2,
                                    std::chrono::milliseconds(
                                            im->nextCaptureAt - now));
                }
                if (im->stop) break;

                // ── 排下一次的时刻，然后**放锁**抓帧 ──
                //
                // 时刻在抓帧**之前**排：抓帧本身要 23~120ms，
                // 放在后面会把整个节奏往后推一个抓帧周期，
                // 实际帧率永远达不到目标。
                const int fps = im->maxFps;
                im->nextCaptureAt = NowMs() + 1000 / (fps > 0 ? fps : 1);

                // 序号在持锁时取，保证发布顺序和序号一致
                const uint64_t seq = im->nextSeq++;
                lk2.unlock();

                const int64_t t0 = NowMs();
                FramePtr f = CaptureOnce(im->dispatcher, seq);
                const int64_t dt = NowMs() - t0;

                lk2.lock();
                if (f != nullptr) {
                    im->latest = f;
                    im->stats.frames++;
                    im->stats.lastSeq = f->seq;
                    im->stats.lastCaptureMs = dt;
                } else {
                    ALOGW("共享抓帧失败");
                }
                im->cv.notify_all();
            }
        });

        im->threadRunning = true;
        im->stats.running = true;
        ALOGI("抓帧线程启动（第一个订阅者，%d fps）", fps);
    }

    ALOGI("FrameHub: 订阅 +1 → %d，最高需求 %d fps",
          static_cast<int>(im->subFps.size()), im->maxFps);
    return sub;
}

// ── 抓一帧 ───────────────────────────────────────────────────────────────────

FramePtr FrameHub::CaptureOnce(Dispatcher* dispatcher, uint64_t seq) {
    Request r{};
    r.magic = kMagic;
    r.cmd   = static_cast<uint32_t>(Cmd::Capture);

    ReplyPacket rp = dispatcher->Handle(r, "", -1, 0);
    if (rp.reply.status != kOk || rp.fd < 0) {
        if (rp.fd >= 0) close(rp.fd);
        return nullptr;
    }

    const size_t size = static_cast<size_t>(rp.reply.dataSize);
    if (size == 0) {
        close(rp.fd);
        return nullptr;
    }

    void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, rp.fd, 0);
    if (base == MAP_FAILED) {
        ALOGW("mmap 抓帧缓冲失败: %s", strerror(errno));
        close(rp.fd);
        return nullptr;
    }

    const int fd = rp.fd;
    auto* f = new SharedFrame();
    f->data          = static_cast<const uint8_t*>(base);
    f->size          = size;
    f->width         = rp.reply.width;
    f->height        = rp.reply.height;
    f->stride        = rp.reply.stride;
    f->pixelFormat   = rp.reply.format;
    f->capturedAtMs  = NowMs();
    f->seq           = seq;

    // 删除器负责 munmap + close —— 最后一个消费者放手时才释放。
    // 这样"共享"是真的共享：三个客户端拿到的是同一个映射，
    // 而不是三份 614KB（1080p 是 8MB）的拷贝。
    return FramePtr(f, [fd, base, size](const SharedFrame* p) {
        munmap(base, size);
        close(fd);
        delete p;
    });
}

// ── 取帧 ─────────────────────────────────────────────────────────────────────

FramePtr FrameHub::Latest() const {
    if (impl_ == nullptr) return nullptr;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->latest;
}

FrameHub::Stats FrameHub::GetStats() const {
    if (impl_ == nullptr) return Stats{};
    std::lock_guard<std::mutex> lk(impl_->mu);
    Stats s = impl_->stats;
    s.running     = impl_->threadRunning;
    s.subscribers = static_cast<int>(impl_->subFps.size());
    s.maxFps      = impl_->maxFps;
    s.lastSeq     = impl_->latest != nullptr ? impl_->latest->seq : 0;
    return s;
}

}  // namespace autod
