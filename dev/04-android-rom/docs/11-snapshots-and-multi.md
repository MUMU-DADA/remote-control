# 11 · 快照与多实例

## 速览

| 能力 | 状态 | 怎么做 |
|---|---|---|
| 保存快照 | ✓ 实测 | `adb -s emulator-<port> emu avd snapshot save <名>` |
| 列出 / 删除 | ✓ | `... emu avd snapshot list` / `... emu avd snapshot delete <名>` |
| 从快照恢复 | ✓ 实测 **7 秒** | `./scripts/run-linux.sh --snapshot <名>` |
| 再开一台机器 | ✓ 实测 | `./scripts/run-linux.sh --port 5584` |
| 保留已装应用 / 状态 | ✓ | `./scripts/run-linux.sh --port 5584 --reuse` |
| 多台同时桥接到物理 LAN | ✗ 有硬限制 | guest MAC 全都一样（见 §4） |

---

## 1. 快照：支持，而且很快

实测（自编 ROM / x86_64 + KVM）：

```
$ adb -s emulator-5586 emu avd snapshot save snap2
OK
$ adb -s emulator-5586 emu avd snapshot list
List of snapshots present on all disks:
ID        TAG                 VM SIZE                DATE       VM CLOCK
--        snap2                   67M 2026-09-28 14:11:25   00:00:19.457

# 硬杀掉进程（不给它自动保存的机会），再从快照启动：
$ ./run-linux.sh --port 5586 --snapshot snap2
Loading snapshot 'snap2'...
→ 从进程启动到 sys.boot_completed=1：7 秒      （冷启动 21~28 秒）
→ 快照前写入 /data/local/tmp/snapmark2 的内容原样还在
```

**快照存在哪**：`<工作目录 sysdir>/snapshots/<名>/`，每个约 **1.1 G**，里面是
`ram.bin` + `textures.bin` + `snapshot.pb` + `hardware.ini` + `screenshot.png`
（`list` 里显示的 "VM SIZE 67M" 只是内存镜像的压缩后大小）。

> 顺带一个容易误判的点：实例的**全部状态**都在工作目录里 —— `build.avd/`、
> `*.qcow2`（userdata/cache/encryptionkey 的 qcow2 覆盖层）、`snapshots/`。
> 所以"删工作目录"就等于"这台机器彻底消失"。

---

## 2. 三个前提，缺一不可

**① `fastboot.forceColdBoot` 必须是 `no`。**
ROM 上游的 `config.ini.xl` 第 3 行写的是 `yes`，含义是"永远冷启动、忽略快照"。
不改它，`-snapshot` 会被**静默丢掉**：

```
WARNING | ignoring -snapshot option due to the use of -no-snapshot.
```

（注意：这个 `-no-snapshot` 不是你传的，是 `forceColdBoot=yes` 带出来的。
另外 `-no-snapshot` 本身并**不禁止手动存取快照** —— 实测在它下面 `snapshot save` 照样成功。）

`--snapshot <名>` 会自动把工作目录里那份 config.ini 改成 `no`。项目源文件
`emulator/config.ini` 保持 `yes`：验收要的是**确定性**，默认每次全新冷启动。

**② `hardware-qemu.ini` 必须与存档时逐项一致。**
模拟器会拿当前硬件配置和快照里存的 `hardware.ini` 对比，不一致就拒绝加载：

```
Loading snapshot 'snap1'...
ERROR | The emulator hardware cannot load snapshot: snap1
WARNING | Failed to load snapshot 'snap1'
```

实测触发它的差异就是第 ① 条本身（存的时候 `forceColdBoot = true`，加载时 `false`）。
**所以顺序必须是：先定好配置 → 再存快照。** 改过 `config.ini` 或任何 `hw.*`
之后，旧快照全部作废，得重新存。

**③ 工作目录不能删。**
`run-linux.sh` 默认每次启动都 `rm -rf .run/sysdir-<port>` 重建 —— 那是"每次一台
新机器"的语义，快照会跟着一起没。`--reuse` / `--snapshot` 会保留工作目录。

发布包里的 Linux、Darwin 和 Windows `start-headless` 入口会先检查端口登记与目标
工作目录：端口已登记给另一个实例时拒绝启动；新实例若遇到未登记但已有的数据目录，
也会拒绝复用。这样显式指定已停止实例的端口，不会把该实例的状态当成新实例数据
覆盖。请用对应实例名和 `--reuse`（PowerShell 为 `-Reuse`）恢复已有状态。

---

## 3. 多开一台机器

```bash
./scripts/run-linux.sh --port 5584              # 一键：再开一台，全新
./scripts/run-linux.sh --port 5584 --reuse      # 保留已装应用与快照
./scripts/run-linux.sh --port 5584 --verify     # 只对它做验收
./scripts/run-linux.sh --port 5584 --stop       # 只停它
```

实测两台并存互不干扰：

```
emulator-5580  -sysdir .run/sysdir-5580  -datadir .run/datadir-5580  桥接 → eth0 192.168.0.110/24
emulator-5584  -sysdir .run/sysdir-5584  -datadir .run/datadir-5584  NAT  → eth0 10.0.2.15/24
             （第二台验收同样全部通过：Boot completed in 21352 ms）
```

每台实例独立持有三样东西，这是互不干扰的关键：

* `-sysdir .run/sysdir-<port>` —— 镜像（符号链接到交付目录，只读）+ 状态 + 快照
* `-datadir .run/datadir-<port>`
* `-net-tap tap<port>` —— TAP 名字按端口派生

> 这三样是踩出来的：`-datadir` 写死成 `.run/datadir`（且目录不存在）时，AOSP 自带
> 模拟器直接 `ERROR: Invalid -datadir directory` 退出；TAP 写死成 `tap0` 时，第二台
> 报 `could not configure /dev/net/tun (tap0): Device or resource busy`。

---

## 4. 硬限制：多台不能同时桥接到物理 LAN

guest 的 `eth0` MAC 是 QEMU 的默认值 `52:54:00:12:34:56`，**所有实例都一样**，
而模拟器没有暴露改 eth0 MAC 的参数（只有 `-wifi-mac-address`，且仅对 wlan0 +
`-read-only` 快照模式生效）。两台同时桥接 → 局域网上 MAC 冲突。

所以：**同时只让一台上物理 LAN，其余走默认 NAT**。要关掉某台的桥接：

```bash
NET_BRIDGE_IF= ./scripts/run-linux.sh --port 5584     # 空值 = 明确不走桥
```

（`common.sh` 里用的是 `${NET_BRIDGE_IF-br0}` 而不是 `${NET_BRIDGE_IF:-br0}` ——
后者对空值也会替换成默认值，那样就关不掉了。这个坑实测踩过。）
