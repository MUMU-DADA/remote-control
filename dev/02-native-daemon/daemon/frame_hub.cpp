// frame_hub.cpp — 共享抓帧的实现
//
// 并发模型（只有一把锁 + 一个条件变量）：
//
//   消费者                         抓帧线程
//   ──────                         ────────
//   拿锁
//   有比我要的新帧？ → 直接返回（共享命中）
//   没有：
//     有人在抓了？ → 就等（不重复请求）
//     没人抓       → wanted = true; 唤醒
//     等条件变量 …
//                                  等 wanted
//                                  wanted = false; capturing = true
//                                  **放锁** → 抓帧（慢）→ 拿锁
//                                  capturing = false; latest = 新帧; 唤醒
//   拿到新帧 → 返回
//
// 关键点：
//   1. **抓帧时不持锁** —— 抓帧要 23~120ms，持锁会让所有消费者排队
//   2. **一次抓帧服务所有等待者** —— 都靠 `latest.seq > afterSeq` 判断
//   3. **没有 wanted 就完全不抓** —— 没人在看的时候线程阻塞在条件变量上
//   4. join 必须在**锁外**做 —— 线程退出也要拿这把锁，锁内 join 会死锁

#include "frame_hub.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
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

}  // namespace

struct FrameHub::Impl {
    mutable std::mutex      mu;
    std::condition_variable cv;

    Dispatcher* dispatcher = nullptr;

    FramePtr latest;
    uint64_t nextSeq = 1;

    int  subscribers  = 0;
    bool threadRunning = false;
    bool stop          = false;
    std::thread thread;

    // 按需抓帧
    bool wanted    = false;   // 有消费者在等新帧
    bool capturing = false;   // 抓帧线程正在抓

    mutable Stats stats;
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
        if (im->subscribers > 0) --im->subscribers;
        im->stats.subscribers = im->subscribers;
        ALOGI("FrameHub: 退订 -1 → %d (sub=%p)", im->subscribers, (void*)this);

        if (im->subscribers == 0 && im->threadRunning) {
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

FrameHub::Sub::Sub(Sub&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

std::unique_ptr<FrameHub::Sub> FrameHub::Subscribe(std::string* error) {
    if (impl_ == nullptr) impl_ = new Impl();
    Impl* im = impl_;

    std::lock_guard<std::mutex> lk(im->mu);

    if (im->dispatcher == nullptr) {
        if (error) *error = "FrameHub 没有配置 Dispatcher";
        return nullptr;
    }

    ++im->subscribers;
    im->stats.subscribers = im->subscribers;

    if (!im->threadRunning) {
        im->stop = false;
        im->wanted = false;
        im->capturing = false;

        // 线程主体。`im` 是稳定的（Impl 由 FrameHub 持有，不会搬）。
        im->thread = std::thread([im]() {
            std::unique_lock<std::mutex> lk(im->mu);
            while (true) {
                im->cv.wait(lk, [im] { return im->stop || im->wanted; });
                if (im->stop) break;

                // 认领这次抓帧，然后**放锁**。
                //
                // 放着锁抓帧的话，一次 23ms（SF）到 120ms（screencap）
                // 会把所有消费者的取帧调用全堵住。
                im->wanted = false;
                im->capturing = true;
                // 序号在**持锁时**取，保证发布顺序和序号一致
                const uint64_t seq = im->nextSeq++;
                lk.unlock();

                const int64_t t0 = NowMs();
                FramePtr f = CaptureOnce(im->dispatcher, seq);
                const int64_t dt = NowMs() - t0;

                lk.lock();
                im->capturing = false;
                if (f != nullptr) {
                    im->latest = f;
                    im->stats.frames++;
                    im->stats.lastSeq = f->seq;
                    im->stats.lastCaptureMs = dt;
                } else {
                    ALOGW("共享抓帧失败");
                }
                // 唤醒所有等待者 —— 它们靠 seq 判断自己要不要
                im->cv.notify_all();
            }
        });

        im->threadRunning = true;
        im->stats.running = true;
        ALOGI("抓帧线程启动（第一个订阅者）");
    }

    auto sub = std::unique_ptr<Sub>(new Sub());
    sub->active_ = true;
    ALOGI("FrameHub: 订阅 +1 → %d (sub=%p)", im->subscribers, (void*)sub.get());
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
    f->capturedAtMs = NowMs();
    f->seq          = seq;

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

FramePtr FrameHub::WaitNext(uint64_t afterSeq, int timeoutMs,
                            uint64_t* outLastSeq) {
    if (impl_ == nullptr) return nullptr;
    Impl* im = impl_;

    std::unique_lock<std::mutex> lk(im->mu);

    if (outLastSeq != nullptr) {
        *outLastSeq = im->latest != nullptr ? im->latest->seq : 0;
    }
    if (!im->threadRunning) return nullptr;

    // 已经有更新的帧了 —— 别人抓的，直接拿走。
    // 这是"共享"生效的地方：三个消费者等同一帧，只有第一个
    // 触发抓帧，另外两个走这条路径。
    if (im->latest != nullptr && im->latest->seq > afterSeq) {
        im->stats.sharedHits++;
        return im->latest;
    }

    // 需要新帧。已经有人在抓了就别重复请求 ——
    // 那次抓帧出来的帧一定比我手里的新。
    if (!im->capturing) {
        im->wanted = true;
        im->stats.waits++;
        im->cv.notify_all();
    } else {
        im->stats.sharedHits++;   // 蹭上了一次在飞的抓帧
    }

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
        return im->latest;
    }
    return nullptr;
}

FrameHub::Stats FrameHub::GetStats() const {
    if (impl_ == nullptr) return Stats{};
    std::lock_guard<std::mutex> lk(impl_->mu);
    Stats s = impl_->stats;
    s.running     = impl_->threadRunning;
    s.subscribers = impl_->subscribers;
    s.lastSeq     = impl_->latest != nullptr ? impl_->latest->seq : 0;
    return s;
}

}  // namespace autod
