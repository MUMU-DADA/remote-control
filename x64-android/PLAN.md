# 进度与阶段

> 目标：**x86_64 Linux + x86_64 Windows 上同架构运行的自编 Android ROM，可跑 arm64 应用**
> （arm64 若不成立，下放到 arm32）
> 仓库：`/root/AutoSnapshotAndroid/x64-android`

---

## 阶段与状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| **P0 · 立项与设备树** | 项目目录、设备树、载荷提取、注入脚本、产品能在 `lunch` 里被识别 | ✅ 完成（`TARGET_CPU_ABI_LIST=x86_64,arm64-v8a` 已验证） |
| **P1 · 全量编译** | `m` 产出完整 x86_64 镜像（system/vendor/product/ramdisk/kernel-ranchu） | ⏳ 进行中 |
| **P2 · Linux 侧验收** | KVM 启动 + arm64 应用实跑（abilist / 装 / 起 / maps） | ⬜ 待 P1 |
| **P3 · Windows 侧验收** | 同一份镜像 + SDK emulator 37.x + WHPX | ⬜ 待 P2 |
| **P4 · arm32 下放（可选）** | 若需要 32 位 ARM：切 API 30 基座 + 四 ABI 板级配置 | ⬜ 备选 |
| **P5 · ROM 定制** | 把 `autod` 等自制组件编进 `/system`（本项目的下一步价值所在） | ⬜ 待 P2 |

---

## 已完成的实测结论（P0 依据）

来源：`dev/04-emulator/X86_64-ARM64-BRIDGE-EVAL.md`（全部为本机实测）

1. **libhoudini 不可用**：官方源 `_z`（arm64）变体只到 Android 7；8/9 系列只有 32 位 ARM 翻译器。
2. **libndk_translation 可用**：官方 `google_apis;x86_64`(API 31) 镜像 `abilist=x86_64,arm64-v8a`，
   34.4 秒（emulator 31.3.10）/ 29.0 秒（37.2.11）开机，arm64 APK 装 + 跑 + 映射 22 条 arm64 库。
3. **可移植**：把载荷搬进一份普通 AOSP 风格 x86_64 镜像（23 MB 载荷 + 10 行属性），
   同一份镜像从"拒绝 arm64 安装"变成"arm64 应用正常运行"。
4. **属性三处**：`ro.product.cpu.abilist` 由 init 按 product→odm→vendor→system 派生，
   三处分区属性必须一致（只改 system+vendor 不生效）。
5. **API 30 有四 ABI**（含 arm32），API 31 纯 64 位——这是 arm32 下放路线的依据。

---

## P0 交付物清单（本项目内）

- `device/`：完整设备树（BoardConfig / device.mk / product mk / AndroidProducts.mk）
- `payload/`：90 个文件的翻译层 + `MANIFEST.sha256`（可校验）
- `scripts/`：`fetch-payload.sh` / `apply-overlay.sh` / `build-rom.sh` / `run-linux.sh` / `common.sh`
- 注入后 AOSP 侧落点：`aosp/device/autosnap/`（**不改上游任何文件**，`--revert` 可清）

---

## 构建记录

| 时间 | 动作 | 结果 |
|---|---|---|
| 本轮 | `apply-overlay.sh` 注入 90 个载荷文件 + 生成 90 条拷贝规则 | ✅ |
| 本轮 | `lunch autosnap_x64_arm64-userdebug` | ✅ `TARGET_ARCH=x86_64`、`abilist=x86_64,arm64-v8a` |
| 本轮 | `build-rom.sh` 全量构建（`m -j12`，容器 `autod-builder`） | ⏳ 日志 `aosp/out/autosnap-build.log` |
