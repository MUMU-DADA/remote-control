// ClipTool.java — 剪贴板读取辅助
//
// 为什么需要它，而不是让 daemon 直接调 Binder：
//
//   1. NDK 构建里没有 libbinder —— daemon 发不了 Binder 事务。
//      Java 侧一个调用就够了。
//
//   2. 访问控制在 ClipboardService 里，规则是
//        READ_CLIPBOARD_IN_BACKGROUND 权限（com.android.shell 持有，
//        实测 granted=true）/ 默认输入法 / 有焦点的应用
//      而**权限是按包名查的**，所以调用时必须传对包名。
//
// 为什么不用 ClipboardManager 而直接调 IClipboard：
//
//   ClipboardManager 的包名来自 Context.getOpPackageName()。
//   app_process 起的进程只能拿到 systemContext()，它的包名是 "android"
//   —— 于是 ClipboardService 按 "android" 查权限，拒绝：
//     "Denying clipboard access to android, application is not in focus"
//   实测就是这样失败的。createPackageContext("com.android.shell") 也改不掉
//   getOpPackageName()。
//
//   直接调 IClipboard 就能显式传包名，和 UID 对上：
//     checkPackage(2000, "com.android.shell") ✓
//     权限查 "com.android.shell" → READ_CLIPBOARD_IN_BACKGROUND ✓
//
// ⚠️ 必须以 **shell UID** 运行（daemon 是 root，要 setuid(2000) 后再 exec）。
//
// 用法：
//   app_process /system/bin com.autod.clip.ClipTool get
//   app_process /system/bin com.autod.clip.ClipTool info
//
// 约定：**结果走 stdout，诊断走 stderr**，退出码区分"空"和"失败"：
//   0 = 有内容（已写到 stdout）
//   4 = 剪贴板为空（不是错误）
//   2/3/5 = 失败

package com.autod.clip;

import android.content.ClipData;
import android.content.ClipDescription;
import android.os.IBinder;
import android.os.Looper;

import java.lang.reflect.Method;

public final class ClipTool {

    /** 必须是运行时的 UID 真正拥有的包名，否则 checkPackage 会拒绝 */
    private static final String PKG = "com.android.shell";
    private static final int USER_ID = 0;

    private ClipTool() {}

    public static void main(String[] args) {
        int rc = 1;
        try {
            rc = run(args);
        } catch (Throwable t) {
            System.err.println("异常: " + t);
            rc = 2;
        }
        System.exit(rc);
    }

    /**
     * 通过反射拿 IClipboard。
     *
     * IClipboard 与 ServiceManager 都是 @hide，**所有 SDK 变体的 android.jar
     * 里都没有**（实测 public/system/system-server/test 五个变体全都没有）。
     * 所以编译期只用公开类型，运行期在设备上取真实实现。
     */
    private static Object clipboardService() throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) sm.getMethod("getService", String.class)
                                     .invoke(null, "clipboard");
        if (binder == null) return null;
        Class<?> stub = Class.forName("android.content.IClipboard$Stub");
        return stub.getMethod("asInterface", IBinder.class).invoke(null, binder);
    }

    private static int run(String[] args) throws Exception {
        Looper.prepareMainLooper();

        final String op = args.length > 0 ? args[0] : "get";
        System.err.println("uid=" + android.os.Process.myUid()
                + " pkg=" + PKG + " op=" + op);

        Object iclip = clipboardService();
        if (iclip == null) {
            System.err.println("拿不到 clipboard 服务");
            return 3;
        }
        final Method mHas = iclip.getClass().getMethod("hasPrimaryClip",
                                                       String.class, int.class);
        final Method mGet = iclip.getClass().getMethod("getPrimaryClip",
                                                       String.class, int.class);
        final Method mDesc = iclip.getClass().getMethod("getPrimaryClipDescription",
                                                        String.class, int.class);

        if ("info".equals(op)) {
            Object has = mHas.invoke(iclip, PKG, USER_ID);
            StringBuilder sb = new StringBuilder();
            sb.append("has=").append(has);
            if (Boolean.TRUE.equals(has)) {
                Object d = mDesc.invoke(iclip, PKG, USER_ID);
                if (d instanceof ClipDescription) {
                    ClipDescription cd = (ClipDescription) d;
                    sb.append(" types=");
                    for (int i = 0; i < cd.getMimeTypeCount(); i++) {
                        if (i > 0) sb.append(',');
                        sb.append(cd.getMimeType(i));
                    }
                    sb.append(" label=").append(cd.getLabel());
                }
            }
            System.out.print(sb);
            return 0;
        }

        if ("set".equals(op)) {
            if (args.length < 2) {
                System.err.println("set 需要文本参数");
                return 5;
            }
            // 用反射取 setPrimaryClip(ClipData, String, int)。
            // ClipData 是公开类型，可以直接构造 —— 只有 IClipboard 是隐藏的。
            Method set = iclip.getClass().getMethod("setPrimaryClip",
                    ClipData.class, String.class, int.class);
            set.invoke(iclip, ClipData.newPlainText("autod", args[1]), PKG, USER_ID);
            // ⚠️ ClipboardService 在权限不足时是**静默 return 不抛异常**的，
            //    所以这里不能凭"没抛异常"就报成功 —— 回读一次确认。
            Object has = mHas.invoke(iclip, PKG, USER_ID);
            if (!Boolean.TRUE.equals(has)) {
                System.err.println("写入后回读仍是空 —— 多半是 AppOps 拒绝了");
                return 6;
            }
            System.out.print("ok");
            return 0;
        }

        if (!"get".equals(op)) {
            System.err.println("未知操作: " + op);
            return 5;
        }

        Object clipObj = mGet.invoke(iclip, PKG, USER_ID);
        if (!(clipObj instanceof ClipData)) {
            return 4;                      // 空剪贴板：不是错误
        }
        ClipData clip = (ClipData) clipObj;
        if (clip.getItemCount() == 0) return 4;

        // coerceToText 需要 Context，而这里没有可用的
        // —— 直接取文本项，不行再看 URI/Intent 的文本表示。
        CharSequence text = clip.getItemAt(0).getText();
        if (text == null) {
            if (clip.getItemAt(0).getUri() != null) {
                text = clip.getItemAt(0).getUri().toString();
            } else if (clip.getItemAt(0).getIntent() != null) {
                text = clip.getItemAt(0).getIntent().toUri(0);
            }
        }
        if (text == null) return 4;

        System.out.print(text);
        return 0;
    }
}
