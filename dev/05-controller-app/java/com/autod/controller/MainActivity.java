// MainActivity.java — autod 服务管理器
//
// 这个应用**只做三件事**：
//   1. 启动 / 停止 autod 服务
//   2. 改它的对外监听端口
//   3. 开关接口访问鉴权
//
// 为什么把原来的画面、触控、应用管理、文件管理全删了：
//
//   那些能力属于"控制设备"，而它们已经在网页控制台里了（那里有实时画面、
//   流式触控、按键、应用列表……而且不用装在设备上）。应用再做一遍，
//   等于两套 UI 各维护一遍，行为还容易不一致。
//
//   应用真正的独有价值是**它是设备本地的**：服务没起来、网络不通、
//   端口改错了导致连不上 —— 这些情况下网页控制台自己也进不去，
//   只有本机应用还能把服务拉回来。所以它就该只做这件事。
//
// 怎么做到"没有 root 也能启停 root 服务"：
//
//   应用改不了进程，但它能写共享存储。配置写在 /sdcard/autod.conf，
//   由常驻的 autod-supervisord 监视并执行真正的启停。
//   应用侧因此只依赖"文件能写"，不需要任何特权。
//
// ⚠️ 写 /sdcard 需要 MANAGE_EXTERNAL_STORAGE（Android 11+ 的
//    "所有文件访问"）。没有它的话应用只能写自己的私有目录，
//    守护进程就读不到了。

package com.autod.controller;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

public class MainActivity extends Activity {

    /** 与 autod-supervisord 约定的路径 */
    private static final String CONF   = "/sdcard/autod.conf";
    private static final String STATUS = "/sdcard/autod.status";

    private TextView statusView;
    private TextView tokenView;
    private Switch   enableSwitch;
    private Switch   authSwitch;
    private EditText portEdit;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean suppressCallbacks = false;   // 程序化改开关时别触发保存

    // ── 生命周期 ────────────────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        ensureStorageAccess();
    }

    @Override
    protected void onResume() {
        super.onResume();
        reload();
        ui.postDelayed(poller, 1000);
    }

    @Override
    protected void onPause() {
        super.onPause();
        ui.removeCallbacks(poller);
    }

    private final Runnable poller = new Runnable() {
        @Override public void run() {
            refreshStatus();
            ui.postDelayed(this, 1000);
        }
    };

    // ── 存储权限 ────────────────────────────────────────────────────────────
    //
    // Android 11+ 里往 /sdcard 根目录写文件需要"所有文件访问"，
    // 而这个权限只能在系统设置里由用户授予 —— 应用不能自己弹窗申请。
    // 所以这里只负责把用户送过去，并说明为什么需要。

    private void ensureStorageAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        if (Environment.isExternalStorageManager()) return;

        toast("需要「所有文件访问」权限才能改写服务配置");
        try {
            Intent i = new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            // 有些 ROM 没有这个页面，退回到总列表
            try {
                startActivity(new Intent(
                        Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                toast("请手动到系统设置里授予「所有文件访问」");
            }
        }
    }

    // ── 界面 ────────────────────────────────────────────────────────────────

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private TextView label(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(12);
        t.setPadding(dp(12), dp(10), dp(12), dp(4));
        t.setTextColor(0xFF888888);
        return t;
    }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(13);
        b.setOnClickListener(l);
        return b;
    }

    private View buildUi() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        // 标题
        TextView title = new TextView(this);
        title.setText("autod 服务管理");
        title.setTextSize(19);
        title.setTypeface(null, Typeface.BOLD);
        title.setPadding(dp(16), dp(16), dp(16), dp(4));
        title.setTextColor(0xFF111111);
        col.addView(title);

        TextView sub = new TextView(this);
        sub.setText("只管服务的启停、端口与鉴权\n（控制设备请用网页控制台）");
        sub.setTextSize(12);
        sub.setPadding(dp(16), 0, dp(16), dp(10));
        sub.setTextColor(0xFF666666);
        col.addView(sub);

        // ── 状态 ──
        col.addView(label("服务状态"));
        statusView = new TextView(this);
        statusView.setTextSize(13);
        statusView.setPadding(dp(16), 0, dp(16), dp(8));
        statusView.setTextColor(0xFF222222);
        statusView.setTypeface(Typeface.MONOSPACE);
        col.addView(statusView);

        LinearLayout stRow = new LinearLayout(this);
        stRow.setPadding(dp(12), 0, dp(12), dp(8));
        stRow.addView(button("刷新", v -> { reload(); refreshStatus(); }));
        stRow.addView(button("重新读取配置", v -> reload()));
        col.addView(stRow);

        // ── 启停 ──
        col.addView(label("启动 / 停止"));
        LinearLayout enRow = new LinearLayout(this);
        enRow.setGravity(Gravity.CENTER_VERTICAL);
        enRow.setPadding(dp(16), 0, dp(16), dp(8));
        enableSwitch = new Switch(this);
        enableSwitch.setText("服务运行中");
        enableSwitch.setTextSize(14);
        enableSwitch.setOnCheckedChangeListener((b, checked) -> {
            if (suppressCallbacks) return;
            save("enabled", checked ? "1" : "0",
                 checked ? "正在启动服务…" : "正在停止服务…");
        });
        enRow.addView(enableSwitch);
        col.addView(enRow);

        // ── 端口 ──
        col.addView(label("对外监听端口"));
        LinearLayout pRow = new LinearLayout(this);
        pRow.setGravity(Gravity.CENTER_VERTICAL);
        pRow.setPadding(dp(16), 0, dp(16), dp(4));
        portEdit = new EditText(this);
        portEdit.setInputType(InputType.TYPE_CLASS_NUMBER);
        portEdit.setTextSize(14);
        portEdit.setHint("1024-65535");
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        portEdit.setLayoutParams(lp);
        pRow.addView(portEdit);
        pRow.addView(button("保存", v -> savePort()));
        col.addView(pRow);

        TextView portHint = new TextView(this);
        portHint.setText("改完 supervisor 会自动重启服务。\n"
                + "连不上时先看这里 —— 端口写错是唯一能把你自己关在门外又\n"
                + "只有本机应用能救回来的情况。");
        portHint.setTextSize(11);
        portHint.setPadding(dp(16), 0, dp(16), dp(8));
        portHint.setTextColor(0xFF777777);
        col.addView(portHint);

        // ── 鉴权 ──
        col.addView(label("接口访问鉴权"));
        LinearLayout aRow = new LinearLayout(this);
        aRow.setGravity(Gravity.CENTER_VERTICAL);
        aRow.setPadding(dp(16), 0, dp(16), dp(4));
        authSwitch = new Switch(this);
        authSwitch.setText("要求访问令牌");
        authSwitch.setTextSize(14);
        authSwitch.setOnCheckedChangeListener((b, checked) -> {
            if (suppressCallbacks) return;
            save("auth", checked ? "1" : "0",
                 checked ? "已开启鉴权（首次会自动生成令牌）" : "已关闭鉴权");
        });
        aRow.addView(authSwitch);
        col.addView(aRow);

        tokenView = new TextView(this);
        tokenView.setTextSize(12);
        tokenView.setPadding(dp(16), 0, dp(16), dp(6));
        tokenView.setTextColor(0xFF1A7A4A);
        tokenView.setTypeface(Typeface.MONOSPACE);
        tokenView.setTextIsSelectable(true);
        col.addView(tokenView);

        LinearLayout tRow = new LinearLayout(this);
        tRow.setPadding(dp(12), 0, dp(12), dp(12));
        tRow.addView(button("复制令牌", v -> copyToken()));
        tRow.addView(button("重新生成", v -> {
            if (!authSwitch.isChecked()) { toast("先开启鉴权"); return; }
            save("token", "", "已清空令牌，重启后会生成新的");
        }));
        col.addView(tRow);

        // 安全提示：这个开关很容易被随手打开又随手关掉
        TextView warn = new TextView(this);
        warn.setText("⚠️ 关闭鉴权后，同一网络里的任何人都能完全控制本设备：\n"
                + "看屏幕、点屏幕、按键、读剪贴板、装应用、删文件。");
        warn.setTextSize(11);
        warn.setPadding(dp(16), 0, dp(16), dp(20));
        warn.setTextColor(0xFFB45309);
        col.addView(warn);

        ScrollView sv = new ScrollView(this);
        // 白底。早先用了深色（0xFF111111）配浅色文字，
        // 但这个应用是"设备管理"性质的工具页，和系统设置一类；
        // 浅色更符合用户对这类界面的预期，在强光下也更清楚。
        sv.setBackgroundColor(0xFFFFFFFF);
        sv.addView(col);
        return sv;
    }

    // ── 配置读写 ────────────────────────────────────────────────────────────
    //
    // 格式与 ConfigFile::Serialize()（守护进程侧）必须一致：
    // 纯 key=value、# 开头是注释。这样两边都不需要 JSON 库，
    // 而且用户能直接用编辑器看懂和改。

    private Map<String, String> readConf() {
        Map<String, String> m = new LinkedHashMap<>();
        File f = new File(CONF);
        if (!f.exists()) return m;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(
                new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (Exception e) {
            toast("读配置失败：" + e.getMessage());
        }
        return m;
    }

    /** 只改一个字段，保留文件里其它内容和注释。 */
    private void save(String key, String value, String toastText) {
        Map<String, String> m = readConf();
        m.put(key, value);

        StringBuilder sb = new StringBuilder();
        sb.append("# autod 配置 —— 由上位应用或手工编辑，守护进程启动时读取。\n");
        sb.append("# 改完之后需要重启服务才生效。\n\n");
        sb.append("# 服务是否应当运行\n");
        sb.append("enabled=").append(m.containsKey("enabled") ? m.get("enabled") : "1").append('\n');
        sb.append("\n# 监听地址。127.0.0.1 = 仅本机；0.0.0.0 = 对外（注意鉴权设置）\n");
        sb.append("bind=").append(m.containsKey("bind") ? m.get("bind") : "127.0.0.1").append('\n');
        sb.append("\n# HTTP 监听端口\n");
        sb.append("port=").append(m.containsKey("port") ? m.get("port") : "8088").append('\n');
        sb.append("\n# 是否要求访问令牌。0 = 无鉴权（任何人都能访问接口）\n");
        sb.append("auth=").append(m.containsKey("auth") ? m.get("auth") : "0").append('\n');
        sb.append("\n# 访问令牌。auth=1 时若为空，守护进程启动时会随机生成并写回这里。\n");
        sb.append("token=").append(m.containsKey("token") ? m.get("token") : "").append('\n');

        try (OutputStreamWriter w = new OutputStreamWriter(
                new FileOutputStream(CONF + ".tmp"), StandardCharsets.UTF_8)) {
            w.write(sb.toString());
        } catch (Exception e) {
            toast("写配置失败：" + e.getMessage()
                    + "\n（多半是没有「所有文件访问」权限）");
            reload();
            return;
        }
        // 原子替换：守护进程可能正在读，不能让它看到半个文件
        File tmp = new File(CONF + ".tmp");
        File dst = new File(CONF);
        if (!tmp.renameTo(dst)) {
            toast("替换配置失败");
            reload();
            return;
        }
        if (toastText != null) toast(toastText);
        ui.postDelayed(this::reload, 700);
    }

    private void savePort() {
        String s = portEdit.getText().toString().trim();
        int p;
        try {
            p = Integer.parseInt(s);
        } catch (Exception e) {
            toast("端口必须是数字");
            return;
        }
        if (p < 1 || p > 65535) {
            toast("端口范围 1-65535");
            return;
        }
        if (p < 1024) {
            // 1024 以下是特权端口，root 进程能绑，但很容易和系统服务撞上
            toast("提示：" + p + " 是特权端口，可能和系统服务冲突");
        }
        save("port", String.valueOf(p), "端口已改为 " + p + "，正在重启服务…");
    }

    private void copyToken() {
        Map<String, String> m = readConf();
        String t = m.get("token");
        if (t == null || t.isEmpty()) { toast("当前没有令牌"); return; }
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("autod token", t));
        toast("令牌已复制");
    }

    // ── 状态显示 ────────────────────────────────────────────────────────────
    //
    // 状态来自 supervisor 写的 /sdcard/autod.status。
    // 不去探端口：端口可能被转发规则挡住，"进程在不在"才是确定的；
    // 而且服务没起来时探端口只会得到"连不上"，分不清是挂了还是没启动。

    private void reload() {
        Map<String, String> c = readConf();
        suppressCallbacks = true;
        enableSwitch.setChecked(!"0".equals(c.get("enabled")));
        authSwitch.setChecked("1".equals(c.get("auth")));
        portEdit.setText(c.containsKey("port") ? c.get("port") : "8088");
        suppressCallbacks = false;

        String t = c.get("token");
        boolean auth = "1".equals(c.get("auth"));
        if (!auth) {
            tokenView.setText("令牌：—（无鉴权模式）");
            tokenView.setTextColor(0xFFB45309);
        } else if (t == null || t.isEmpty()) {
            tokenView.setText("令牌：（重启服务后自动生成）");
            tokenView.setTextColor(0xFF8A6D00);
        } else {
            tokenView.setText("令牌：" + t);
            tokenView.setTextColor(0xFFAACCAA);
        }
        refreshStatus();
    }

    private void refreshStatus() {
        Map<String, String> s = new LinkedHashMap<>();
        File f = new File(STATUS);
        if (f.exists()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(
                    new FileInputStream(f), StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) {
                    int eq = line.indexOf('=');
                    if (eq > 0) s.put(line.substring(0, eq).trim(),
                                     line.substring(eq + 1).trim());
                }
            } catch (Exception ignored) {
            }
        }

        boolean running = "1".equals(s.get("running"));
        StringBuilder sb = new StringBuilder();
        sb.append(running ? "● 运行中" : "○ 已停止").append('\n');
        if (running) {
            sb.append("  pid   ").append(s.getOrDefault("pid", "?")).append('\n');
            sb.append("  监听  ").append(s.getOrDefault("bind", "?"))
              .append(':').append(s.getOrDefault("port", "?")).append('\n');
            String b = s.get("bind");
            if ("0.0.0.0".equals(b)) {
                sb.append("  ⚠️ 对外监听");
            } else {
                sb.append("  仅本机");
            }
        } else if (!f.exists()) {
            sb.append("  （supervisor 未运行，状态文件不存在）");
        }
        statusView.setText(sb.toString());
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }
}
