# payload/ · ARM 翻译层（libndk_translation）

本目录由 `scripts/fetch-payload.sh` 生成，**`system/` 不入库**（Google 专有二进制，23 MB）。

```
payload/
├── system/                     ← 90 个文件，提取自官方镜像（.gitignore）
│   ├── lib64/libndk_translation*.so        (21：翻译器 + 20 个 proxy)
│   ├── lib64/arm64/*.so                    (59：aarch64 系统库)
│   ├── bin/arm64/{linker64,app_process64}
│   ├── bin/ndk_translation_program_runner_binfmt_misc_arm64
│   ├── etc/binfmt_misc/{arm,arm64}_{exe,dyn}
│   ├── etc/init/ndk_translation.rc
│   └── etc/ld.config.arm.txt  etc/ld.config.arm64.txt
└── MANIFEST.sha256             ← 入库：锁定"这份 ROM 用的是哪一版载荷"
```

来源：`system-images;android-31;google_apis;x86_64`（`x86_64-31_r14.zip`，sha1
`9aedd3e85cad7a479146f6858f4a94840c2a3f29`）。**API 级别必须与 ROM 一致**——
翻译层里的 ARM 侧 bionic 是跟框架版本配套的，所以 API 31 的载荷只能配 Android 12 的 ROM。

重新生成：

```bash
cd .. && ./scripts/fetch-payload.sh          # 重新下载 + 提取 + 重算 MANIFEST
```

⚠️ **许可**：`libndk_translation` 是 Google 专有二进制，只随 SDK 系统镜像分发，
SDK 许可不包含再分发。自用/内部开发无碍；**对外交付整机前必须过法务**。
