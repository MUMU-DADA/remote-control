#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

namespace remote_control {

struct EncodedImage {
    std::string bytes;
    uint32_t width = 0, height = 0;
};
using EncodedImagePtr = std::shared_ptr<const EncodedImage>;

// Owned by one generation of pixels, including successive unchanged captures.
// Matching viewers share both the in-flight encode and its immutable result.
class EncodedFrameCache {
  public:
    using Key = std::tuple<int, int, uint32_t, uint32_t>;
    struct Stats { uint64_t encodes, hits, waitTimeouts; };

    EncodedImagePtr Get(const Key& key, int waitMs,
                        const std::function<EncodedImagePtr()>& encode) {
        std::shared_ptr<Entry> entry;
        bool cacheResult = true;
        {
            std::unique_lock<std::mutex> lock(mu_);
            auto it = entries_.find(key);
            if (it != entries_.end()) {
                entry = it->second;
                if (!entry->done && !entry->cv.wait_for(lock,
                        std::chrono::milliseconds(waitMs), [&] { return entry->done; })) {
                    ++waitTimeouts_;
                    return nullptr;
                }
                if (entry->image) ++hits_;
                return entry->image;
            }
            // Bound retained variants even when clients continuously change quality.
            if (entries_.size() >= 8) {
                auto victim = entries_.begin();
                while (victim != entries_.end() && !victim->second->done) ++victim;
                if (victim != entries_.end()) entries_.erase(victim);
                else cacheResult = false;
            }
            if (cacheResult) {
                entry = std::make_shared<Entry>();
                entries_.emplace(key, entry);
            }
        }
        EncodedImagePtr result = encode();
        if (result) ++encodes_;
        if (entry) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                entry->image = result;
                entry->done = true;
                // A dropped or failed encode must be retried on the same pixels.
                if (!result) entries_.erase(key);
            }
            entry->cv.notify_all();
        }
        return result;
    }

    static Stats GetStats() {
        return {encodes_.load(), hits_.load(), waitTimeouts_.load()};
    }

  private:
    struct Entry {
        std::condition_variable cv;
        bool done = false;
        EncodedImagePtr image;
    };
    std::mutex mu_;
    std::map<Key, std::shared_ptr<Entry>> entries_;
    inline static std::atomic<uint64_t> encodes_{0}, hits_{0}, waitTimeouts_{0};
};

}  // namespace remote_control
