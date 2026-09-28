// mc.c — 最小验证：原生进程能不能用 AMediaCodec 做 H.264 编码
//
// 这不是玩具：它回答的是"H.264 这条路走不走得通"这个前置问题。
// 具体三个：
//   1. 原生守护进程（非 app）能不能创建 AVC 编码器
//   2. configure/start 能不能过（权限、编解码器可用性）
//   3. 能不能真的编出一帧（拿到 NAL 数据）
//
// 任何一步失败，后面写多少代码都是白费。

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("\n  === AMediaCodec AVC 编码器验证 ===\n");

    AMediaCodec* c = AMediaCodec_createEncoderByType("video/avc");
    if (c == NULL) {
        printf("  x 创建失败（设备没有 AVC 编码器？）\n");
        return 1;
    }
    printf("  + 编码器已创建\n");

    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 320);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 480);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, 2000000);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
    // COLOR_FormatYUV420Flexible：让编码器接受通用的 YUV420 输入
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);

    media_status_t st = AMediaCodec_configure(c, f, NULL, NULL,
                                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    printf("  configure: %d %s\n", (int)st, st == AMEDIA_OK ? "+" : "x");
    if (st != AMEDIA_OK) {
        AMediaFormat_delete(f);
        AMediaCodec_delete(c);
        return 1;
    }

    st = AMediaCodec_start(c);
    printf("  start: %d %s\n", (int)st, st == AMEDIA_OK ? "+" : "x");
    if (st != AMEDIA_OK) {
        AMediaFormat_delete(f);
        AMediaCodec_delete(c);
        return 1;
    }

    // 送一帧。用 YUV420Flexible 的话，dequeueInputBuffer 拿到的缓冲
    // 大小由编码器决定 —— 不能假设是 w*h*3/2。
    ssize_t idx = AMediaCodec_dequeueInputBuffer(c, 2000000 /* 2s */);
    printf("  dequeueInputBuffer: %zd %s\n", idx, idx >= 0 ? "+" : "x");
    if (idx >= 0) {
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(c, (size_t)idx, &cap);
        printf("  输入缓冲: %zu 字节（320x480 的 YUV420 理论上是 %d）\n",
               cap, 320 * 480 * 3 / 2);
        if (buf != NULL && cap > 0) {
            memset(buf, 128, cap);                                // Y=128 灰
            for (size_t i = 0; i + 1 < cap; i += 2) buf[i] = 0;   // U=0，偏色便于分辨
            st = AMediaCodec_queueInputBuffer(c, (size_t)idx, 0, cap, 0, 0);
            printf("  queueInputBuffer: %d %s\n", (int)st, st == AMEDIA_OK ? "+" : "x");
        }
    }

    AMediaCodecBufferInfo info;
    memset(&info, 0, sizeof(info));
    ssize_t o = AMediaCodec_dequeueOutputBuffer(c, &info, 3000000 /* 3s */);
    printf("  dequeueOutputBuffer: %zd\n", o);
    if (o >= 0) {
        printf("  + 编出了一帧: %d 字节, flags=0x%x\n", info.size, info.flags);
        size_t osz = 0;
        uint8_t* ob = AMediaCodec_getOutputBuffer(c, (size_t)o, &osz);
        if (ob != NULL && info.size > 4) {
            printf("    前 8 字节:");
            for (int i = 0; i < 8 && i < info.size; ++i) {
                printf(" %02X", ob[info.offset + i]);
            }
            printf("\n");
            // 00 00 00 01 或 00 00 01 = Annex-B 起始码
            const uint8_t* p = ob + info.offset;
            printf("    起始码: %s\n",
                   (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1)))
                           ? "Annex-B（00 00 01 / 00 00 00 01）"
                           : "不是 Annex-B（可能是 AVCC 长度前缀）");
        }
        AMediaCodec_releaseOutputBuffer(c, (size_t)o, false);
    } else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        // 正常：编码器在出第一帧之前会先报一次格式变化（含 SPS/PPS）
        printf("  （格式变化，再取一次）\n");
        o = AMediaCodec_dequeueOutputBuffer(c, &info, 3000000);
        printf("  第二次 dequeueOutputBuffer: %zd\n", o);
        if (o >= 0) {
            printf("  + 编出了一帧: %d 字节, flags=0x%x\n", info.size, info.flags);
            size_t osz = 0;
            uint8_t* ob = AMediaCodec_getOutputBuffer(c, (size_t)o, &osz);
            if (ob != NULL && info.size > 4) {
                const uint8_t* p = ob + info.offset;
                printf("    前 8 字节:");
                for (int i = 0; i < 8 && i < info.size; ++i) printf(" %02X", p[i]);
                printf("\n");
                printf("    起始码: %s\n",
                       (p[0]==0 && p[1]==0 && (p[2]==1 || (p[2]==0 && p[3]==1)))
                           ? "Annex-B" : "不是 Annex-B（可能是 AVCC 长度前缀）");
                // 找 NAL 类型
                size_t off = (p[2] == 1) ? 3 : 4;
                if (info.size > (int)off) {
                    const int t = p[off] & 0x1F;
                    const char* n = (t==1)?"非 IDR 片":(t==5)?"IDR 片":(t==7)?"SPS":
                                    (t==8)?"PPS":(t==6)?"SEI":"其它";
                    printf("    首个 NAL 类型: %d（%s）\n", t, n);
                }
            }
            AMediaCodec_releaseOutputBuffer(c, (size_t)o, false);
        }
    } else {
        printf("  x 没拿到输出（%zd）\n", o);
    }

    AMediaCodec_stop(c);
    AMediaFormat_delete(f);
    AMediaCodec_delete(c);
    printf("  === 完成 ===\n\n");
    return 0;
}
