package com.autod.controller;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * autod 的 Java 客户端。
 *
 * <p>为什么能直接用 Java 而不用 JNI：Android 的 {@link LocalSocket} 原生支持
 * {@code SOCK_SEQPACKET}，并且通过 {@code setFileDescriptorsForSend()} /
 * {@code getAncillaryFileDescriptors()} 暴露了 SCM_RIGHTS ——
 * 也就是协议里传 memfd 的那套机制。截图帧和 JSON 应答都能收。
 *
 * <p>回答体永远是「40 字节应答头」一条消息，正文（JSON 或像素）走 fd。
 * 所以这里的读缓冲区只需要装得下应答头，不会被大消息截断。
 */
public class AutodClient implements Closeable {

    private static final String TAG = "AutodClient";

    /** 应答头只有 40 字节，留足余量即可。SEQPACKET 下缓冲区小于消息会**丢弃**多余部分 */
    private static final int READ_BUF = 64 * 1024;

    /** 单次读取正文的上限，防服务端返回异常大的内容把内存吃光 */
    private static final long MAX_PAYLOAD = 64L * 1024 * 1024;

    public static final String DEFAULT_SOCKET = "/data/local/tmp/autod.sock";

    private LocalSocket socket;
    private InputStream in;
    private OutputStream out;
    private String path;
    private final byte[] readBuf = new byte[READ_BUF];

    // ── 连接 ────────────────────────────────────────────────────────────────

    public synchronized void connect(String socketPath) throws IOException {
        close();
        path = socketPath;

        LocalSocket s = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
        s.connect(new LocalSocketAddress(socketPath,
                LocalSocketAddress.Namespace.FILESYSTEM));
        socket = s;
        in = s.getInputStream();
        out = s.getOutputStream();
        Log.i(TAG, "已连接 " + socketPath);
    }

    public synchronized boolean isConnected() {
        return socket != null && socket.isConnected();
    }

    public String socketPath() {
        return path;
    }

    @Override
    public synchronized void close() {
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException ignored) {
                // 关闭失败没有补救手段，也不影响后续重连
            }
            socket = null;
            in = null;
            out = null;
        }
    }

    // ── 传输 ────────────────────────────────────────────────────────────────

    /** 一次事务的结果：应答头 + 正文（可能是 JSON，也可能是像素） */
    private static final class Result {
        Protocol.Reply reply;
        byte[] payload;
    }

    /**
     * 带一次重试的事务。
     *
     * <p>服务端对连接设了空闲超时（防止连上就不说话的客户端永久占资源），
     * 所以一个长时间空闲的上位应用会发现自己那条连接已经被关掉了 ——
     * 下次写入就是 Broken pipe。这里检测到失败就重连一次再发，
     * 调用方不必关心这种"睡了一觉"的情况。
     *
     * <p>带 fd 的请求**不重试**：fd 可能已经被服务端读走并关闭，
     * 拿一个失效的 fd 再发一次只会得到更难懂的错误。
     */
    private Result transact(byte[] request, FileDescriptor sendFd) throws IOException {
        try {
            return transactOnce(request, sendFd);
        } catch (IOException e) {
            if (sendFd != null || path == null) throw e;
            Log.i(TAG, "连接已失效（" + e.getMessage() + "），重连后重试");
            connect(path);
            return transactOnce(request, sendFd);
        }
    }

    private synchronized Result transactOnce(byte[] request, FileDescriptor sendFd)
            throws IOException {
        if (socket == null) throw new IOException("未连接");

        if (sendFd != null) {
            socket.setFileDescriptorsForSend(new FileDescriptor[]{sendFd});
        }

        out.write(request);
        out.flush();

        final int n = in.read(readBuf);
        if (n < 0) throw new IOException("服务端关闭了连接");
        if (n < Protocol.REPLY_SIZE) {
            throw new IOException("应答过短: " + n);
        }

        Result r = new Result();
        r.reply = Protocol.Reply.parse(readBuf, 0);

        if (r.reply.magic != Protocol.MAGIC) {
            throw new IOException(String.format("magic 不匹配: 0x%x", r.reply.magic));
        }

        // 正文走 fd。即便命令失败也带正文 —— 里面是服务端给的可读原因，
        // 所以先读出来再判断状态，报错才是原话而不是"internal error"。
        FileDescriptor[] fds = socket.getAncillaryFileDescriptors();
        if (fds != null && fds.length > 0) {
            // readAll 里的 FileInputStream 关闭时会关掉这个 fd ——
            // 它是 LocalSocket 从 SCM_RIGHTS 收到的，由我们负责关闭。
            r.payload = readAll(fds[0], r.reply.dataSize);
        }
        return r;
    }

    private static byte[] readAll(FileDescriptor fd, long expected) throws IOException {
        long limit = expected > 0 ? expected : MAX_PAYLOAD;
        if (limit > MAX_PAYLOAD) limit = MAX_PAYLOAD;

        ByteArrayOutputStream bos = new ByteArrayOutputStream(
                expected > 0 && expected < (1 << 22) ? (int) expected : 8192);
        try (FileInputStream fis = new FileInputStream(fd)) {
            byte[] buf = new byte[64 * 1024];
            long total = 0;
            int n;
            while ((n = fis.read(buf)) > 0) {
                if (total + n > limit) {
                    bos.write(buf, 0, (int) (limit - total));
                    break;
                }
                bos.write(buf, 0, n);
                total += n;
            }
        }
        return bos.toByteArray();
    }

    /** 解析 JSON 正文；失败时抛出带服务端原文的异常 */
    private static JSONObject parseJson(Result r) throws IOException {
        String text = r.payload == null ? "" : new String(r.payload, StandardCharsets.UTF_8);

        if (!r.reply.isOk()) {
            // v2 的失败应答也带 JSON，优先用里面的 error 字段
            String reason = null;
            if (!text.isEmpty()) {
                try {
                    reason = new JSONObject(text).optString("error", null);
                } catch (Exception ignored) {
                }
            }
            throw new IOException(reason != null
                    ? reason
                    : Protocol.statusName(r.reply.status)
                      + String.format(" (0x%x)", r.reply.status));
        }

        if (text.isEmpty()) return new JSONObject();
        // org.json 的 JSONException 是**受检**异常；转成 IOException 让调用方
        // 统一处理 —— 对使用者来说"服务端返回了坏 JSON"和"读 socket 失败"
        // 没有区别，都是这条链路断了。
        try {
            return new JSONObject(text);
        } catch (org.json.JSONException e) {
            throw new IOException("服务端返回的不是合法 JSON: " + e.getMessage(), e);
        }
    }

    // ── 高层 API ────────────────────────────────────────────────────────────

    /** 状态查询：显示参数 */
    public JSONObject info() throws IOException {
        Result r = transact(Protocol.packRequest(Protocol.CMD_INFO, null, 0), null);
        JSONObject o = new JSONObject();
        try {
            o.put("width", r.reply.width);
            o.put("height", r.reply.height);
            if (!r.reply.isOk()) {
                o.put("error", Protocol.statusName(r.reply.status));
            }
        } catch (Exception ignored) {
        }
        return o;
    }

    /** 截图。返回原始像素，尺寸与格式通过 out 参数带回。 */
    public byte[] capture(int[] outW, int[] outH, int[] outFormat, int flags)
            throws IOException {
        Result r = transact(Protocol.packRequest(Protocol.CMD_CAPTURE, null, flags), null);
        if (!r.reply.isOk()) {
            throw new IOException(Protocol.statusName(r.reply.status));
        }
        if (outW != null) outW[0] = r.reply.width;
        if (outH != null) outH[0] = r.reply.height;
        if (outFormat != null) outFormat[0] = r.reply.format;
        return r.payload;
    }

    public void tap(int x, int y, int ms) throws IOException {
        Result r = transact(Protocol.packTouchRequest(
                Protocol.CMD_TAP, x, y, 0, 0, ms, 0), null);
        if (!r.reply.isOk()) throw new IOException(Protocol.statusName(r.reply.status));
    }

    public void swipe(int x1, int y1, int x2, int y2, int ms) throws IOException {
        Result r = transact(Protocol.packTouchRequest(
                Protocol.CMD_SWIPE, x1, y1, x2, y2, ms, 0), null);
        if (!r.reply.isOk()) throw new IOException(Protocol.statusName(r.reply.status));
    }

    // ── v2：应用与文件 ──────────────────────────────────────────────────────

    public JSONObject listApps(boolean includeSystem, boolean withMetadata)
            throws IOException {
        int flags = 0;
        if (includeSystem) flags |= Protocol.FLAG_INCLUDE_SYSTEM;
        if (withMetadata)  flags |= Protocol.FLAG_WITH_METADATA;
        return parseJson(transact(
                Protocol.packRequest(Protocol.CMD_LIST_APPS, null, flags), null));
    }

    public JSONObject appInfo(String pkg) throws IOException {
        return parseJson(transact(Protocol.packRequest(Protocol.CMD_APP_INFO,
                Protocol.packPayload(pkg), 0), null));
    }

    public JSONObject launchApp(String pkg, String activity) throws IOException {
        return parseJson(transact(Protocol.packRequest(Protocol.CMD_LAUNCH_APP,
                Protocol.packPayload(pkg, activity), 0), null));
    }

    public JSONObject killApp(String pkg) throws IOException {
        return parseJson(transact(Protocol.packRequest(Protocol.CMD_KILL_APP,
                Protocol.packPayload(pkg), 0), null));
    }

    public JSONObject foregroundApp() throws IOException {
        return parseJson(transact(Protocol.packRequest(
                Protocol.CMD_FOREGROUND_APP, null, 0), null));
    }

    /**
     * 安装 APK。
     *
     * <p>用 {@link ParcelFileDescriptor} 打开 APK，把 fd 交给 SCM_RIGHTS 送过去 ——
     * 而不是把几 MB 的字节塞进请求 payload（SEQPACKET 单条消息约 208KB 上限）。
     */
    public JSONObject installApp(String apkPath, boolean replace) throws IOException {
        try (ParcelFileDescriptor pfd =
                     ParcelFileDescriptor.open(new java.io.File(apkPath),
                             ParcelFileDescriptor.MODE_READ_ONLY)) {
            int flags = replace ? Protocol.FLAG_REPLACE : 0;
            return parseJson(transact(
                    Protocol.packRequest(Protocol.CMD_INSTALL_APP, null, flags),
                    pfd.getFileDescriptor()));
        }
    }

    public JSONObject download(String url, String filename, String subdir)
            throws IOException {
        return parseJson(transact(Protocol.packRequest(Protocol.CMD_DOWNLOAD,
                Protocol.packPayload(url, filename, subdir), 0), null));
    }

    public JSONObject fileOp(String op, String path, String arg2, int flags)
            throws IOException {
        return parseJson(transact(Protocol.packRequest(Protocol.CMD_FILE_OP,
                Protocol.packPayload(op, path, arg2), flags), null));
    }

    public JSONObject listDir(String path) throws IOException {
        return fileOp("list", path, null, 0);
    }

    public JSONObject delete(String path, boolean recursive) throws IOException {
        return fileOp("delete", path, null,
                recursive ? Protocol.FLAG_RECURSIVE : 0);
    }

    public JSONObject mkdir(String path, boolean parents) throws IOException {
        return fileOp("mkdir", path, null,
                parents ? Protocol.FLAG_RECURSIVE : 0);
    }
}
