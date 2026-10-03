#
# remote-control arm64 · 产品定义
#
#   lunch remote_control_arm64-userdebug
#   产物目录 out/target/product/remote_control_arm64/
#
# 目标：**原生 arm64** 的 Android 12 ROM（无翻译层），跑 remote-control 服务。
#       宿主是 arm64（Apple Silicon / arm64 Linux），走同一架构的硬件加速。
#
# 结构对照 AOSP 上游的 `sdk_phone64_arm64`（Android 12 / API 31 的 arm64 模拟器产品），
# 以及本项目的 x86_64 产品 `remote_control_x64_arm64`。差异只有三处：
#   · 设备目录换成 arm64 版（device/remote_control/remote_control_arm64）
#   · vendor 走 arm64-vendor.mk（内含 arm64 内核）
#   · **去掉整套翻译层**（载荷、属性、artifact path 放行）
#

# ⚠️ 这两个开关的**位置是有讲究的**：
#
#   · `PRODUCT_USE_DYNAMIC_PARTITIONS` —— **必须在这里**。放到 BoardConfig.mk 里会报
#       "cannot assign to readonly variable: PRODUCT_USE_DYNAMIC_PARTITIONS"
#     （它在 config.mk 更早处已被固化）。实测踩到，见 docs/13-macos-port.md §7.0.5。
#
#   · `QEMU_USE_SYSTEM_EXT_PARTITIONS` —— BoardConfig.mk 里那份（第 40 行附近）才是生效的，
#     因为 BoardConfigEmuCommon.mk 要用 `ifeq` 读它来决定动态分区列表。
#     这里再写一次只是让"两个产品顶部长得一样"，值相同，无副作用。
#
PRODUCT_USE_DYNAMIC_PARTITIONS := true
QEMU_USE_SYSTEM_EXT_PARTITIONS := true

#
# All components inherited here go to system image
#
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_system.mk)

#
# ⚠️ generic_system.mk 规定了 system 镜像里哪些路径算"合规"（artifact path requirement）。
#    本产品的 remote-control 落点是 system/bin、system/etc/remote-control ——
#    **这些本来就在合规清单里**，所以不需要 x86_64 产品那种 `relaxed` 放行。
#
#    下面这几条是从 x86_64 产品抄来的白名单，保留是因为它们**实测能过**：
#    改产品清单时少一条就会撞 "produces files inside ... artifact path requirement"
#    这类报错。想收紧的话，删掉对应项后重新 lunch 一次就知道哪条是多余的。
#
PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := relaxed
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/bin/remote-control \
    system/bin/remote-control-launch \
    system/bin/rcctl \
    system/etc/init/remote-control.rc \
    system/etc/remote-control/cliptool.jar

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
# 注意：arm64-vendor.mk 里 include 的是 **arm64 内核**（arm64-kernel.mk，
# EMULATOR_KERNEL_FILE = kernel/prebuilts/5.10/arm64/kernel-5.10-gz）。
#
# ⚠️ 这里与 x86_64 产品的**实质差别**：那边因为要跑 ARM 应用，翻译层是用户态的，
#    系统侧全是 x86_64 原生；这边**从内核到框架到应用全是 arm64**。
$(call inherit-product, device/generic/goldfish/64bitonly/product/arm64-vendor.mk)
$(call inherit-product, device/generic/goldfish/64bitonly/product/emulator64_vendor.mk)

#
# 设备
#
$(call inherit-product, device/remote_control/remote_control_arm64/device.mk)

# Define the host tools and libs that are parts of the SDK.
$(call inherit-product-if-exists, sdk/build/product_sdk.mk)
$(call inherit-product-if-exists, development/build/product_sdk.mk)

# ===========================================================================
# 翻译层：**本产品没有**
# ===========================================================================
#
# x86_64 产品在这一段做三件事，这里一件都不做：
#   1) include bridge/bridge-copy.mk（80+ 条 PRODUCT_COPY_FILES）
#   2) 写 ro.dalvik.vm.isa.arm / isa.arm64 / enable.native.bridge.exec
#   3) 写 ro.dalvik.vm.native.bridge=libndk_translation.so
#
# ⚠️ 第 3 条尤其不能抄：在原生 arm64 上设了它，ART 会去找一个**不存在的**
#    libndk_translation.so。这里显式记一笔，避免以后"对齐两个产品"时被顺手抄过来。
#
# 也因此，本产品**没有** TARGET_NATIVE_BRIDGE_*（那在 BoardConfig 里），
# build.prop 里的 ro.*.product.cpu.abilist64 就是纯 arm64-v8a。

# ===========================================================================
# remote-control（截图 / 触控 / 设备管理服务）
# ===========================================================================
#
# ⚠️ 不写这一段的话模块**根本不会进 system.img** —— Soong 编得出来，
#    但没有人把它装进镜像，开机后 /system/bin/ 里什么都没有
#    （x86_64 产品踩过这个坑，见 docs/09-deployment-and-update.md §4.3）。
#
# 三个模块的分工（与 x86_64 产品一致）：
#   remote-control-launch  壳。init 拉起的入口，只负责"选版本 + 校验 + 拉起"。
#   remote-control         真正的载荷。运行时从 /data/misc/remote-control/current
#                          指向的版本槽里取，可以热替换而不重编 ROM。
#   rcctl                  控制客户端（命令行）。
#
# 剪贴板辅助工具 cliptool.jar：只含 classes.dex 的 zip，由服务以 shell 身份经
# app_process 运行（为什么要这么绕见 daemon/clipops.h 的文件头）。
# 放在 /system/etc/remote-control/ —— 正是 clipops.cpp 的搜索路径之一。
PRODUCT_COPY_FILES += \
    frameworks/native/cmds/remote-control/tools/cliptool/build/cliptool.jar:$(TARGET_COPY_OUT_SYSTEM)/etc/remote-control/cliptool.jar

PRODUCT_PACKAGES += \
    remote-control-launch \
    remote-control \
    rcctl \
    pm

# ⚠️⚠️ **`pm` 必须显式加进来**（x86_64 产品用极大代价换来的结论，这里照抄）：
#    我们的产品继承 generic_system.mk，而它没带 pm（base_system.mk:228 才带）。
#    少了 pm.jar，应用管理整类会以"退出码 20"这种像权限问题的症状失败，
#    实际一条 denial 都没有。详见 x86_64 产品里的长注释。

# Overrides
PRODUCT_BRAND := remote-control
PRODUCT_NAME := remote_control_arm64
PRODUCT_DEVICE := remote_control_arm64
PRODUCT_MODEL := remote-control arm64 (native, no translation layer)
