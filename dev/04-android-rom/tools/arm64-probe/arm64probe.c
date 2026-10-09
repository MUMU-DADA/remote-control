/*
 * arm64 探针的原生侧。只编译 arm64-v8a（见 tools/build-probe-apk.sh）。
 *
 * 注意：uname() 报的是**内核**架构（x86_64）——这正是"用户态翻译"的预期表现：
 * 内核与框架都是 x86_64，只有这段 ARM 代码是被翻译执行的。
 * 所以判定标记用的是编译期宏 + 运行期实测值，而不是 uname。
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

// ⚠️ 符号名必须与 Java 包名**逐字对应**（点换成下划线，不能再多下划线）。
//    这里原来写的是 org_remote_control_…（多一个下划线），而 Java 侧的包是
//    org.remotecontrol.…（没有下划线）—— 于是运行时报
//    `satisfiedLinkError: No implementation found for …abiInfo()`，
//    探针其实**从来没真正调到过原生代码**（2026-10-09 发现）。
JNIEXPORT jstring JNICALL
Java_org_remotecontrol_arm64probe_MainActivity_abiInfo(JNIEnv *env, jclass clazz) {
    char buf[256];
    struct utsname u;
    uname(&u);

    const char *built = "unknown";
#if defined(__aarch64__)
    built = "arm64-v8a";
#elif defined(__arm__)
    built = "armeabi-v7a";
#elif defined(__x86_64__)
    built = "x86_64";
#elif defined(__i386__)
    built = "x86";
#endif

    /* 真算一点东西：整数依赖链，避免被优化成常量 */
    volatile unsigned long acc = 0;
    for (unsigned long i = 0; i < 1000000UL; i++) acc += i * 3 + 1;

    snprintf(buf, sizeof(buf),
             "arm64-v8a native ok | built_for=%s | kernel=%s | ptr=%d bit | acc=%lu",
             built, u.machine, (int)(sizeof(void *) * 8), (unsigned long)acc);
    return (*env)->NewStringUTF(env, buf);
}
