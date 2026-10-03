# remote-control 设备树 · 产品发现入口
#
# 由 dev/04-android-rom/scripts/apply-overlay.sh 从项目目录同步到 aosp/device/remote_control/。
# Soong 的 finder 会递归扫描 device/、vendor/、product/ 下的 AndroidProducts.mk，
# 因此不需要改动 AOSP 上游任何文件即可让 `lunch ...` 生效。
#
# 两个产品并列：
#
#   remote_control_x64_arm64  x86_64 guest + 用户态翻译层（libndk_translation）
#       → 跑在 x86_64 宿主：Linux(KVM) / Windows(WHPX) / Intel Mac(HVF，未实测)
#       → 能跑 arm64 应用（应用侧被翻译，系统侧原生 x86_64）
#
#   remote_control_arm64      原生 arm64，无翻译层
#       → 跑在 arm64 宿主：Apple Silicon(HVF，待实测) / arm64 Linux(KVM，未实测)
#       → 应用必须自带 arm64-v8a 库；**不能**跑纯 32/64 位 x86 应用
#
# 两条线的分工与为什么都要保留，见 docs/13-macos-port.md §4。

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/remote_control_x64_arm64/product/remote_control_x64_arm64.mk \
    $(LOCAL_DIR)/remote_control_arm64/product/remote_control_arm64.mk
