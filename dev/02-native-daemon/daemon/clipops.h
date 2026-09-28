// clipops.h — 剪贴板访问
//
// 实现方式：exec 一个 Java 辅助工具（tools/cliptool），**以 shell 身份运行**。
//
// 为什么是 shell 而不是 root：
//   ClipboardService 的访问控制要过
//     mAppOps.noteOp(OP_READ_CLIPBOARD, uid, callingPackage)
//   而权限是按包名查的。实测：
//     root  + package "android"        → 被拒（"not in focus"）
//     shell + package "com.android.shell" → 通过
//   因为 com.android.shell 持有 READ_CLIPBOARD_IN_BACKGROUND（signature 级，
//   实测 granted=true）。daemon 是 root，所以要 setuid(2000) 之后再 exec。
//
// 为什么用 Java 辅助工具而不是 `service call`：
//   `service call clipboard 4` 能调通，但返回的是编组过的 ClipData Parcel
//   （ClipDescription + 嵌套 Item + CharSequence），从十六进制里解析它
//   既啰嗦又随版本变。Java 侧一个调用就拿到 ClipData 对象了。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace remote_control {

struct ClipInfo {
    bool        has = false;
    std::string text;                    // has 且为文本时有效
    std::vector<std::string> mimeTypes;
    std::string label;
};

class ClipOps {
  public:
    static ClipOps& Instance();

    // 探测辅助工具是否就位（jar 路径 + app_process 存在）
    bool Init(std::string* error);
    bool Available() const { return available_; }

    const std::string& toolPath() const { return toolPath_; }

    // 读取剪贴板。
    //
    // 返回 false 有两种情况，用 *empty 区分：
    //   empty=true  → 剪贴板确实是空的（不是错误）
    //   empty=false → 真出错了，error 里是原因
    bool Get(ClipInfo* out, bool* empty, std::string* error);

    // 写入剪贴板。会回读确认 —— 因为 ClipboardService 在权限不足时
    // 是**静默 return 不抛异常**的，不确认就会报"成功"而实际没写进去。
    bool Set(const std::string& text, std::string* error);

  private:
    ClipOps() = default;

    // 以 shell UID(2000) 运行辅助工具并取回 stdout。
    bool RunTool(const std::vector<std::string>& args, std::string* out,
                 int* exitCode, std::string* error);

    bool        available_ = false;
    std::string toolPath_;      // 设备上的 jar 路径
    std::string toolClass_ = "com.remotecontrol.clip.ClipTool";
};

}  // namespace remote_control
