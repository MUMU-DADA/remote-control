# 上位应用 APK

`remote-control-controller.apk` 是从仓库当前 `dev/05-controller-app/` 源码构建的上位应用，包名为
`com.remotecontrol.controller`。release 打包时会重新构建该 APK，并把它与 ROM 分开存放；它不会被预装进系统镜像。

安装到运行中的实例：

```bash
./runtime/platform-tools/adb -s emulator-5580 install -r ./tools/remote-control-controller.apk
```

Windows 使用 `runtime\platform-tools\adb.exe`，实例序列号同样是 `emulator-<端口>`。

当前应用源码仍通过 `/sdcard/remote-control.conf` 与 `/sdcard/remote-control.status` 管理服务；release ROM 使用 init
服务，读取 `/data/misc/remote-control/remote-control.conf`。两者尚未接通，因此应用中的服务管理操作目前不能控制 release
里的服务。APK 随包提供用于交付源码对应构建产物，不代表这些操作已完成集成。

该 APK 使用本地 debug keystore 签名，仅适用于开发和测试；正式分发应使用受控的发布签名密钥。
