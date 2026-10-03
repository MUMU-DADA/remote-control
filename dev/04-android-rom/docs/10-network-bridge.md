# 10 · 网络：桥接模式（`-net-tap`）

## 结论速览

| 项 | 默认（用户态 NAT / SLIRP） | 桥接模式（本文件） |
|---|---|---|
| 模拟器参数 | 无（内置默认） | `-net-tap tap0 -net-tap-script-up tools/net-bridge-ifup.sh` |
| guest 的 IP | `eth0` 10.0.2.15 / `wlan0` 10.0.2.16 | `eth0` 由**局域网路由器** DHCP 分配（192.168.0.x） |
| 网关 / DNS | 10.0.2.2 / 10.0.2.3（模拟器内部） | 真实网关 / 真实 DNS |
| 默认网络 | Wi-Fi（`AndroidWifi`，NAT） | 以太网（`eth0`，真实局域网） |
| 局域网设备能否直达 | 否（在 NAT 后面，只能靠 `adb forward`） | 能（与其它主机同处一个二层域） |
| 宿主侧改动 | 无 | 建 `br0` 并把上行网卡桥进去 |
| ROM 侧改动 | 无 | 需要声明 `android.hardware.ethernet`（见 §3） |

---

## 1. 模拟器原生支持 TAP 桥接，不用 hack

```
$ emulator -help | grep -E "net-tap|wifi-tap"
  -net-tap <interface>                use this TAP interface for networking
  -net-tap-script-up <script>         script to run when the TAP interface goes up
  -net-tap-script-down <script>       script to run when the TAP interface goes down
  -wifi-tap <interface>               use this TAP interface for Virtio Wi-Fi
  -qemu args...                       pass arguments to qemu
```

`-net-tap <ifname>` 把 guest 的 `eth0` 从内置 SLIRP 换成宿主上的一张 TAP 网卡。
模拟器（QEMU）自己创建这张 TAP，拉起时回调 `-net-tap-script-up` 指定的脚本，
并把接口名作为 `$1` 传进去 —— `tools/net-bridge-ifup.sh` 就是干这个的：把它挂进桥。

> **桥接后 Wi-Fi 不再关联**：只要把 TAP 接进来（`-net-tap`），guest 的 Wi-Fi 就停在
> `DISCONNECTED` —— `wlan0` 掉到 `NO-CARRIER`、拿不到 IP，原来那个 `AndroidWifi`
> （NAT）网络消失，`Active default network` 从 Wi-Fi 变成以太网。
> 对桥接来说这不是问题（本来就该走以太网），但要知道**桥接模式下没有 Wi-Fi 可用**。
>
> 另外 `-wifi-tap` 这条岔路走不通：它与 `advancedFeatures.ini` 里的 `VirtioWifi` +
> `Mac80211hwsimUserspaceManaged`（Wi-Fi 由 `netsimd` 用户态模拟）冲突，实测那条路上
> Wi-Fi 同样起不来、`Active default network` 直接是 `none`。所以**只桥 eth0**。

---

## 2. 宿主侧：`tools/net-bridge.sh`

```bash
sudo tools/net-bridge.sh up        # 建 br0，把 ens33 桥进去，IP/路由搬到 br0
tools/net-bridge.sh confirm        # 确认没问题，撤掉自动回滚
tools/net-bridge.sh status         # 看状态
sudo tools/net-bridge.sh down      # 还原
```

`up` 做的事：记下上行网卡的 `IP/掩码`、默认网关、MAC → 建 `br0` 并**继承上行的 MAC**
（上游交换机/网关看到的 MAC 不变，ARP 表不用重学）→ 把上行网卡的 IP 搬到 `br0` 上 →
把上行网卡改成 `br0` 的端口。

建好之后**不需要改启动脚本**：`scripts/run-linux.sh` 检测到
`/sys/class/net/$NET_BRIDGE_IF/bridge` 存在就自动加上 `-net-tap` 参数；
桥不在时不加（指向不存在的桥会让模拟器直接起不来）。

### ⚠️ 自动回滚

`up` 会把上行网卡的 IP 搬走，中间断链约 0.1 秒。如果上行网卡正是你 SSH 进来的那块，
你会短暂掉线。所以 `up` **先武装一个 systemd 瞬时定时器**再动网络：

* 90 秒（`CONFIRM_WINDOW`）内执行 `confirm` → 撤掉定时器，桥保留；
* 没确认 → 定时器触发 `down` 自动还原。

定时器跑在 systemd 里，**不依赖你的 SSH 会话** —— 会话断了照样会回滚。

### 不持久化

桥只存在于运行期（状态在 `/run/remote-control-net-bridge.state`），**重启后消失**，
机器回到 `iface ens33 inet dhcp` 的原状。要开机自动桥接，把 `/etc/network/interfaces`
改写成 `br0` + `ens33` 的形式（本项目刻意不这么做：宿主网络的持久化改动风险高，
而 `up` 一条命令就能重建）。

---

## 3. ROM 侧：为什么必须声明 `android.hardware.ethernet`

**这是整条路上最容易漏的一环。** 只做宿主桥接的话，实测结果是这样：

```
guest 侧：eth0 = 192.168.99.53/24        ← 真实 DHCP 租约，二层链路是通的
Android：Active default network: none    ← 但它根本不用这张网卡
```

原因：`SystemServer` 只在设备声明了以太网特性时才启动 `EthernetService`：

```java
// frameworks/base/services/java/com/android/server/SystemServer.java:1897
if (mPackageManager.hasSystemFeature(PackageManager.FEATURE_ETHERNET) ||
        mPackageManager.hasSystemFeature(PackageManager.FEATURE_USB_HOST)) {
    t.traceBegin("StartEthernet");
    mSystemServiceManager.startService(ETHERNET_SERVICE_CLASS);
```

没声明 → `dumpsys ethernet` 直接报 `Can't find service: ethernet` → `eth0` 即使拿到
真实 IP 也不会成为 Android 的网络，应用流量仍然只走 Wi-Fi（NAT）。桥接等于白做。

修法是**一行**（`device/remote_control_x64_arm64/device.mk`，写法照抄 goldfish 自己的
`device/generic/goldfish/fvp.mk:87`）：

```make
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml
```

特性生效后，`EthernetTracker` 因为 `config_ethernet_interfaces` 为空，会回落到
`config_ethernet_iface_regex`（默认 `eth\d`）自动接管 `eth0`
（`frameworks/opt/net/ethernet/.../EthernetTracker.java:133 / 636 / 382`）——
**因此不需要再写资源 overlay**。

> 特性声明的副作用：即使不走桥接（走默认 NAT），`eth0` 也会被接管成一张以太网网络
> （10.0.2.15，经 SLIRP 有真实出网能力）。以太网评分（70）高于 Wi-Fi（60），
> 默认网络会从 Wi-Fi 变成以太网 —— 功能上无碍。

---

## 4. 实测证据

宿主是无 VMware NAT 的**桥接网卡**虚拟机，所以 VM 内再做桥接能落到物理局域网
（判据：网关 MAC `60:be:b4:04:49:93` 不是 VMware 的 `00:50:56:*` 前缀）。

零风险预演（不接上行网卡、只建 host-only 桥 + busybox DHCP）：

```
tap-test / tap-wifi 被模拟器创建并挂进 br-test        ✓
androidboot.qemu.skin=720x1280                        ✓
Setting display: 0 configuration to: 720x1280 dpi 320 ✓
guest eth0 = 192.168.99.53/24（udhcpd 发的真实租约）   ✓
Active default network: none                          ← ROM 缺以太网特性
```

### 落地验证（接入物理局域网后）

宿主桥：`br0 = 192.168.0.108/24`（继承 ens33 的 MAC），端口 `ens33 + tap0`。
模拟器由 `run-linux.sh` 自动带 `-net-tap tap0` 启动，guest 侧实测：

```
eth0                        = 192.168.0.110/24      ← 物理路由器发的真实租约
service list                = 71 ethernet: [android.net.IEthernetManager]
dumpsys ethernet            = Ethernet interface name filter: eth\d
                              Default interface: eth0
network{100}                = ni{Ethernet CONNECTED}  Score(70)
Active default network      = 100                    ← 以太网 70 > Wi-Fi 60
宿主 ping 192.168.0.110      = ✓ 通
guest ping 192.168.0.1       = ✓ 通
ip neigh 192.168.0.110       = dev br0 lladdr 52:54:00:12:34:56 REACHABLE
宿主 → 192.168.0.110:9999     = LAN_REACH_OK（guest 里临时起的监听，真实 TCP 连接）
对照 10.0.2.15:9999          = 不通（NAT 内网段，宿主根本没有这条路由）
run-linux.sh 四项验收         = 全部通过（Boot completed in 28558 ms）
```

> **MAC 冲突提醒**：guest 用的 MAC 是 QEMU 默认的 `52:54:00:12:34:56`，
> 模拟器没有暴露改 MAC 的参数。**同时跑两个桥接实例会在局域网上撞 MAC**，
> 要并行就得走 `-qemu` 手工指定（本项目未做）。

---

## 5. 回滚

```bash
sudo tools/net-bridge.sh down
```

把 IP/默认路由还给上行网卡、删掉 `br0`。模拟器侧不用改：桥没了之后
`run-linux.sh` 下次启动就不会再加 `-net-tap`，自动退回用户态 NAT。

---

## 6. Windows 侧（WHPX）

`-net-tap` 在 Windows 上同样存在，但需要一个 TAP 虚拟网卡驱动
（OpenVPN 的 `tap-windows6`，装完在网络连接里把它和物理网卡一起桥接）。
`windows/run-windows.ps1` 目前**没有**接桥接参数，Windows 侧仍走默认 NAT +
`adb forward`。要接的话是同一套参数，只是"建桥"那步改成在 Windows 的
"网络连接 → 桥接"里手工做一次。
