// jpeg_encoder.cpp — dlopen libjpeg 并编码

#include "jpeg_encoder.h"

#include <dlfcn.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "remote_control_log.h"

// vendor 的头文件。用相对路径引，省得每个构建系统都要加 -I。
// 来源与"为什么必须从 AOSP 拿"见 vendor/jpeg/README.md。
#include "vendor/jpeg/jpeglib.h"
#include "vendor/jpeg/jversion.h"   // JVERSION（编译期版本串）

namespace remote_control {

// ── dlopen 出来的函数指针 ──
struct JpegEncoder::Api {
    decltype(&jpeg_std_error)        std_error         = nullptr;
    decltype(&jpeg_CreateCompress)   create_compress   = nullptr;
    decltype(&jpeg_destroy_compress) destroy_compress  = nullptr;
    decltype(&jpeg_set_defaults)     set_defaults      = nullptr;
    decltype(&jpeg_set_quality)      set_quality       = nullptr;
    decltype(&jpeg_start_compress)   start_compress    = nullptr;
    decltype(&jpeg_write_scanlines)  write_scanlines   = nullptr;
    decltype(&jpeg_finish_compress)  finish_compress   = nullptr;
    decltype(&jpeg_mem_dest)         mem_dest          = nullptr;
    decltype(&jpeg_abort_compress)   abort_compress    = nullptr;
};

namespace {

// libjpeg 的错误处理是 setjmp/longjmp —— 它不返回错误码。
//
// 这一点要注意：**调用期间任何一处出错都会 longjmp 回这里**，
// 所以中间不能有需要清理的资源（或者清理必须在这个栈帧里做）。
struct ErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf        jump;
    char           msg[JMSG_LENGTH_MAX];
};

void OnJpegError(j_common_ptr cinfo) {
    auto* e = reinterpret_cast<ErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jump, 1);
}

void OnJpegOutputMessage(j_common_ptr) {
    // libjpeg 会通过这个回调打 trace 级信息（比如"Corrupt JPEG data"）。
    // 默认实现打到 stderr —— 对一个守护进程来说是噪音。
}

}  // namespace

JpegEncoder& JpegEncoder::Instance() {
    static JpegEncoder enc;
    return enc;
}

JpegEncoder::~JpegEncoder() {
    delete api_;
    if (handle_ != nullptr) dlclose(handle_);
}

bool JpegEncoder::Init(std::string* error) {
    if (available_) return true;
    if (handle_ != nullptr) {
        // 之前试过且失败。不要每次编码都重试 dlopen ——
        // 那样在缺库的设备上每帧都要走一遍失败的 dlopen。
        if (error) *error = "libjpeg 不可用（之前已探测过）";
        return false;
    }

    // 名字不带路径：让链接器按常规搜索顺序找（VNDK 的库在
    // /system/lib64 下）。硬写绝对路径会在 32 位设备上找错目录。
    handle_ = dlopen("libjpeg.so", RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        // ⚠️ dlerror() **只能调一次**。
        //
        //    它取出错误之后会把它清掉，第二次调用返回 nullptr。
        //    写成 `dlerror() != nullptr ? dlerror() : "…"` 就会在
        //    拼接时拿到 nullptr —— std::string + nullptr 直接段错误。
        //
        //    实测后果：任何没有 libjpeg 的设备上，守护进程启动即崩。
        const char* e = dlerror();
        if (error) {
            *error = std::string("加载 libjpeg.so 失败: ") +
                     (e != nullptr ? e : "未知原因");
        }
        return false;
    }

    auto* api = new Api();
    const char* missing = nullptr;

    // 一个宏而不是一张表：dlsym 失败时报出**具体是哪个符号**，
    // 比"libjpeg 不可用"有用得多 —— 有些精简 ROM 会裁掉部分符号。
#define REMOTE_CONTROL_JPEG_SYM(field, name)                                        \
    do {                                                                   \
        *(void**)(&api->field) = dlsym(handle_, name);                     \
        if (api->field == nullptr) { missing = name; goto fail; }          \
    } while (0)

    REMOTE_CONTROL_JPEG_SYM(std_error,        "jpeg_std_error");
    REMOTE_CONTROL_JPEG_SYM(create_compress,  "jpeg_CreateCompress");
    REMOTE_CONTROL_JPEG_SYM(destroy_compress, "jpeg_destroy_compress");
    REMOTE_CONTROL_JPEG_SYM(set_defaults,     "jpeg_set_defaults");
    REMOTE_CONTROL_JPEG_SYM(set_quality,      "jpeg_set_quality");
    REMOTE_CONTROL_JPEG_SYM(start_compress,   "jpeg_start_compress");
    REMOTE_CONTROL_JPEG_SYM(write_scanlines,  "jpeg_write_scanlines");
    REMOTE_CONTROL_JPEG_SYM(finish_compress,  "jpeg_finish_compress");
    REMOTE_CONTROL_JPEG_SYM(mem_dest,         "jpeg_mem_dest");
    REMOTE_CONTROL_JPEG_SYM(abort_compress,   "jpeg_abort_compress");
#undef REMOTE_CONTROL_JPEG_SYM

    api_ = api;
    available_ = true;

    // 版本字符串用编译期的 JVERSION（vendor/jpeg/jversion.h）。
    // 不用 dlsym —— libjpeg 没有稳定的版本查询函数，
    // 拿 JVERSION 反而更准（它就是编这个头文件时的那一版）。
    backend_ = std::string("libjpeg: ") + JVERSION;
    ALOGI("JPEG 编码器就绪: %s（dlopen，ABI v%d）", backend_.c_str(),
          JPEG_LIB_VERSION);
    return true;

fail:
    if (error) {
        *error = std::string("libjpeg 缺少符号: ") +
                 (missing != nullptr ? missing : "?");
    }
    ALOGW("%s —— JPEG 不可用，会退回 PNG", error ? error->c_str() : "");
    delete api;
    dlclose(handle_);
    handle_ = nullptr;
    return false;
}

std::string JpegEncoder::EncodeRgba(const uint8_t* rgba, uint32_t width,
                                    uint32_t height, int quality,
                                    std::string* error) {
    if (!available_) {
        if (error) *error = "libjpeg 未就绪";
        return {};
    }
    if (rgba == nullptr || width == 0 || height == 0) {
        if (error) *error = "空图像";
        return {};
    }
    // ⚠️ 参数在这之后就**不能再改**了。
    //
    //    setjmp/longjmp 的规则：在 setjmp 和 longjmp 之间被改动过的、
    //    非 volatile 的局部变量（含参数），longjmp 回来之后取值未定义。
    //    编译器会报 -Wclobbered。
    //
    //    这里先归一化到局部变量，之后只读 —— 参数本身不再被改。
    int q = quality;
    if (q < 1) q = 1;
    if (q > 100) q = 100;

    jpeg_compress_struct cinfo;
    ErrorMgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));

    cinfo.err = api_->std_error(&jerr.pub);
    jerr.pub.error_exit = OnJpegError;
    jerr.pub.output_message = OnJpegOutputMessage;

    // ⚠️ 这两个必须是 volatile。
    //
    //    C 标准：setjmp 之后被修改过的非 volatile 局部变量，
    //    longjmp 回来后取值**未定义**。outBuf 正是 libjpeg 在
    //    出错前可能已经分配过的那个 —— 不标 volatile 就可能漏 free。
    unsigned char* volatile outBuf = nullptr;
    unsigned long  volatile outSize = 0;
    volatile bool created = false;

    // longjmp 会跳回这里。
    if (setjmp(jerr.jump) != 0) {
        if (error) *error = std::string("libjpeg: ") + jerr.msg;
        if (outBuf != nullptr) free(const_cast<unsigned char*>(outBuf));
        // create 成功了才 abort —— 对没初始化的 cinfo 调它，
        // libjpeg 会去碰 cinfo->mem 这个野指针。
        if (created) api_->abort_compress(&cinfo);
        return {};
    }

    api_->create_compress(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
    created = true;
    // ⚠️ 这里必须 cast 掉 volatile。
    //
    //    libjpeg 的 jpeg_mem_dest 收的是 `unsigned char**`，它会在
    //    **压缩过程中**（也就是 setjmp 之后）通过这个指针给我们赋值。
    //    正因为它是在 setjmp 之后被改的，这个变量才必须是 volatile ——
    //    否则编译器可能把它放在寄存器里，longjmp 回来就看不到新值，
    //    那块内存就漏了。
    api_->mem_dest(&cinfo,
                   const_cast<unsigned char**>(&outBuf),
                   const_cast<unsigned long*>(&outSize));

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 4;
    // 直接吃 RGBA，省掉每帧一次 width*height*3 的转换拷贝。
    // JCS_EXT_RGBA 是 libjpeg-turbo 的扩展（值 15），Android 上的
    // libjpeg 就是 turbo，所以有。
    cinfo.in_color_space   = JCS_EXT_RGBA;

    api_->set_defaults(&cinfo);
    api_->set_quality(&cinfo, q, TRUE);

    // ⚠️ 这一行是"和 Skia 输出一致"的关键。
    //
    // libjpeg 默认 optimize_coding = FALSE（jcparam.c:229）：用标准
    // Huffman 表，快但文件大。而 Skia 显式设成 TRUE
    // （SkJpegEncoder.cpp:163，注释写着 "improves compression at the
    // cost of slower encode performance"）。
    //
    // 不设的话两条路差 5% 大小 —— 那不是编码器不同，是参数不同。
    cinfo.optimize_coding = TRUE;

    api_->start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        // 行指针指向源缓冲，不做拷贝
        JSAMPROW row = const_cast<JSAMPROW>(
                rgba + static_cast<size_t>(cinfo.next_scanline) * width * 4);
        api_->write_scanlines(&cinfo, &row, 1);
    }
    api_->finish_compress(&cinfo);

    std::string out(reinterpret_cast<const char*>(outBuf), outSize);
    api_->destroy_compress(&cinfo);
    free(const_cast<unsigned char*>(outBuf));
    return out;
}

}  // namespace remote_control
