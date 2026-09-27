// protocol.h — autod 对外协议定义
//
// 客户端与服务端共用。设计约束：
//   1. 定长结构体，走 SOCK_SEQPACKET，天然保留消息边界
//   2. 截图数据不走这个结构体（Binder/socket 都装不下 8MB 的帧），
//      而是通过 SCM_RIGHTS 传一个 memfd，这里只传元数据
//   3. 所有字段显式小端，跨架构安全

#pragma once

#include <cstddef>
#include <cstdint>

namespace autod {

// 'AUTD' — 用于快速识别串流错位
constexpr uint32_t kMagic = 0x44545541;

enum class Cmd : uint32_t {
    Info       = 1,   // 查询显示参数，不产生副作用
    Capture    = 2,   // 截图，应答附带 memfd
    Tap        = 3,   // 单击
    Swipe      = 4,   // 滑动
    TouchDown  = 5,   // 手动多点触控：按下
    TouchMove  = 6,   // 手动多点触控：移动
    TouchUp    = 7,   // 手动多点触控：抬起
    KeyEvent   = 8,   // 按键注入（保留）

    // ── 应用与文件管理（v2）───────────────────────────────────────────────
    //
    // 这批命令的**请求** payload 是一串 NUL 分隔的 UTF-8 字符串
    // （字符串本身不含 NUL，所以零依赖、无歧义），
    // **应答** payload 是 JSON，通过 memfd 传回（见 Reply.dataSize）。
    //
    // 为什么应答用 JSON 而不是同样用 NUL 分隔：应答是结构化的、字段会增长，
    // 而且我们只需要"写"JSON 不需要"读"——不必引入解析器。
    // 请求则相反，参数少而固定，NUL 分隔比 JSON 更省事也更难写错。
    ListApps      = 10,  // payload: 无。flags & kFlagIncludeSystem 控制范围
                         // → {"count":N,"apps":[{"package":..,"label":..,...}]}
    AppInfo       = 11,  // payload: "<package>"
                         // → 包名/版本/UID/权限/Activity/Service/Receiver/签名摘要
    LaunchApp     = 12,  // payload: "<package>[\0<activity>]"
                         // → {"started":true,"component":..}
    KillApp       = 13,  // payload: "<package>"
                         // → {"stopped":true}
    ForegroundApp = 14,  // payload: 无
                         // → {"package":..,"activity":..,"pid":..}
    InstallApp    = 15,  // fd = APK 内容（memfd），payload: 无
                         // flags & kFlagReplace 控制 -r
                         // → {"installed":true,"package":..}
    Download      = 16,  // payload: "<url>[\0<filename>]"
                         // → {"path":"/sdcard/Download/..","bytes":N}
    FileOp        = 17,  // payload: "<op>[\0<path>[\0<arg>]]"
                         // op: list|stat|mkdir|delete|rename|exists
                         // → 随 op 不同，见 docs/09-file-and-app-api.md
};

enum Flags : uint32_t {
    kFlagNone       = 0,
    // Capture 专用
    kFlagRawRGBA    = 1u << 0,  // 原始 RGBA_8888（默认）
    kFlagPng        = 1u << 1,  // 服务端编码成 PNG（费 CPU，省带宽）
    kFlagGrayscale  = 1u << 2,  // 只要灰度，数据量 1/4
    // 手势专用
    kFlagAsync      = 1u << 8,  // 不等待分发完成

    // ── v2 命令专用 ──
    kFlagIncludeSystem = 1u << 3,  // ListApps：含系统应用（默认只列第三方）
    kFlagWithMetadata  = 1u << 4,  // ListApps：附带标签/版本/安装时间（更慢）
    kFlagReplace       = 1u << 5,  // InstallApp：-r 覆盖安装
    kFlagRecursive     = 1u << 6,  // FileOp delete：递归删除目录
};

struct Request {
    uint32_t magic;        // 必须等于 kMagic
    uint32_t cmd;          // Cmd
    uint32_t flags;        // Flags 位或
    uint32_t pointerId;    // 多点触控指针 ID

    int32_t  x;            // 主坐标（Tap/Touch*）或滑动起点（Swipe）
    int32_t  y;
    int32_t  x2;           // 滑动终点（Swipe）
    int32_t  y2;

    uint32_t durationMs;   // 手势时长，0 表示使用默认值
    float    pressure;     // 0.0 ~ 1.0，0 表示使用默认值
    float    size;         // 接触面积，0 表示使用默认值
};

// ⚠️ 布局提醒：Reply 里的 uint64_t 如果紧跟在一串 uint32_t 后面，
//    编译器会在它前面插入 4 字节隐式填充（把它对齐到 8 字节），
//    导致 C++ 侧 40 字节、Python 侧 36 字节的经典错位。
//    这里显式写出 reserved 字段，让布局在两边都是确定的 40 字节。
//    改动本结构体后，务必同步 client/autod_client.py 里的 REPLY_FMT 和断言。
struct Reply {
    uint32_t magic;        // 回填 kMagic
    uint32_t status;       // 0 = 成功，否则为 errno 风格错误码
    uint32_t cmd;          // 回显

    // Capture 有效
    uint32_t width;
    uint32_t height;
    uint32_t stride;       // 单位：像素
    uint32_t format;       // android PixelFormat，仅 RAW 时有效

    uint32_t reserved;     // 显式填充，不要使用。保证 dataSize 落在 8 字节边界
    uint64_t dataSize;     // memfd 的实际有效字节数
};

static_assert(sizeof(Request) == 44, "Request 布局变了，需同步 Python 客户端");
static_assert(sizeof(Reply) == 40, "Reply 布局变了，需同步 Python 客户端");

// 常见错误码（自定义区间，避免与 errno 混淆）
enum Status : uint32_t {
    kOk             = 0,
    kErrBadMagic    = 0x1001,
    kErrBadCmd      = 0x1002,
    kErrBadArg      = 0x1003,
    kErrNoDisplay   = 0x1004,
    kErrCaptured    = 0x1005,
    kErrInjected    = 0x1006,
    kErrInternal    = 0x1007,
    kErrUnsupported = 0x1008,
    kErrNotFound    = 0x1009,   // 包不存在 / 路径不存在
    kErrPermission  = 0x100a,   // 权限不足
    kErrTimeout     = 0x100b,   // 子进程或下载超时
    kErrPayload     = 0x100c,   // payload 缺失或格式不对
    kErrIo          = 0x100d,   // 文件/网络 IO 失败
};

inline const char* StatusName(uint32_t s) {
    switch (s) {
        case kOk:             return "ok";
        case kErrBadMagic:    return "bad magic";
        case kErrBadCmd:      return "unknown command";
        case kErrBadArg:      return "bad argument";
        case kErrNoDisplay:   return "no display found";
        case kErrCaptured:    return "capture failed";
        case kErrInjected:    return "injection failed";
        case kErrInternal:    return "internal error";
        case kErrUnsupported: return "unsupported";
        case kErrNotFound:    return "not found";
        case kErrPermission:  return "permission denied";
        case kErrTimeout:     return "timeout";
        case kErrPayload:     return "bad payload";
        case kErrIo:          return "io error";
        default:              return "unknown";
    }
}

// 服务端应答包：结构体 + 可选的 fd
//
// 截图时 fd 是装着帧数据的 memfd，通过 SCM_RIGHTS 传给客户端。
// 定义在 protocol.h 而不是 socket_server.h，因为 dispatch 层也要构造它。
struct ReplyPacket {
    Reply reply{};
    int   fd = -1;   // >=0 时通过 SCM_RIGHTS 发送；发送后由调用方关闭
};

// 请求 payload 上限。
//
// SOCK_SEQPACKET 单条消息有上限（net.core.wmem_default 附近，通常 ~208KB），
// 超过会 EMSGSIZE。需要传大块数据（APK、文件内容）时走 fd，不要塞进 payload。
constexpr size_t kMaxRequestPayload = 4096;

// 应答 payload（JSON）上限，超过就报 kErrPayload 而不是悄悄截断
constexpr size_t kMaxReplyPayload = 4u << 20;   // 4 MB

// 默认手势参数
constexpr uint32_t kDefaultTapMs   = 50;
constexpr uint32_t kDefaultSwipeMs = 300;
constexpr float    kDefaultPressure = 1.0f;
constexpr float    kDefaultSize     = 0.02f;  // 相对屏幕短边

}  // namespace autod
