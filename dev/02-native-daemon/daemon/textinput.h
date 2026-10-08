// textinput.h — 整串文本输入
//
// 两条路，各有各的适用面，**不是二选一**：
//
//   paste（默认）—— 写剪贴板 + 发 KEY_PASTE
//       能打任意 Unicode（中文、emoji、任何符号），因为它压根不经过键盘。
//       代价：会**临时改写系统剪贴板**（用完恢复）；个别应用会禁用粘贴。
//
//   type —— 逐字符发按键
//       行为与真人打字一致（会触发逐字符的文本监听、自动补全、
//       输入法的联想），而且不碰剪贴板。
//       代价：**只能打键盘布局里有的字符** —— 也就是可打印 ASCII。
//       Android 自己的 `input text` 也是这条路，所以它对中文会直接崩
//       （实测 `getEvents()` 返回 null → NullPointerException）。
//
// 为什么需要这个模块而不是直接调 `input text`：
//   · `/system/bin/input` 在部分精简系统上不存在
//   · 它同样打不出中文（见上），并不能省掉 paste 这条路
//   · 它会把失败**抛成异常**而不是一句能读懂的话

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace remote_control {
namespace textinput {

// ── UTF-8 ──────────────────────────────────────────────────────────────────

struct Decoded {
    std::vector<uint32_t> codepoints;   // 码点序列
    std::vector<size_t>   offsets;      // 每个码点在原串里的起始字节偏移
    // 非法 UTF-8 的字节数。**不静默丢弃** —— 调用方据此报错，
    // 否则"发了一串乱码进去"会被当成成功。
    size_t invalidBytes = 0;
};

Decoded Decode(const std::string& utf8);

// 把码点编回 UTF-8（报错信息里要回显是哪个字符打不出来）
std::string Encode(uint32_t codepoint);

// ── 键盘布局 ────────────────────────────────────────────────────────────────
//
// 假定是 **US 布局**（Android 默认的 Generic.kcm）。这是刻意的取舍：
// 真正做对需要读设备当前的 key character map，而 Android 没有稳定的
// native 接口去拿它。宁愿明确假定并在文档里写清楚，也不要含糊地猜。

// 字符能不能用按键打出来。能则给出 Linux 键码与是否需要 Shift。
bool MapChar(uint32_t codepoint, uint32_t* keycode, bool* shift);

// 整串能不能全部用按键打出来。
// 不能时通过出参给出**第一个**打不出来的码点及其**字符序号**（从 0 起），
// 让调用方能说出"第 N 个字符打不出来"而不是笼统一句失败。
bool FirstUntypable(const std::string& utf8, uint32_t* codepoint,
                    size_t* charIndex);

}  // namespace textinput
}  // namespace remote_control
