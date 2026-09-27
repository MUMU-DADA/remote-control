// fake_screencap.c —— 假装是 /system/bin/screencap
//
// 让 test_capture_screencap 能在开发机上验证 capture_screencap.cpp 的解析逻辑，
// 不需要真的安卓设备。
//
// 复刻 AOSP screencap.cpp 在「不给文件名」时的输出格式：
//   16 字节头（w, h, pixelFormat, colorSpace）+ 逐行紧密排列的像素
//
// 画的是可反推坐标的渐变图：
//   R = x/(w-1)*255,  G = y/(h-1)*255,  B = 0x40,  A = 0xFF

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    uint32_t w = 64, h = 48, format = 1;   // RGBA_8888

    // 支持 fake_screencap [W H] [--bad-format] [--truncate] 用于异常路径测试
    int truncateOutput = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bad-format") == 0) {
            format = 0xDEAD;
        } else if (strcmp(argv[i], "--truncate") == 0) {
            truncateOutput = 1;
        } else if (strcmp(argv[i], "--fail") == 0) {
            fprintf(stderr, "fake screencap: 故意失败\n");
            return 3;
        } else if (i + 1 < argc) {
            w = (uint32_t)atoi(argv[i]);
            h = (uint32_t)atoi(argv[++i]);
        }
    }

    // 环境变量让测试脚本能控制参数
    if (getenv("FAKE_SCREENCAP_SIZE")) {
        sscanf(getenv("FAKE_SCREENCAP_SIZE"), "%ux%u", &w, &h);
    }
    if (getenv("FAKE_SCREENCAP_BAD_FORMAT")) format = 0xDEAD;
    if (getenv("FAKE_SCREENCAP_TRUNCATE")) truncateOutput = 1;
    if (getenv("FAKE_SCREENCAP_FAIL")) {
        fprintf(stderr, "fake screencap: 故意失败\n");
        return 3;
    }

    uint32_t header[4] = {w, h, format, 0};
    if (fwrite(header, sizeof(uint32_t), 4, stdout) != 4) return 1;

    if (format == 0xDEAD) return 0;   // 只需要头部就能触发未知格式分支

    size_t rowBytes = (size_t)w * 4;
    unsigned char* row = malloc(rowBytes);
    if (!row) return 1;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = (unsigned char)(w > 1 ? x * 255 / (w - 1) : 0);
            row[x * 4 + 1] = (unsigned char)(h > 1 ? y * 255 / (h - 1) : 0);
            row[x * 4 + 2] = 0x40;
            row[x * 4 + 3] = 0xFF;
        }
        if (truncateOutput && y == h / 2) break;   // 只写一半，触发读不全的分支
        if (fwrite(row, 1, rowBytes, stdout) != rowBytes) { free(row); return 1; }
    }

    free(row);
    fflush(stdout);
    return 0;
}
