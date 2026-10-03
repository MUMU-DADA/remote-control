#
# remote-control arm64 · 设备配置
#
# 直接继承 Google 的 arm64 模拟器设备目录，再补两件 sdk_phone64_* 里有、
# 而它没有的东西（与 x86_64 那个产品的 device.mk 同样处理）。
#

$(call inherit-product, device/generic/goldfish/emulator64_arm64/device.mk)

# 模拟器需要 BIOS 文件。
# 上游的 emulator64_x86_64/device.mk 里有这两行，arm64 版没有（官方镜像里
# 也没有 bios.bin，能跑；这里补上是为了让 sysdir 更接近完整模拟器布局）。
PRODUCT_HOST_PACKAGES += \
    bios.bin \
    vgabios-cirrus.bin

# ---------------------------------------------------------------------------
# 以太网特性
#
# SystemServer 只有在声明了 android.hardware.ethernet（或 USB_HOST）时才会启动
# EthernetService（frameworks/base/services/java/com/android/server/SystemServer.java:1897）。
# 没有它时 `dumpsys ethernet` 直接报 "Can't find service: ethernet"，
# 即便 eth0 有地址，Android 也不把它当成网络。
#
# ⚠️ 两个产品的处境不同，别照抄结论：
#   · x86_64 产品加它是为了配 `-net-tap`（Linux 桥接）——那边的目标是"落到物理局域网"。
#   · arm64 产品这边 **macOS / Windows 都没有 tap 等价物**（见 docs/13-macos-port.md §3.5），
#     所以这一项在当前交付里**用不上**。
#   保留是为了：(a) 与 x86_64 产品的 sysdir 属性面尽量一致；
#               (b) 将来若在 arm64 Linux 宿主上做桥接，不用再改产品。
#   它只多一个权限 xml（约 800 字节），没有运行时开销。
#
# 写法照抄 goldfish 自己的 device/generic/goldfish/fvp.mk:87。
# ---------------------------------------------------------------------------
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml
