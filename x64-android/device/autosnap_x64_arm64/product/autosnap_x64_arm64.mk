#
# AutoSnap x86_64 + ARM64 桥 · 产品定义
#
#   lunch autosnap_x64_arm64-userdebug
#   产物目录 out/target/product/autosnap_x64_arm64/
#
# 目标：**同架构**（x86_64 guest 跑在 x86_64 宿主上，KVM/WHPX 硬件加速）
#       的自编 Android 12 ROM，同时能跑 arm64 应用（走 libndk_translation 翻译层）。
#
# 结构对照 AOSP 上游的 sdk_phone64_x86_64（Android 12 / API 31 的 x86_64 模拟器产品），
# 差异只有三处：device 目录换成 arm64 桥版本、补 BIOS 主机包、加翻译层载荷与属性。
#

QEMU_USE_SYSTEM_EXT_PARTITIONS := true
PRODUCT_USE_DYNAMIC_PARTITIONS := true

#
# All components inherited here go to system image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_system.mk)

# 翻译层载荷通过 PRODUCT_COPY_FILES 安装到 /system 下若干固定路径，
# 会与 artifact path 规则冲突，这里放宽（官方 sdk_phone64_x86_64 同样设为 relaxed）。
PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed

#
# All components inherited here go to system_ext image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/handheld_system_ext.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/telephony_system_ext.mk)

#
# All components inherited here go to product image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_product.mk)

#
# All components inherited here go to vendor image
#
# 注意：x86_64-vendor.mk 里 include 的是 **x86_64 内核**（x86_64-kernel.mk），
# 这正是"同架构"的关键——ARM 应用是在用户态被翻译的，内核与框架都是 x86_64。
$(call inherit-product, device/generic/goldfish/64bitonly/product/x86_64-vendor.mk)
$(call inherit-product, device/generic/goldfish/64bitonly/product/emulator64_vendor.mk)

#
# 设备
#
$(call inherit-product, device/autosnap/autosnap_x64_arm64/device.mk)

# Define the host tools and libs that are parts of the SDK.
$(call inherit-product-if-exists, sdk/build/product_sdk.mk)
$(call inherit-product-if-exists, development/build/product_sdk.mk)

# ===========================================================================
# ARM 翻译层（libndk_translation）
# ===========================================================================
#
# 1) 载荷文件：由 x64-android/scripts/apply-overlay.sh 从 Google 官方
#    `system-images;android-31;google_apis;x86_64` 镜像里提取，落到
#    device/autosnap/autosnap_x64_arm64/bridge/ 下，并生成 bridge-copy.mk
#    （显式列出 80+ 条 PRODUCT_COPY_FILES，避免用 shell 在 make 里遍历目录）。
#
include device/autosnap/autosnap_x64_arm64/bridge/bridge-copy.mk

# 2) 属性。分两处放，是为了**照抄官方镜像的落位**（见评估文档 §2.6）：
#      /system/build.prop  ← isa 映射 + 放行 exec
#      /vendor/build.prop  ← 翻译层库名（vendor 的 build.prop 后加载，会覆盖 system 里的同名项）
#
#    ABI 列表不在这里写：TARGET_NATIVE_BRIDGE_ABI（BoardConfig）会让
#    ro.{system,vendor,odm}.product.cpu.abilist{64} 自动带上 arm64-v8a。
#
#    注意 `ro.dalvik.vm.native.bridge=0` 由 build/make/target/product/runtime_libart.mk
#    写进 system 分区且是强赋值，无法在同分区覆盖（重复 sysprop 会直接报错），
#    所以这里走 vendor 分区——这也正是官方镜像的做法。
PRODUCT_SYSTEM_PROPERTIES += \
    ro.dalvik.vm.isa.arm=x86 \
    ro.dalvik.vm.isa.arm64=x86_64 \
    ro.enable.native.bridge.exec=1

PRODUCT_VENDOR_PROPERTIES += \
    ro.dalvik.vm.native.bridge=libndk_translation.so

# Overrides
PRODUCT_BRAND := AutoSnap
PRODUCT_NAME := autosnap_x64_arm64
PRODUCT_DEVICE := autosnap_x64_arm64
PRODUCT_MODEL := AutoSnap x86_64 with ARM64 bridge
