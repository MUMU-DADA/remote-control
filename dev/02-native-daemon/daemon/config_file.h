// config_file.h — 持久化配置（/data/misc/remote-control/remote-control.conf）
//
// 路径怎么定的：
//
//   产品形态下 init 用 --config 显式指向 /data/misc/remote-control/remote-control.conf
//   （见 daemon/remote-control.rc）；没传 --config 时 DefaultPath() 返回同一个路径。
//   两者一致是有意的 —— 少一个"文档写 A、运行读 B"的坑。
//
// 为什么**不**放 /sdcard（早期原型曾经放那儿）：
//
//   ① /sdcard 是共享存储，普通应用可写。把配置和可执行载荷放在一起，
//      等于把执行权交出去。
//   ② 它是 FUSE 挂载，UID 1000(system) 读写被拒（实测见
//      docs/09-deployment-and-update.md §4.4）。
//
//   /data/misc/remote-control 由 init 以 0770 shell shell 建出来，服务正好以
//   shell 身份运行。代价是上位应用不再能直接写配置 —— 要改配置得走
//   POST /api/v1/config，由服务自己持久化。
//
// 为什么是纯文本 key=value 而不是 JSON：
//
//   Android 应用侧要用 Java 读写它。key=value 用几行字符串处理就够了，
//   引 JSON 库只为存四个字段不划算 —— 而且双方各用一套 JSON 实现时，
//   格式分歧（转义、数字精度）会变成很难查的兼容性问题。
//   这个文件的读者是人，可读性也重要。
//
// 格式：
//   enabled=1        服务是否应当运行（上位应用写，supervisor 读）
//   bind=127.0.0.1   监听地址。改 0.0.0.0 才是真正"对外"
//   port=8088        HTTP 监听端口。**这就是对外端口本身**，
//                    不再依赖额外的端口转发 —— 转发器和这个值对不上
//                    的话，"改端口"就会变成一个改完就失联的操作
//   auth=0           是否要求访问令牌
//   token=...        auth=1 且为空时，守护进程启动时随机生成并写回

#pragma once

#include <string>

namespace remote_control {

struct PersistedConfig {
    bool        enabled = true;
    std::string bind    = "127.0.0.1";   // 监听地址；0.0.0.0 = 对外
    int         port    = 8088;
    bool        auth    = false;
    std::string token;

    // 上一次读到的 token 是否为空、由本次启动生成
    bool tokenWasGenerated = false;
};

class ConfigFile {
  public:
    // 默认路径。可用 REMOTE_CONTROL_CONFIG 环境变量覆盖（测试和非常规部署要用）。
    static std::string DefaultPath();

    // 读取。**文件不存在不算错误** —— 返回全默认值。
    // 首启就是这个状态：无鉴权、8088、启用。
    static bool Load(const std::string& path, PersistedConfig* out,
                     std::string* error);

    // 写入。原子替换（写临时文件再 rename），避免上位应用读到半个文件 ——
    // 它读的时候守护进程可能正在写 token。
    static bool Save(const std::string& path, const PersistedConfig& cfg,
                     std::string* error);

    // 生成一个随机令牌。
    //
    // 取自 /dev/urandom，base64url 编码。**不能用 rand()** ——
    // 那是可预测的，而这个令牌是唯一的访问控制手段。
    // 取不到 urandom 时返回空串，调用方必须当成失败处理
    // （宁可不启动鉴权，也不能用一个弱令牌假装安全）。
    static std::string GenerateToken(size_t bytes = 24);

    // 解析/序列化单个字段，供上位应用以外的地方复用
    static std::string Serialize(const PersistedConfig& cfg);
};

}  // namespace remote_control
