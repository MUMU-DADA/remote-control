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

    // 每个订阅者的目标帧率 / 降采样宽度。
    // 用 map 而不是计数，因为要取最大值。
    std::map<uint64_t, int>      subFps;
    std::map<uint64_t, uint32_t> subMaxWidth;

    // 订阅者的自述信息（谁/什么格式/挂了多久）。
    struct SubMeta {
        int64_t     startedAtMs = 0;
        std::string peer;
        std::string format;
        std::string transport;
    };
    std::map<uint64_t, SubMeta> subMeta;

    uint64_t nextSubId = 1;
    int      maxFps    = 0;      // 0 = 没有任何需求
    uint32_t captureWidth = 0;   // 0 = 原始分辨率

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

        // 抓帧宽度取所有订阅者里**最大的**（0 = 有人要原始分辨率）
        uint32_t w = 0;
        for (const auto& kv : subMaxWidth) {
            if (kv.second == 0) { w = 0; break; }   // 有人要原始尺寸
            if (kv.second > w) w = kv.second;
        }

        if (m != maxFps || w != captureWidth) {
            maxFps = m;
            captureWidth = w;
            // 节奏/尺寸变了，让抓帧线程立刻重新评估
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
    std::string peer;
    {
        std::lock_guard<std::mutex> lk(im->mu);
        im->subFps.erase(id_);
        im->subMaxWidth.erase(id_);
        auto meta = im->subMeta.find(id_);
        if (meta != im->subMeta.end()) {
            peer = meta->second.peer;
            im->subMeta.erase(meta);
        }
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

    if (!peer.empty()) {
        ALOGI("画面流断开 <- %s (id=%llu)", peer.c_str(),
              static_cast<unsigned long long>(id_));
    }
    active_ = false;
}

FrameHub::Sub::Sub(Sub&& other) noexcept
      : id_(other.id_), fps_(other.fps_), maxWidth_(other.maxWidth_),
        active_(other.active_) {
    other.active_ = false;
    other.id_ = 0;
}

void FrameHub::Sub::SetMaxWidth(uint32_t w) {
    if (w == maxWidth_) return;
    Impl* im = Instance().impl_;
    if (im == nullptr) return;
    std::lock_guard<std::mutex> lk(im->mu);
    maxWidth_ = w;
    if (active_) im->subMaxWidth[id_] = w;
    im->RecomputeMaxFps();
    im->stats.captureWidth = im->captureWidth;
    im->cv.notify_all();
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
    im->stats.captureWidth = im->captureWidth;
    // 帧率降下来时，抓帧线程可能正等在一个很早的时刻上 —— 叫醒它重算
    im->cv.notify_all();
}

void FrameHub::Sub::Describe(std::string peer, std::string format,
                             std::string transport) {
    Impl* im = Instance().impl_;
    if (im == nullptr) return;
    std::lock_guard<std::mutex> lk(im->mu);
    auto it = im->subMeta.find(id_);
    if (it == im->subMeta.end()) return;
    it->second.peer      = std::move(peer);
    it->second.format    = std::move(format);
    it->second.transport = std::move(transport);
    ALOGI("画面流订阅 id=%llu <- %s (%s/%s), 共 %zu 个, 最高需求 %d fps",
          static_cast<unsigned long long>(id_),
          it->second.transport.c_str(), it->second.format.c_str(),
          it->second.peer.c_str(), im->subFps.size(), im->maxFps);
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

std::unique_ptr<FrameHub::Sub> FrameHub::Subscribe(int fps, uint32_t maxWidth,
                                                   std::string* error) {
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
    sub->id_       = im->nextSubId++;
    sub->fps_      = fps;
    sub->maxWidth_ = maxWidth;
    sub->active_   = true;

    im->subFps[sub->id_]      = fps;
    im->subMaxWidth[sub->id_] = maxWidth;
    im->subMeta[sub->id_].startedAtMs = NowMs();
    im->RecomputeMaxFps();
    im->stats.subscribers  = static_cast<int>(im->subFps.size());
    im->stats.maxFps       = im->maxFps;
    im->stats.captureWidth = im->captureWidth;

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

                const uint32_t capW = im->captureWidth;
                const int64_t t0 = NowMs();
                FramePtr f = CaptureOnce(im->dispatcher, seq, capW);
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

FramePtr FrameHub::CaptureOnce(Dispatcher* dispatcher, uint64_t seq,
                               uint32_t targetWidth) {
    Request r{};
    r.magic = kMagic;
    r.cmd   = static_cast<uint32_t>(Cmd::Capture);
    // req.x 复用成目标抓帧宽度（见 capture.h 的 SetTargetWidth）：
    // 让 SurfaceFlinger 在**合成阶段**就按这个尺寸渲染，
    // 而不是全分辨率抓下来再软件缩放。
    r.x     = static_cast<int32_t>(targetWidth);

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
    s.captureWidth = impl_->captureWidth;
    s.lastSeq     = impl_->latest != nullptr ? impl_->latest->seq : 0;
    return s;
}

std::vector<FrameHub::SubscriberInfo> FrameHub::ListSubscribers() const {
    std::vector<SubscriberInfo> out;
    if (impl_ == nullptr) return out;

    std::lock_guard<std::mutex> lk(impl_->mu);
    const int64_t now = NowMs();
    out.reserve(impl_->subFps.size());

    for (const auto& kv : impl_->subFps) {
        SubscriberInfo si;
        si.id       = kv.first;
        si.fps      = kv.second;
        si.maxWidth = impl_->subMaxWidth.count(kv.first)
                              ? impl_->subMaxWidth.at(kv.first) : 0;
        // 标记出"是谁把抓帧节奏顶上来的" —— 这是排查"为什么还在满速抓"
        // 时唯一真正要看的那个。
        si.isMaxFps = (kv.second == impl_->maxFps && impl_->maxFps > 0);

        auto m = impl_->subMeta.find(kv.first);
        if (m != impl_->subMeta.end()) {
            si.ageMs = m->second.startedAtMs > 0
                               ? now - m->second.startedAtMs : 0;
            si.peer        = m->second.peer;
            si.format      = m->second.format;
            si.transport   = m->second.transport;
        }
        out.push_back(std::move(si));
    }
    return out;
}

}  // namespace autod
