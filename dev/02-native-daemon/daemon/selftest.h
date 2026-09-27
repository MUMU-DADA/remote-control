// selftest.h —— 部署自检
//
// `autod --selftest` 用。第一次把 autod 推到设备上时，
// 一条命令就能看出环境缺什么，不用靠猜。

#pragma once

#include <cstdint>

namespace autod {

// 运行环境自检。
//
// touchWidth/touchHeight：命令行 --touch-range 指定的触控范围，0 表示自动。
// 必须传进来 —— screencap 后端拿不到显示尺寸，不传的话触控自检只能用
// 32767x32767 试探，那个结论没有意义（坐标换算完全对不上）。
//
// 返回 0 表示全部通过，非 0 是失败项数。
int RunSelfTest(bool verbose, uint32_t touchWidth, uint32_t touchHeight);

}  // namespace autod
