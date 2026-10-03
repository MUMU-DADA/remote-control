#
# remote-control arm64 · BoardConfig
#
# 来源：AOSP 12 `device/generic/goldfish/emulator64_arm64/BoardConfig.mk`
#       （上游的 arm64 模拟器板；`sdk_phone64_arm64` 这个产品就是它编出来的）
#
# 与 x86_64 那个产品（remote_control_x64_arm64）的差别是**减少**，不是增加：
#   · 没有 TARGET_NATIVE_BRIDGE_* 四个变量（原生 arm64 不需要翻译层）
#   · 没有 bridge/ 载荷目录、没有 bridge-copy.mk
#   · 没有 BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES
#   论证见 docs/13-macos-port.md §2.2。
#

TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_VARIANT := generic
TARGET_CPU_ABI := arm64-v8a

TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_VARIANT := generic

#
# ⚠️ 分区形态要在 include 之前定下来。
#
# BoardConfigEmuCommon.mk 里用 `ifeq ($(QEMU_USE_SYSTEM_EXT_PARTITIONS),true)` 决定
# 动态分区列表：true → system/system_ext/product/vendor 四个独立分区；
# 否则 → 只有 system/vendor，且 TARGET_COPY_OUT_PRODUCT/SYSTEM_EXT 会被改成
# system/product、system/system_ext（GSI 布局）。
# make 的条件判断在 include 那一刻求值，所以这个开关**必须写在 include 之前**。
#
# ⚠️⚠️ **只能设 QEMU_USE_SYSTEM_EXT_PARTITIONS，不能设 PRODUCT_USE_DYNAMIC_PARTITIONS。**
#
#    后者走到 board_config.mk 时已经是 **readonly**（在 config.mk 更早处被固化），
#    在这里赋值会直接让 lunch 失败：
#        BoardConfig.mk:32: error: cannot assign to readonly variable: PRODUCT_USE_DYNAMIC_PARTITIONS
#        dumpvars failed with: exit status 1
#    （实测踩到，见 docs/13-macos-port.md §7.0.5。）
#
#    它的正确位置是**产品 mk**：product/remote_control_arm64.mk 顶部。
#    注意 x86_64 那个产品**两边都写了**——BoardConfig 里那行之所以没炸，
#    是因为产品侧已经先把它设成同一个值，早于 BoardConfig 被求值。
#    本产品只在产品 mk 里设一次，不复制那个巧合。
#
QEMU_USE_SYSTEM_EXT_PARTITIONS := true

include build/make/target/board/BoardConfigGsiCommon.mk
include build/make/target/board/BoardConfigEmuCommon.mk

BOARD_USERDATAIMAGE_PARTITION_SIZE := 576716800
BOARD_BOOTIMAGE_PARTITION_SIZE := 0x02000000

TARGET_PRELINK_MODULE := false

# Wifi（与上游 emulator64_arm64 逐行一致）
BOARD_WLAN_DEVICE           := emulator
BOARD_HOSTAPD_DRIVER        := NL80211
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB   := lib_driver_cmd_simulated
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_simulated
WPA_SUPPLICANT_VERSION      := VER_0_8_X
WIFI_DRIVER_FW_PATH_PARAM   := "/dev/null"
WIFI_DRIVER_FW_PATH_STA     := "/dev/null"
WIFI_DRIVER_FW_PATH_AP      := "/dev/null"

# 与 x86_64 产品保持一致。当前 AOSP 12 树里这两个产品都不需要，
# 保留是为了**万一** arm64 侧的规则与翻译层产品不同源时，报错能指向这里而不是刷屏。
# 若构建报 "duplicate rules"，把下面这行的注释去掉即可。
# BUILD_BROKEN_DUP_RULES := true

# ===========================================================================
# remote-control 增量
# ===========================================================================
#
# 1) SELinux 策略：**两个产品共用同一份策略文件**，所以这里的路径仍然指向
#    remote_control_x64_arm64/sepolicy —— 这是历史命名，与 guest 架构无关。
#
#    ⚠️ 不要为了"路径好看"复制一份策略到 arm64 目录下。那会制造第二处真源，
#       以后两边必然漂移（策略里 33 条命令的规则有一千多行）。
#       改成中性目录名（如 device/remote_control/common/sepolicy）是可以的，
#       但那要同时改 apply-overlay.sh 与 x86_64 产品的 BoardConfig —— 另作一次改动。
#
# 2) 策略分区选 **vendor**（BOARD_SEPOLICY_DIRS），与 x86_64 产品一致。
#    "vendor 策略 vs system_ext 策略"那段双向取舍记在 x86_64 的 BoardConfig 里，
#    结论对两个产品都成立（取决于抓帧链路要用的 HAL 类型，与架构无关）。
#
BOARD_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy
