# remote-control x86_64 + ARM64 桥 · 产品发现入口
#
# 由 x64-android/scripts/apply-overlay.sh 从项目目录同步到 aosp/device/remote_control/。
# Soong 的 finder 会递归扫描 device/、vendor/、product/ 下的 AndroidProducts.mk，
# 因此不需要改动 AOSP 上游任何文件即可让 `lunch remote_control_x64_arm64-userdebug` 生效。

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/remote_control_x64_arm64/product/remote_control_x64_arm64.mk
