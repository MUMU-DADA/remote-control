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
