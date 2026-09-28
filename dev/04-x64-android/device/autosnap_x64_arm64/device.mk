#
# AutoSnap x86_64 + ARM64 桥 · 设备配置
#
# 直接继承 Google 的 arm64 桥设备目录，再补两件 sdk_phone64_x86_64 里有、
# 而它没有的东西。
#

$(call inherit-product, device/generic/goldfish/emulator64_x86_64_arm64/device.mk)

# 模拟器需要 BIOS 文件。
# 上游的 emulator64_x86_64/device.mk 里有这三行，arm64 桥版没有（官方镜像里
# 也没有 bios.bin，能跑；这里补上是为了让 sysdir 更接近完整模拟器布局）。
PRODUCT_HOST_PACKAGES += \
    bios.bin \
    vgabios-cirrus.bin

# ---------------------------------------------------------------------------
# 以太网特性 —— 桥接模式的**必要前提**
#
# SystemServer 只有在声明了 android.hardware.ethernet（或 USB_HOST）时才会启动
# EthernetService（frameworks/base/services/java/com/android/server/SystemServer.java:1897）。
# 没有它时 `dumpsys ethernet` 直接报 "Can't find service: ethernet"：
# 即便 eth0 已经从桥拿到了真实局域网 IP，Android 也不会把它当成网络，
# 应用流量仍然只走模拟器内置 Wi-Fi（NAT），桥接等于白做。
#
# 声明之后，EthernetTracker 会因为 config_ethernet_interfaces 为空而回落到
# config_ethernet_iface_regex（默认 eth\d —— EthernetTracker.java:133/636/382），
# 自动接管 eth0，因此**不需要**再写资源 overlay。
#
# 配合 emulator 的 -net-tap tap0（见 scripts/run-linux.sh + tools/net-bridge.sh）
# 就得到真正意义上的桥接模式。
# 写法照抄 goldfish 自己的 device/generic/goldfish/fvp.mk:87。
# ---------------------------------------------------------------------------
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml
