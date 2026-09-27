// service_state.h — 服务的运行时状态与自身控制
//
// 目标里那句「api 拥有控制自身的所有能力」落到代码上就是这一层：
// 启动参数里的每一项、以及运行期才知道的后端/设备/计数，都要能被
// 客户端查到；能改的要能改得动；不能改的**必须如实说需要重启**。
//
// 设计上的两个取舍：
//
//   1. 单例而不是层层传递。埋点遍布各处（每个请求、每次注入、每次抓帧），
//      层层透传一个 State* 只会让签名变脏。单例的代价是测试不便，
//      但这一层的逻辑主要是"读写几个字段"，值得。
//
//   2. Config 与 Runtime 分开。Config 是"启动时决定的"，值稳定；
//      Runtime 是"现在实际是什么样"，每次查询重新采集 ——
//      因为热改配置之后它就会变，缓存住只会撒谎。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "inject.h"   // InjectorConfig

namespace autod {

class Capture;
class Injector;

class ServiceState {
  public:
    static ServiceState& Instance();

    // ── 启动配置 ────────────────────────────────────────────────────────────
    struct Config {
        std::string socketPath;
        bool        usingInitSocket = false;
        std::string initSocketName;
        uint32_t    socketMode   = 0660;
        uint64_t    displayId    = 0;    // 0 = 自动选主显示
        uint32_t    touchWidth   = 0;    // 0 = 取显示分辨率
        uint32_t    touchHeight  = 0;
        bool        verbose      = false;
        int32_t     dropUid      = -1;   // -1 = 不降权
        int32_t     dropGid      = -1;
    };

    void   SetInitialConfig(const Config& c);
    Config GetConfig() const;

    // ── 后端指针 ────────────────────────────────────────────────────────────
    //
    // Init 完成后由 main 设置。用于两件事：
    //   - 采集 Runtime（后端名、显示尺寸、触控范围、uinput 设备）
    //   - 热改配置时真正作用到后端上（改显示、重建 uinput 设备）
    void SetBackends(Capture* capture, Injector* injector);
    void SetInjectorConfig(const InjectorConfig& cfg);   // 供热改后更新记录
    const InjectorConfig& GetInjectorConfig() const;

    // ── 运行时状态（每次调用重新采集）────────────────────────────────────────
    std::string RuntimeJson() const;
    std::string ConfigJson() const;
    std::string StatsJson() const;

    // ── 热改配置 ────────────────────────────────────────────────────────────
    struct ApplyResult {
        std::vector<std::string> applied;          // 已生效
        std::vector<std::string> requiresRestart;  // 需要重启才生效
        std::vector<std::pair<std::string, std::string>> rejected;  // key → 原因
        bool AnyChange() const {
            return !applied.empty() || !requiresRestart.empty();
        }
    };

    // kv 是顺序敏感的键值对（同一键出现多次时后者生效）
    ApplyResult Apply(const std::vector<std::pair<std::string, std::string>>& kv);
    std::string ApplyResultJson(const ApplyResult& r) const;

    // 供 --socket-mode 热改时回调（chmod 现有 socket 文件）
    void SetSocketChmodHook(void (*hook)(uint32_t mode));
    uint32_t CurrentSocketMode() const;

    // ── 统计 ────────────────────────────────────────────────────────────────
    void CountRequest(uint32_t cmd, uint32_t status);

    // ── 生命周期 ────────────────────────────────────────────────────────────
    void RequestShutdown(bool restart);
    bool ShutdownRequested() const;
    bool RestartRequested() const;

    int64_t UptimeMs() const;

    // 供 Describe 用的版本
    static uint32_t ProtocolVersion();

  private:
    ServiceState() = default;

    mutable std::mutex mutex_;
    Config config_;
    InjectorConfig injectorConfig_;
    Capture*  capture_  = nullptr;
    Injector* injector_ = nullptr;

    void (*socketChmodHook_)(uint32_t) = nullptr;
    uint32_t socketMode_ = 0660;

    int64_t  startTimeMs_ = 0;
    bool     shutdown_ = false;
    bool     restart_  = false;

    // 计数器。用数组按命令索引，避免为每条命令写一个字段。
    uint64_t requests_ = 0;
    uint64_t errors_   = 0;
    uint64_t byCommand_[64] = {};
};

}  // namespace autod
