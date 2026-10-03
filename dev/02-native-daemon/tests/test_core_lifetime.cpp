#include <atomic>
#include <stdio.h>
#include <thread>
#include <vector>

#include "../daemon/http_client.h"
#include "../daemon/inject.h"
#include "../daemon/inject_backend.h"
#include "../daemon/png_encoder.h"
#include "test_util.h"

using remote_control_test::Check;
using remote_control_test::Info;
using remote_control_test::Summary;

namespace remote_control {
namespace {

std::atomic<int> gBackendOpens{0};
std::atomic<int> gBackendCloses{0};
std::atomic<int> gBackendEmits{0};
std::atomic<bool> gFailNextOpen{false};

class LifetimeBackend final : public InjectorBackend {
  public:
    const char* Name() const override { return "lifetime-test"; }

    bool Open(const InjectorConfig&, std::string* error) override {
        ++gBackendOpens;
        opened_ = true;
        if (gFailNextOpen.exchange(false)) {
            if (error) *error = "injected partial open failure";
            return false;
        }
        return true;
    }

    bool Emit(int32_t, const TouchPoint&, int64_t, int64_t, bool,
              std::string*) override {
        if (!opened_) return false;
        ++gBackendEmits;
        return true;
    }

    void Close() override {
        if (!opened_) return;
        opened_ = false;
        ++gBackendCloses;
    }

  private:
    bool opened_ = false;
};

}  // namespace

std::unique_ptr<InjectorBackend> CreateInjectorBackend() {
    return std::unique_ptr<InjectorBackend>(new LifetimeBackend());
}

}  // namespace remote_control

namespace {

void TestHttpClientInit() {
    printf("\n[1] HttpClient 并发及重复初始化\n");

    constexpr int kThreads = 8;
    constexpr int kClientsPerThread = 4;
    std::atomic<int> available{0};
    std::atomic<int> unavailable{0};
    std::atomic<int> inconsistent{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < kClientsPerThread; ++j) {
                remote_control::HttpClient client;
                std::string error;
                const bool first = client.Init(&error);
                const bool second = client.Init(&error);
                if (first) {
                    ++available;
                    if (!client.Available() || !second || client.version().empty()) {
                        ++inconsistent;
                    }
                } else {
                    ++unavailable;
                    if (second || client.Available()) ++inconsistent;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    Check(inconsistent.load() == 0,
          "重复 Init 保持对象状态一致（成功 %d，未提供 %d）",
          available.load(), unavailable.load());
    Check(available.load() == 0 || unavailable.load() == 0,
          "并发初始化结果一致（成功 %d，未提供 %d）",
          available.load(), unavailable.load());
    if (available.load() == 0) {
        Info("主机未提供 libcurl；成功加载后的句柄复用场景跳过");
    }
}

void TestPngEncoderInit() {
    printf("\n[2] PngEncoder 并发初始化及编码\n");

    constexpr int kThreads = 8;
    constexpr int kEncodesPerThread = 16;
    const uint8_t pixel[] = {0x12, 0x34, 0x56, 0xFF};
    const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::atomic<int> initialized{0};
    std::atomic<int> unavailable{0};
    std::atomic<int> invalid{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            auto& encoder = remote_control::PngEncoder::Instance();
            std::string error;
            if (!encoder.Init(&error)) {
                ++unavailable;
                return;
            }
            ++initialized;
            for (int j = 0; j < kEncodesPerThread; ++j) {
                const std::string png = encoder.EncodeRgba(pixel, 1, 1, 0, &error);
                if (png.size() < sizeof(signature) ||
                    png.compare(0, sizeof(signature),
                                reinterpret_cast<const char*>(signature),
                                sizeof(signature)) != 0) {
                    ++invalid;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    Check(invalid.load() == 0,
          "并发重复 Init 后 PNG 编码有效（初始化 %d，未提供 %d）",
          initialized.load(), unavailable.load());
    Check(initialized.load() == 0 || unavailable.load() == 0,
          "并发初始化结果一致（初始化 %d，未提供 %d）",
          initialized.load(), unavailable.load());
    if (initialized.load() == 0) {
        Info("主机未提供 zlib；PNG 编码场景跳过");
    }
}

void TestInjectorReinitialization() {
    printf("\n[3] Injector 后端重配置\n");

    remote_control::gBackendOpens = 0;
    remote_control::gBackendCloses = 0;
    remote_control::gBackendEmits = 0;
    remote_control::gFailNextOpen = false;
    bool first = false;
    bool second = false;
    bool failed = false;
    bool retained = false;
    bool emitted = false;
    {
        remote_control::Injector injector;
        remote_control::InjectorConfig config;
        std::string error;
        first = injector.Init(config, &error);
        second = injector.Init(config, &error);
        remote_control::gFailNextOpen = true;
        failed = !injector.Init(config, &error);
        retained = std::string(injector.BackendName()) == "lifetime-test";
        remote_control::TouchPoint point;
        emitted = injector.TouchDown(point, false, &error);
    }

    Check(first && second && failed,
          "两次成功重配置后再拒绝失败配置");
    Check(remote_control::gBackendOpens.load() == 3 &&
                  remote_control::gBackendCloses.load() == 3,
          "旧后端与打开失败的候选均释放（open=%d close=%d）",
          remote_control::gBackendOpens.load(), remote_control::gBackendCloses.load());
    Check(retained && emitted && remote_control::gBackendEmits.load() == 1,
          "候选打开失败后旧后端仍可用");
}

}  // namespace

int main() {
    TestHttpClientInit();
    TestPngEncoderInit();
    TestInjectorReinitialization();
    return Summary("daemon 动态库生命周期");
}
