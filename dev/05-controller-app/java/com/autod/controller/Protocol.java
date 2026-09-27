package com.autod.controller;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * 与 daemon/protocol.h 严格对应的协议定义。
 *
 * <p>布局必须逐字节对齐 —— C++ 侧有 static_assert 钉住大小，这里也用常量校验，
 * 任何一边改了都会在运行期立刻暴露（magic 或长度不符），而不是悄悄错位。
 *
 * <p>注意 Reply 里有 8 个 uint32 而不是 7 个：最后一个 {@code reserved} 不是装饰，
 * 它占掉了编译器本来会插入的隐式填充，少了它整体会错位 4 字节。
 */
public final class Protocol {

    private Protocol() {}

    public static final int MAGIC = 0x44545541;   // 'AUTD'

    public static final int REQUEST_SIZE = 44;
    public static final int REPLY_SIZE   = 40;

    // ── 命令 ──────────────────────────────────────────────────────────────
    public static final int CMD_INFO            = 1;
    public static final int CMD_CAPTURE         = 2;
    public static final int CMD_TAP             = 3;
    public static final int CMD_SWIPE           = 4;
    public static final int CMD_TOUCH_DOWN      = 5;
    public static final int CMD_TOUCH_MOVE      = 6;
    public static final int CMD_TOUCH_UP        = 7;

    public static final int CMD_LIST_APPS       = 10;
    public static final int CMD_APP_INFO        = 11;
    public static final int CMD_LAUNCH_APP      = 12;
    public static final int CMD_KILL_APP        = 13;
    public static final int CMD_FOREGROUND_APP  = 14;
    public static final int CMD_INSTALL_APP     = 15;
    public static final int CMD_DOWNLOAD        = 16;
    public static final int CMD_FILE_OP         = 17;

    // ── flags ─────────────────────────────────────────────────────────────
    public static final int FLAG_RAW_RGBA       = 1 << 0;
    public static final int FLAG_PNG            = 1 << 1;
    public static final int FLAG_INCLUDE_SYSTEM = 1 << 3;
    public static final int FLAG_WITH_METADATA  = 1 << 4;
    public static final int FLAG_REPLACE        = 1 << 5;
    public static final int FLAG_RECURSIVE      = 1 << 6;

    // ── 状态码 ────────────────────────────────────────────────────────────
    public static final int OK              = 0;
    public static final int ERR_BAD_MAGIC   = 0x1001;
    public static final int ERR_BAD_CMD     = 0x1002;
    public static final int ERR_BAD_ARG     = 0x1003;
    public static final int ERR_NO_DISPLAY  = 0x1004;
    public static final int ERR_CAPTURED    = 0x1005;
    public static final int ERR_INJECTED    = 0x1006;
    public static final int ERR_INTERNAL    = 0x1007;
    public static final int ERR_UNSUPPORTED = 0x1008;
    public static final int ERR_NOT_FOUND   = 0x1009;
    public static final int ERR_PERMISSION  = 0x100a;
    public static final int ERR_TIMEOUT     = 0x100b;
    public static final int ERR_PAYLOAD     = 0x100c;
    public static final int ERR_IO          = 0x100d;

    public static String statusName(int s) {
        switch (s) {
            case OK:              return "ok";
            case ERR_BAD_MAGIC:   return "bad magic";
            case ERR_BAD_CMD:     return "unknown command";
            case ERR_BAD_ARG:     return "bad argument";
            case ERR_NO_DISPLAY:  return "no display";
            case ERR_CAPTURED:    return "capture failed";
            case ERR_INJECTED:    return "injection failed";
            case ERR_INTERNAL:    return "internal error";
            case ERR_UNSUPPORTED: return "unsupported";
            case ERR_NOT_FOUND:   return "not found";
            case ERR_PERMISSION:  return "permission denied";
            case ERR_TIMEOUT:     return "timeout";
            case ERR_PAYLOAD:     return "bad payload";
            case ERR_IO:          return "io error";
            default:              return String.format("0x%x", s);
        }
    }

    /** 应答头 */
    public static final class Reply {
        public int magic;
        public int status;
        public int cmd;
        public int width;
        public int height;
        public int stride;
        public int format;
        public long dataSize;

        public boolean isOk() { return status == OK; }

        public static Reply parse(byte[] buf, int offset) {
            ByteBuffer b = ByteBuffer.wrap(buf, offset, REPLY_SIZE)
                                    .order(ByteOrder.LITTLE_ENDIAN);
            Reply r = new Reply();
            r.magic    = b.getInt();
            r.status   = b.getInt();
            r.cmd      = b.getInt();
            r.width    = b.getInt();
            r.height   = b.getInt();
            r.stride   = b.getInt();
            r.format   = b.getInt();
            b.getInt();                       // reserved，显式跳过
            r.dataSize = b.getLong();
            return r;
        }
    }

    /**
     * 打包请求头。
     *
     * <p>payload 与头拼成**一条消息** —— SEQPACKET 保留消息边界，
     * 服务端按「44 字节头 + 剩余即 payload」解析，不需要长度前缀。
     */
    public static byte[] packRequest(int cmd, byte[] payload, int flags) {
        final int payloadLen = payload == null ? 0 : payload.length;
        ByteBuffer b = ByteBuffer.allocate(REQUEST_SIZE + payloadLen)
                                 .order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(MAGIC);
        b.putInt(cmd);
        b.putInt(flags);
        b.putInt(0);        // pointerId
        b.putInt(0);        // x
        b.putInt(0);        // y
        b.putInt(0);        // x2
        b.putInt(0);        // y2
        b.putInt(0);        // durationMs
        b.putFloat(0f);     // pressure
        b.putFloat(0f);     // size
        if (payloadLen > 0) b.put(payload);
        return b.array();
    }

    /** 打包触控类请求（带坐标） */
    public static byte[] packTouchRequest(int cmd, int x, int y, int x2, int y2,
                                          int durationMs, int flags) {
        ByteBuffer b = ByteBuffer.allocate(REQUEST_SIZE)
                                 .order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(MAGIC);
        b.putInt(cmd);
        b.putInt(flags);
        b.putInt(0);
        b.putInt(x);
        b.putInt(y);
        b.putInt(x2);
        b.putInt(y2);
        b.putInt(durationMs);
        b.putFloat(0f);
        b.putFloat(0f);
        return b.array();
    }

    /**
     * 把参数拼成 NUL 分隔的 payload。
     *
     * <p>协议约定：v2 命令的请求 payload 是一串 NUL 分隔的 UTF-8 字符串。
     * 字符串本身不含 NUL，所以不需要转义，也不会有歧义。
     * null 参数会被跳过（用于可选尾参数）。
     */
    public static byte[] packPayload(String... parts) {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        boolean first = true;
        for (String p : parts) {
            if (p == null) continue;
            byte[] bytes = p.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            if (!first) bos.write(0);
            bos.write(bytes, 0, bytes.length);
            first = false;
        }
        return bos.toByteArray();
    }

    /** 本机是否小端。协议统一小端，大端机器上需要额外转换（Android 都是小端） */
    public static boolean isLittleEndian() {
        return ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN;
    }
}
