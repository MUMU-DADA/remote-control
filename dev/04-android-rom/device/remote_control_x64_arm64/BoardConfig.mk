#
# remote-control x86_64 + ARM64 桥 · BoardConfig
#
# 来源：AOSP 12 `device/generic/goldfish/emulator64_x86_64_arm64/BoardConfig.mk`
#       （Google 官方 `google_apis;android-31;x86_64` 镜像就是这块板编出来的：
#        设备名 emulator64_x86_64_arm64，abilist = x86_64,arm64-v8a）
#
# 与上游的差异只有文件末尾「remote-control 增量」一节，其余逐行照抄，便于和官方对照。
#

TARGET_CPU_ABI := x86_64
TARGET_ARCH := x86_64
TARGET_ARCH_VARIANT := x86_64
TARGET_2ND_ARCH_VARIANT := x86_64

# ---- ARM 翻译层：让 x86_64 系统能跑 arm64 应用 ----
# 这两个变量会被 build/make/core/board_config.mk 追加进 TARGET_CPU_ABI_LIST{,_64_BIT}，
# 再由 sysprop.mk 写进 system/vendor/odm 三个分区的 build.prop：
#     ro.<分区>.product.cpu.abilist64 = x86_64,arm64-v8a
# init 启动时会按 product → odm → vendor → system 的优先级派生出平凡的
# ro.product.cpu.abilist —— 所以这三处必须是同一份值，不能只改一处。
TARGET_NATIVE_BRIDGE_ARCH := arm64
TARGET_NATIVE_BRIDGE_ARCH_VARIANT := armv8-a
TARGET_NATIVE_BRIDGE_CPU_VARIANT := generic
TARGET_NATIVE_BRIDGE_ABI := arm64-v8a

BUILD_BROKEN_DUP_RULES := true

TARGET_PRELINK_MODULE := false

include build/make/target/board/BoardConfigMainlineCommon.mk
include build/make/target/board/BoardConfigEmuCommon.mk

# the settings differ from BoardConfigMainlineCommon.mk
BOARD_USES_SYSTEM_OTHER_ODEX :=

BOARD_USERDATAIMAGE_PARTITION_SIZE := 576716800

BOARD_SEPOLICY_DIRS += device/generic/goldfish/sepolicy/x86
# remote-control 的 SELinux 域。
#
# ⚠️ **必须放设备树，不能放 system/sepolicy/private/**。
#    往平台策略树里加文件会让 AOSP 的 sepolicy_freeze_test 挂掉
#    （它 diff 当前树与 prebuilts/api/31.0，多了文件就报
#     "Only in system/sepolicy/private: ..."，整个 ninja 停在那里）。
#    设备/产品自己的策略本来就该走 BOARD_SEPOLICY_DIRS ——
#    goldfish 的 x86 策略就是这么接的（上一行）。
#
# ⚠️⚠️ 用 **SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS**，不是 BOARD_SEPOLICY_DIRS。
#
#    两者都能避开 sepolicy_freeze_test（那只管 system/sepolicy/{public,private}），
#    但**分区不同**：
#      · BOARD_SEPOLICY_DIRS        → 编进 **vendor** 策略
#      · SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS → 编进 **system_ext** 策略（属平台侧）
#
#    这个区别是实测撞出来的：服务要跑 `pm`/`am`/`svc`（都是 shell 脚本 →
#    app_process 起 Java VM），而 app_process 要读的属性里有**平台私有类型**
#    （如 odsign_prop，定义在 system/sepolicy/private/property.te）。
#    vendor 策略**看不见**平台私有类型，写就报 `unknown type odsign_prop` ——
#    这是分区可见性规则，不是缺什么。
#
#    换成 system_ext 之后，设备树的策略回到平台侧，平台私有类型就能用了。
#    （顺带：get_prop() 这类宏也才在正确的可见性上下文里。）
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy

# ⚠️⚠️ **这一整段是历史记录，不要照着它做决定 —— 生效的只有上面第 64 行。**
#
#   策略分区两头都实测撞过，先后顺序是「先 vendor，撞墙后改 system_ext」：
#
#   BOARD_SEPOLICY_DIRS              → vendor 策略   ← **已弃用**（下面那行是注释）
#        ✅ 看得见 vendor 侧类型（hal_graphics_allocator_default 等 HAL）
#        ❌ 看不见平台私有类型（odsign_prop → unknown type）
#
#   SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS → system_ext 策略  ← **当前生效**（第 64 行）
#        ✅ 看得见平台私有类型
#        ⚠️ 曾担心"看不见 vendor 侧类型" —— 但 x86_64 产品用这个宏**全量构建全绿**，
#           说明该担心在这个 AOSP 12 树上**不成立**（至少对策略里用到的那些类型不成立）。
#
#   （两者都能避开 sepolicy_freeze_test —— 那只管 system/sepolicy/{public,private}。）
#
# ⚠️ 2026-10-03 补记：这段注释原来结尾写着"当前选 **vendor**"，与文件里**实际生效的行相反**，
#    直接导致 arm64 产品照着它抄、构建失败在 `unknown type odsign_prop`
#    （device/remote_control_arm64/BoardConfig.mk 的第一版）。
#    教训：**注释会被当成事实抄走**。改代码时如果旧注释不再成立，要么删掉、要么显式标注"已弃用"。
#    详见 docs/13-macos-port.md §7.0.10。
#
# 旧写法（**不要启用**）：
# BOARD_SEPOLICY_DIRS += device/remote_control/remote_control_x64_arm64/sepolicy

# Wifi.
BOARD_WLAN_DEVICE           := emulator
BOARD_HOSTAPD_DRIVER        := NL80211
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB   := lib_driver_cmd_simulated
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_simulated
WPA_SUPPLICANT_VERSION      := VER_0_8_X
WIFI_DRIVER_FW_PATH_PARAM   := "/dev/null"
WIFI_DRIVER_FW_PATH_STA     := "/dev/null"
WIFI_DRIVER_FW_PATH_AP      := "/dev/null"

# ===========================================================================
# remote-control 增量（唯一与上游不同的地方）
# ===========================================================================
#
# 翻译层载荷是「预编译 ELF + PRODUCT_COPY_FILES」。AOSP 默认会拦下这种写法并提示
# 改用 cc_prebuilt_binary / cc_prebuilt_library_shared；这里显式放行。
#
# 为什么不用 prebuilt 模块：载荷是 80+ 个文件、目的地跨 lib64 / lib64/arm64 /
# bin / bin/arm64 / etc 多处，做成模块既冗长又容易在安装路径上出错。
# Google 自己的 Cuttlefish（device/google/cuttlefish/vsoc_x86/BoardConfig.mk）
# 处理同一份 ndk_translation 载荷时，用的也是这个开关。
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true
