// selftest.h —— 部署自检
//
// `remote-control --selftest` 用。第一次把 remote-control 推到设备上时，
// 一条命令就能看出环境缺什么，不用靠猜。

#pragma once

#include <cstdint>
#include <string>

namespace remote_control {

// 运行环境自检。
//
// touchWidth/touchHeight：命令行 --touch-range 指定的触控范围，0 表示自动。
// 必须传进来 —— screencap 后端拿不到显示尺寸，不传的话触控自检只能用
// 32767x32767 试探，那个结论没有意义（坐标换算完全对不上）。
//
// 返回 0 表示全部通过，非 0 是失败项数。
int RunSelfTest(bool verbose, uint32_t touchWidth, uint32_t touchHeight);

// 同一套检查的 JSON 版本（API 的 SelfTest 命令用）。
//
// 与上面共用检查逻辑，只是呈现方式不同 —— 两份实现迟早会不一致，
// 而不一致的那个一定是没人跑的那个。
//
// 注意：跑自检会**真的**抓一帧、真的建一个 uinput 设备。它是有副作用的，
// 不是纯查询。频繁调用会在 logcat 里留下痕迹，也会短暂占用输入设备。
std::string RunSelfTestJson(bool verbose, uint32_t touchWidth,
                            uint32_t touchHeight);

}  // namespace remote_control
