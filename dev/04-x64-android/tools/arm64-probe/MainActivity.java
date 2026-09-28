package org.autosnap.arm64probe;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * 探针 Activity：只做一件事——把 arm64 原生库的结论打出来。
 *
 * 判定依据（run-linux.sh / run-windows.ps1 会检查）：
 *   1. 静态初始化块 System.loadLibrary("arm64probe") 成功
 *      —— 说明系统接受了这个 APK 的 arm64-v8a ABI 并完成了加载；
 *   2. 原生方法返回的字符串里带 "arm64-v8a"
 *      —— 说明真的执行了 aarch64 机器码（不是恰好有个 x86_64 副本）。
 */
public class MainActivity extends Activity {

    private static final String TAG = "ARM64PROBE";

    static {
        System.loadLibrary("arm64probe");
    }

    /** 在 arm64 机器码里拼出来的字符串。 */
    public static native String abiInfo();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String s;
        try {
            s = abiInfo();
        } catch (Throwable t) {
            s = "NATIVE_FAILED: " + t;
        }
        Log.i(TAG, "PROBE_RESULT " + s);
        // 再补一条 Java 侧的 ABI 列表（Build.SUPPORTED_ABIS 来自 ro.product.cpu.abilist），
        // 便于从 logcat 一眼看出系统声明的 ABI 顺序
        Log.i(TAG, "PROBE_SYS abis=" + android.text.TextUtils.join(",", android.os.Build.SUPPORTED_ABIS));
        TextView tv = new TextView(this);
        tv.setText("ARM64 probe\n" + s);
        setContentView(tv);
    }
}
