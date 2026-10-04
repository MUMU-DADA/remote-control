#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "../daemon/encoded_frame_cache.h"
#include "test_util.h"

using namespace remote_control;
using namespace remote_control_test;

namespace {

EncodedImagePtr Image(const char* bytes = "encoded") {
    auto image = std::make_shared<EncodedImage>();
    image->bytes = bytes;
    image->width = 720;
    image->height = 1280;
    return image;
}

void TestConcurrentResult() {
    EncodedFrameCache cache;
    const EncodedFrameCache::Key key{1, 75, 720, 1280};
    const auto before = EncodedFrameCache::GetStats();
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    std::atomic<int> encodes{0};
    const auto expected = Image();
    auto first = std::async(std::launch::async, [&] {
        return cache.Get(key, 1000, [&] {
            ++encodes;
            started.set_value();
            gate.wait();
            return expected;
        });
    });
    started.get_future().wait();

    std::vector<std::future<EncodedImagePtr>> waiters;
    for (int i = 0; i < 8; ++i) {
        waiters.emplace_back(std::async(std::launch::async, [&] {
            return cache.Get(key, 1000, [&] {
                ++encodes;
                return Image("unexpected duplicate");
            });
        }));
    }
    release.set_value();
    bool shared = first.get() == expected;
    for (auto& waiter : waiters) shared = waiter.get() == expected && shared;
    Check(shared && encodes == 1, "same-key viewers share one immutable encode");

    const auto after = EncodedFrameCache::GetStats();
    Check(after.encodes - before.encodes == 1 && after.hits - before.hits == 8,
          "successful shared encode and cache-hit counters are accurate");
}

void TestTimeoutAndRetry() {
    EncodedFrameCache cache;
    const EncodedFrameCache::Key key{1, 75, 720, 1280};
    const auto before = EncodedFrameCache::GetStats();
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    std::atomic<int> encodes{0};
    auto producer = std::async(std::launch::async, [&] {
        return cache.Get(key, 1000, [&]() -> EncodedImagePtr {
            ++encodes;
            started.set_value();
            gate.wait();
            return nullptr;
        });
    });
    started.get_future().wait();
    const auto timedOut = cache.Get(key, 1, [&] {
        ++encodes;
        return Image("unexpected duplicate");
    });
    Check(!timedOut && encodes == 1,
          "waiting viewer times out without starting a duplicate encode");
    release.set_value();
    Check(!producer.get(), "failed producer returns no image");

    const auto expected = Image("retry");
    const auto retry = cache.Get(key, 1000, [&] {
        ++encodes;
        return expected;
    });
    Check(retry == expected && encodes == 2,
          "failed encode is retried for the same pixel generation");
    Check(cache.Get(key, 1000, [] { return Image("duplicate"); }) == expected,
          "successful retry is cached");
    const auto after = EncodedFrameCache::GetStats();
    Check(after.waitTimeouts - before.waitTimeouts == 1 &&
                  after.encodes - before.encodes == 1,
          "timeouts and successful encodes are counted separately from failures");
}

void TestVariantLimit() {
    EncodedFrameCache cache;
    std::vector<std::weak_ptr<const EncodedImage>> retained;
    for (int quality = 1; quality <= 20; ++quality) {
        auto image = cache.Get({1, quality, 720, 1280}, 100, [] { return Image(); });
        retained.emplace_back(image);
    }
    int live = 0;
    for (const auto& image : retained) live += !image.expired();
    Check(live == 8, "continuous quality changes retain at most eight variants");

    EncodedFrameCache variants;
    std::atomic<int> encodes{0};
    const auto encode = [&] { ++encodes; return Image(); };
    const auto original = variants.Get({1, 75, 720, 1280}, 100, encode);
    variants.Get({2, 75, 720, 1280}, 100, encode);
    variants.Get({1, 76, 720, 1280}, 100, encode);
    variants.Get({1, 75, 480, 1280}, 100, encode);
    variants.Get({1, 75, 720, 854}, 100, encode);
    Check(encodes == 5 &&
                  variants.Get({1, 75, 720, 1280}, 100, encode) == original,
          "format, quality, width and height select independent variants");

    EncodedFrameCache nextGeneration;
    const auto next = nextGeneration.Get({1, 75, 720, 1280}, 100, encode);
    Check(next != original && encodes == 6,
          "a new pixel generation never reuses a previous generation's image");
}

void TestFullInFlight() {
    EncodedFrameCache cache;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<std::future<EncodedImagePtr>> producers;
    std::vector<std::promise<void>> started(8);
    for (int i = 0; i < 8; ++i) {
        producers.emplace_back(std::async(std::launch::async, [&, i] {
            return cache.Get({1, i, 720, 1280}, 1000, [&] {
                started[i].set_value();
                gate.wait();
                return Image();
            });
        }));
        started[i].get_future().wait();
    }

    int bypassEncodes = 0;
    auto bypass = cache.Get({1, 99, 720, 1280}, 100, [&] {
        ++bypassEncodes;
        return Image("bypass");
    });
    std::weak_ptr<const EncodedImage> weakBypass = bypass;
    bypass.reset();
    Check(weakBypass.expired(),
          "full in-flight cache serves an extra variant without retaining it");
    release.set_value();
    bool valid = true;
    for (auto& producer : producers) valid = static_cast<bool>(producer.get()) && valid;
    Check(valid, "extra variant does not evict or disrupt active producers");
    Check(cache.Get({1, 99, 720, 1280}, 100, [&] {
        ++bypassEncodes;
        return Image("retained");
    }) && bypassEncodes == 2,
          "bypassed variant can be encoded and cached after producers finish");
}

}  // namespace

int main() {
    TestConcurrentResult();
    TestTimeoutAndRetry();
    TestVariantLimit();
    TestFullInFlight();
    return Summary("encoded frame cache");
}
