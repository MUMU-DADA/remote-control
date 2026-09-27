package com.autod.controller;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * autod 上位应用：对服务做控制与状态查询。
 *
 * <p>界面是代码构建的而不是 XML 布局 —— 这是一个控制/调试工具，
 * 功能密度比视觉表现重要，代码构建也省掉一整层资源编译。
 *
 * <p>所有 socket 调用都走后台线程：Android 在主线程做阻塞 IO 会直接
 * 抛 NetworkOnMainThreadException，而且卡 UI 本身也是不可接受的。
 */
public class MainActivity extends Activity {

    private static final String TAG = "AutodUI";

    private final AutodClient client = new AutodClient();
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());

    private TextView statusView;
    private EditText socketEdit;
    private FrameLayout content;
    private Button[] tabs;

    private int currentTab = 0;
    private String currentDir = "";        // 文件页当前目录（相对下载目录）
    private List<AppListAdapter.Item> apps = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        showTab(0);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        client.close();
        io.shutdownNow();
    }

    // ── 后台任务 ────────────────────────────────────────────────────────────

    private interface Job { Object run() throws Exception; }

    private interface Done { void ok(Object result); }

    /** 在后台线程跑 socket 调用，结果回主线程 */
    private void runAsync(String what, Job job, Done onDone) {
        io.execute(() -> {
            try {
                final Object r = job.run();
                ui.post(() -> {
                    if (onDone != null) onDone.ok(r);
                });
            } catch (final Exception e) {
                Log.w(TAG, what + " 失败", e);
                ui.post(() -> {
                    setStatus(what + " 失败: " + e.getMessage(), true);
                    toast(what + " 失败: " + e.getMessage());
                });
            }
        });
    }

    private void setStatus(String text, boolean error) {
        if (statusView != null) {
            statusView.setText(text);
            statusView.setTextColor(error ? 0xFFD32F2F : 0xFF2E7D32);
        }
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    // ── 界面构建 ────────────────────────────────────────────────────────────

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        // 连接栏
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setPadding(dp(8), dp(8), dp(8), dp(4));
        bar.setGravity(Gravity.CENTER_VERTICAL);

        socketEdit = new EditText(this);
        socketEdit.setText(AutodClient.DEFAULT_SOCKET);
        socketEdit.setSingleLine(true);
        socketEdit.setTextSize(12);
        bar.addView(socketEdit, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button connect = new Button(this);
        connect.setText("连接");
        connect.setOnClickListener(v -> doConnect());
        bar.addView(connect);
        root.addView(bar);

        statusView = new TextView(this);
        statusView.setPadding(dp(10), dp(2), dp(10), dp(6));
        statusView.setTextSize(12);
        statusView.setText("未连接");
        root.addView(statusView);

        // 标签栏
        LinearLayout tabBar = new LinearLayout(this);
        tabBar.setOrientation(LinearLayout.HORIZONTAL);
        String[] names = {"状态", "应用", "文件", "控制"};
        tabs = new Button[names.length];
        for (int i = 0; i < names.length; i++) {
            final int idx = i;
            Button b = new Button(this);
            b.setText(names[i]);
            b.setTextSize(13);
            b.setOnClickListener(v -> showTab(idx));
            tabs[i] = b;
            tabBar.addView(b, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
        root.addView(tabBar);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        return root;
    }

    private void showTab(int idx) {
        currentTab = idx;
        for (int i = 0; i < tabs.length; i++) {
            tabs[i].setEnabled(i != idx);
        }
        switch (idx) {
            case 0: content.removeAllViews(); content.addView(buildStatusTab()); break;
            case 1: content.removeAllViews(); content.addView(buildAppsTab()); break;
            case 2: content.removeAllViews(); content.addView(buildFilesTab()); break;
            default: content.removeAllViews(); content.addView(buildControlTab()); break;
        }
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(13);
        b.setOnClickListener(l);
        return b;
    }

    private TextView label(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(13);
        t.setPadding(dp(8), dp(6), dp(8), dp(2));
        return t;
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setPadding(dp(6), dp(2), dp(6), dp(2));
        return r;
    }

    private EditText field(String hint, int weight) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextSize(13);
        e.setSingleLine(true);
        e.setInputType(InputType.TYPE_CLASS_TEXT);
        if (weight > 0) {
            e.setLayoutParams(new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, weight));
        }
        return e;
    }

    // ── 页 1：状态 ──────────────────────────────────────────────────────────

    // ⚠️ 只声明，**不要**在这里 new。
    //    字段初始化器在 onCreate 之前、Activity 还没挂到 Context 上时执行，
    //    那时 this.getResources() 是 null，new TextView(this) 会直接
    //    NPE 崩在 <init> 里 —— 实测就是这样崩的。
    //    所有依赖 Context 的 View 一律在 onCreate/buildUi 里创建。
    private TextView statusDetail;

    private View buildStatusTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        sv.addView(col);

        LinearLayout r = row();
        r.addView(button("刷新状态", v -> refreshStatus()), new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(button("查询前台应用", v -> refreshForeground()),
                new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        col.addView(r);

        statusDetail = new TextView(this);
        statusDetail.setTextSize(13);
        statusDetail.setPadding(dp(10), dp(8), dp(10), dp(8));
        statusDetail.setTextIsSelectable(true);
        statusDetail.setText("点「刷新状态」查询");
        col.addView(statusDetail);
        return sv;
    }

    private void doConnect() {
        final String path = socketEdit.getText().toString().trim();
        setStatus("连接中…", false);
        runAsync("连接", () -> {
            client.connect(path);
            return path;
        }, o -> {
            setStatus("已连接 " + o, false);
            refreshStatus();
        });
    }

    private void refreshStatus() {
        runAsync("查询状态", () -> {
            JSONObject info = client.info();
            JSONObject fg = null;
            try {
                fg = client.foregroundApp();
            } catch (Exception e) {
                // 息屏或开机中没有前台 Activity，不是错误
                fg = new JSONObject().put("error", e.getMessage());
            }
            JSONObject both = new JSONObject();
            both.put("display", info);
            both.put("foreground", fg);
            both.put("socket", client.socketPath());
            return both;
        }, o -> {
            try {
                JSONObject both = (JSONObject) o;
                JSONObject d = both.getJSONObject("display");
                JSONObject f = both.getJSONObject("foreground");

                StringBuilder sb = new StringBuilder();
                sb.append("socket: ").append(both.optString("socket")).append('\n');
                sb.append("显示: ")
                  .append(d.optInt("width")).append(" x ")
                  .append(d.optInt("height")).append('\n');
                sb.append('\n').append("当前前台应用:\n");
                if (f.has("package")) {
                    sb.append("  包名  : ").append(f.optString("package")).append('\n');
                    sb.append("  Activity: ").append(f.optString("activity")).append('\n');
                    sb.append("  pid   : ").append(f.optInt("pid")).append('\n');
                    sb.append("  userId: ").append(f.optInt("userId")).append('\n');
                } else {
                    sb.append("  ").append(f.optString("error", "无")).append('\n');
                }

                if (statusDetail != null) statusDetail.setText(sb.toString());
                setStatus("状态已更新", false);
            } catch (Exception e) {
                setStatus("解析失败: " + e.getMessage(), true);
            }
        });
    }

    private void refreshForeground() {
        runAsync("查询前台应用", () -> client.foregroundApp(), o -> {
            JSONObject f = (JSONObject) o;
            new AlertDialog.Builder(this)
                    .setTitle("当前前台应用")
                    .setMessage("包名: " + f.optString("package")
                            + "\nActivity: " + f.optString("activity")
                            + "\npid: " + f.optInt("pid"))
                    .setPositiveButton("知道了", null)
                    .show();
        });
    }

    // ── 页 2：应用 ──────────────────────────────────────────────────────────

    private AppListAdapter appAdapter;
    private CheckBox systemCheck;
    private ListView appList;

    private View buildAppsTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        LinearLayout r = row();
        systemCheck = new CheckBox(this);
        systemCheck.setText("含系统应用");
        systemCheck.setTextSize(13);
        r.addView(systemCheck);
        r.addView(button("刷新", v -> refreshApps()));
        r.addView(button("当前前台", v -> refreshForeground()));
        col.addView(r);

        appList = new ListView(this);
        appAdapter = new AppListAdapter(this);
        appList.setAdapter(appAdapter);
        appList.setOnItemClickListener(this::onAppClicked);
        appList.setOnItemLongClickListener((p, v, pos, id) -> {
            showAppMenu(appAdapter.getItemAt(pos));
            return true;
        });
        col.addView(appList, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return col;
    }

    private void refreshApps() {
        final boolean withSystem = systemCheck.isChecked();
        setStatus("拉取应用列表…", false);
        runAsync("拉取应用列表", () -> client.listApps(withSystem, true), o -> {
            try {
                JSONObject doc = (JSONObject) o;
                JSONArray arr = doc.optJSONArray("apps");
                List<AppListAdapter.Item> list = new ArrayList<>();
                if (arr != null) {
                    for (int i = 0; i < arr.length(); i++) {
                        JSONObject a = arr.getJSONObject(i);
                        AppListAdapter.Item it = new AppListAdapter.Item();
                        it.pkg = a.optString("package");
                        it.versionCode = a.optLong("versionCode");
                        it.system = a.optBoolean("system");
                        it.apkPath = a.optString("apkPath", null);
                        list.add(it);
                    }
                }
                apps = list;
                appAdapter.setItems(list);
                setStatus("共 " + list.size() + " 个应用（点=启动，长按=更多）", false);
            } catch (Exception e) {
                setStatus("解析失败: " + e.getMessage(), true);
            }
        });
    }

    private void onAppClicked(AdapterView<?> parent, View view, int pos, long id) {
        AppListAdapter.Item it = appAdapter.getItemAt(pos);
        setStatus("启动 " + it.pkg + " …", false);
        runAsync("启动应用", () -> client.launchApp(it.pkg, null),
                o -> setStatus("已启动 " + it.pkg, false));
    }

    private void showAppMenu(AppListAdapter.Item it) {
        String[] actions = {"查看详情 / 清单", "强制停止", "启动"};
        new AlertDialog.Builder(this)
                .setTitle(it.pkg)
                .setItems(actions, (d, which) -> {
                    switch (which) {
                        case 0: showAppInfo(it.pkg); break;
                        case 1: killApp(it.pkg); break;
                        default: runAsync("启动应用",
                                () -> client.launchApp(it.pkg, null),
                                o -> setStatus("已启动 " + it.pkg, false));
                    }
                })
                .show();
    }

    private void killApp(String pkg) {
        setStatus("停止 " + pkg + " …", false);
        runAsync("停止应用", () -> client.killApp(pkg), o -> {
            JSONObject r = (JSONObject) o;
            boolean still = r.optBoolean("stillRunning", false);
            setStatus("已停止 " + pkg + (still ? "（但进程仍在）" : ""), still);
        });
    }

    private void showAppInfo(String pkg) {
        setStatus("读取 " + pkg + " 清单…", false);
        runAsync("读取应用清单", () -> client.appInfo(pkg), o -> {
            JSONObject d = (JSONObject) o;
            StringBuilder sb = new StringBuilder();
            sb.append("版本: ").append(d.optString("versionName"))
              .append(" (").append(d.optLong("versionCode")).append(")\n");
            sb.append("uid: ").append(d.optInt("uid"))
              .append("  SDK: ").append(d.optInt("minSdk"))
              .append("/").append(d.optInt("targetSdk")).append('\n');
            sb.append("系统应用: ").append(d.optBoolean("system") ? "是" : "否").append('\n');
            sb.append("签名: ").append(d.optString("signatureDigest")).append('\n');
            sb.append("安装: ").append(d.optString("firstInstallTime")).append('\n');
            sb.append("路径: ").append(d.optString("apkPath")).append('\n');
            sb.append('\n');
            appendArray(sb, "权限", d.optJSONArray("permissions"));
            appendArray(sb, "Activity", d.optJSONArray("activities"));
            appendArray(sb, "Service", d.optJSONArray("services"));
            appendArray(sb, "Receiver", d.optJSONArray("receivers"));
            appendArray(sb, "Provider", d.optJSONArray("providers"));

            ScrollView sv = new ScrollView(this);
            TextView tv = new TextView(this);
            tv.setText(sb.toString());
            tv.setTextSize(12);
            tv.setTextIsSelectable(true);
            tv.setPadding(dp(16), dp(8), dp(16), dp(8));
            sv.addView(tv);

            new AlertDialog.Builder(this)
                    .setTitle(pkg)
                    .setView(sv)
                    .setPositiveButton("关闭", null)
                    .show();
            setStatus("已读取 " + pkg + " 清单", false);
        });
    }

    private void appendArray(StringBuilder sb, String title, JSONArray arr) {
        int n = arr == null ? 0 : arr.length();
        sb.append(title).append("（").append(n).append("）\n");
        if (arr == null) return;
        int show = Math.min(n, 20);
        for (int i = 0; i < show; i++) {
            sb.append("  ").append(arr.optString(i)).append('\n');
        }
        if (n > show) sb.append("  … 还有 ").append(n - show).append(" 项\n");
        sb.append('\n');
    }

    // ── 页 3：文件 ──────────────────────────────────────────────────────────

    private ArrayAdapter<String> fileAdapter;
    private final List<String> fileLines = new ArrayList<>();
    private final List<JSONObject> fileEntries = new ArrayList<>();
    private TextView pathView;

    private View buildFilesTab() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);

        pathView = label("下载目录: /");
        col.addView(pathView);

        LinearLayout r1 = row();
        r1.addView(button("刷新", v -> refreshFiles()));
        r1.addView(button("上级", v -> goUp()));
        r1.addView(button("新建目录", v -> promptMkdir()));
        r1.addView(button("删除选中", v -> promptDelete()));
        col.addView(r1);

        LinearLayout r2 = row();
        final EditText urlEdit = field("下载 URL（https://…）", 1);
        r2.addView(urlEdit);
        r2.addView(button("下载", v -> {
            final String url = urlEdit.getText().toString().trim();
            if (url.isEmpty()) { toast("请填 URL"); return; }
            setStatus("下载中…", false);
            runAsync("下载", () -> client.download(url, null, currentDir),
                    o -> {
                        JSONObject d = (JSONObject) o;
                        setStatus("已下载 " + d.optString("path")
                                + "（" + d.optLong("bytes") + " 字节）", false);
                        refreshFiles();
                    });
        }));
        col.addView(r2);

        ListView lv = new ListView(this);
        fileAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_1, fileLines);
        lv.setAdapter(fileAdapter);
        lv.setOnItemClickListener((p, v, pos, id) -> {
            JSONObject e = fileEntries.get(pos);
            if (e.optBoolean("dir")) {
                String name = e.optString("name");
                currentDir = currentDir.isEmpty() ? name : currentDir + "/" + name;
                refreshFiles();
            } else {
                toast(e.optString("name") + "  " + e.optLong("size") + " 字节");
            }
        });
        lv.setOnItemLongClickListener((p, v, pos, id) -> {
            showFileMenu(fileEntries.get(pos));
            return true;
        });
        col.addView(lv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        return col;
    }

    private void refreshFiles() {
        final String dir = currentDir;
        runAsync("列出目录", () -> client.listDir(dir), o -> {
            try {
                JSONObject doc = (JSONObject) o;
                JSONArray arr = doc.optJSONArray("entries");
                fileLines.clear();
                fileEntries.clear();
                pathView.setText("下载目录: /" + (dir.isEmpty() ? "" : dir)
                        + "   (" + doc.optInt("count") + " 项)");
                if (arr != null) {
                    for (int i = 0; i < arr.length(); i++) {
                        JSONObject e = arr.getJSONObject(i);
                        fileEntries.add(e);
                        boolean isDir = e.optBoolean("dir");
                        fileLines.add((isDir ? "📁 " : "📄 ") + e.optString("name")
                                + (isDir ? "" : "   " + humanSize(e.optLong("size"))));
                    }
                }
                fileAdapter.notifyDataSetChanged();
                setStatus("已列出 " + fileEntries.size() + " 项", false);
            } catch (Exception e) {
                setStatus("解析失败: " + e.getMessage(), true);
            }
        });
    }

    private static String humanSize(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format("%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format("%.1f MB", bytes / 1048576.0);
        return String.format("%.2f GB", bytes / 1073741824.0);
    }

    private void goUp() {
        int s = currentDir.lastIndexOf('/');
        currentDir = s < 0 ? "" : currentDir.substring(0, s);
        refreshFiles();
    }

    private void promptMkdir() {
        final EditText e = field("目录名", 0);
        new AlertDialog.Builder(this)
                .setTitle("在 /" + currentDir + " 下新建目录")
                .setView(e)
                .setPositiveButton("创建", (d, w) -> {
                    String name = e.getText().toString().trim();
                    if (name.isEmpty()) return;
                    String full = currentDir.isEmpty() ? name : currentDir + "/" + name;
                    runAsync("新建目录", () -> client.mkdir(full, true), o -> {
                        setStatus("已创建 " + full, false);
                        refreshFiles();
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void promptDelete() {
        final EditText e = field("要删除的路径（相对下载目录）", 0);
        e.setText(currentDir);
        new AlertDialog.Builder(this)
                .setTitle("删除")
                .setMessage("路径会被服务端约束在下载目录内，越界会被拒绝。")
                .setView(e)
                .setPositiveButton("删除", (d, w) -> {
                    String p = e.getText().toString().trim();
                    if (p.isEmpty()) return;
                    runAsync("删除", () -> client.delete(p, true), o -> {
                        setStatus("已删除 " + p, false);
                        refreshFiles();
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showFileMenu(JSONObject e) {
        final String path = e.optString("path");
        new AlertDialog.Builder(this)
                .setTitle(e.optString("name"))
                .setItems(new String[]{"删除", "重命名"}, (d, which) -> {
                    if (which == 0) {
                        runAsync("删除", () -> client.delete(path, true), o -> {
                            setStatus("已删除 " + path, false);
                            refreshFiles();
                        });
                    } else {
                        final EditText ni = field("新名字", 0);
                        new AlertDialog.Builder(this)
                                .setTitle("重命名")
                                .setView(ni)
                                .setPositiveButton("确定", (dd, ww) -> {
                                    String name = ni.getText().toString().trim();
                                    if (name.isEmpty()) return;
                                    int slash = path.lastIndexOf('/');
                                    String to = slash < 0 ? name
                                            : path.substring(0, slash + 1) + name;
                                    runAsync("重命名",
                                            () -> client.fileOp("rename", path, to, 0),
                                            o -> {
                                                setStatus("已重命名为 " + name, false);
                                                refreshFiles();
                                            });
                                })
                                .setNegativeButton("取消", null)
                                .show();
                    }
                })
                .show();
    }

    // ── 页 4：控制 ──────────────────────────────────────────────────────────

    private ImageView shotView;

    private View buildControlTab() {
        ScrollView sv = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        sv.addView(col);

        col.addView(label("点击"));
        LinearLayout r1 = row();
        final EditText tx = field("x", 1);
        final EditText ty = field("y", 1);
        r1.addView(tx);
        r1.addView(ty);
        r1.addView(button("点击", v -> runAsync("点击",
                () -> { client.tap(intOf(tx, 0), intOf(ty, 0), 50); return "ok"; },
                o -> setStatus("已点击", false))));
        col.addView(r1);

        col.addView(label("滑动"));
        LinearLayout r2 = row();
        final EditText sx1 = field("x1", 1), sy1 = field("y1", 1);
        final EditText sx2 = field("x2", 1), sy2 = field("y2", 1);
        r2.addView(sx1); r2.addView(sy1); r2.addView(sx2); r2.addView(sy2);
        col.addView(r2);
        LinearLayout r3 = row();
        r3.addView(button("滑动", v -> runAsync("滑动", () -> {
            client.swipe(intOf(sx1, 0), intOf(sy1, 0),
                         intOf(sx2, 0), intOf(sy2, 0), 300);
            return "ok";
        }, o -> setStatus("已滑动", false))));
        r3.addView(button("截图并显示", v -> doScreenshot()));
        col.addView(r3);

        shotView = new ImageView(this);
        shotView.setAdjustViewBounds(true);
        col.addView(shotView);
        return sv;
    }

    private static int intOf(EditText e, int def) {
        try {
            String s = e.getText().toString().trim();
            return s.isEmpty() ? def : Integer.parseInt(s);
        } catch (NumberFormatException ex) {
            return def;
        }
    }

    private void doScreenshot() {
        setStatus("截图中…", false);
        runAsync("截图", () -> {
            int[] w = new int[1], h = new int[1], f = new int[1];
            byte[] px = client.capture(w, h, f, 0);
            return new Object[]{px, w[0], h[0], f[0]};
        }, o -> {
            Object[] a = (Object[]) o;
            byte[] px = (byte[]) a[0];
            int w = (Integer) a[1], h = (Integer) a[2], fmt = (Integer) a[3];
            Bitmap bmp = toBitmap(px, w, h, fmt);
            if (bmp == null) {
                setStatus("像素格式 0x" + Integer.toHexString(fmt) + " 暂不支持显示", true);
                return;
            }
            shotView.setImageBitmap(bmp);
            setStatus("截图 " + w + "x" + h + "（" + px.length + " 字节）", false);
        });
    }

    /** 把服务端返回的原始像素转成 Bitmap。只处理最常见的两种格式。 */
    private static Bitmap toBitmap(byte[] px, int w, int h, int fmt) {
        if (w <= 0 || h <= 0 || px == null || px.length < w * h * 4) return null;
        int[] argb = new int[w * h];
        final int PIXEL_RGBA_8888 = 1, PIXEL_RGBX_8888 = 2, PIXEL_BGRA_8888 = 5;
        for (int i = 0; i < w * h; i++) {
            int r = px[i * 4] & 0xFF;
            int g = px[i * 4 + 1] & 0xFF;
            int b = px[i * 4 + 2] & 0xFF;
            int a = px[i * 4 + 3] & 0xFF;
            if (fmt == PIXEL_BGRA_8888) {
                int t = r; r = b; b = t;
            }
            if (fmt == PIXEL_RGBX_8888) a = 0xFF;
            if (fmt != PIXEL_RGBA_8888 && fmt != PIXEL_RGBX_8888
                    && fmt != PIXEL_BGRA_8888) {
                return null;
            }
            argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return Bitmap.createBitmap(argb, w, h, Bitmap.Config.ARGB_8888);
    }
}
