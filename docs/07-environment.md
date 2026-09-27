# 07 · 开发环境

> 本机实际环境配置记录。**全程使用国内镜像源。**

---

## 1. 硬件

| 项 | 配置 |
|---|---|
| CPU | AMD Ryzen 9 5950X，**16 vCPU** |
| 内存 | **31 GiB** |
| Swap | **35 GiB**（34 GiB 专用分区 + 1.1 GiB 原有） |
| 数据盘 | **458 GiB** ext4（`/dev/sda1`，挂在项目目录） |
| 宿主系统 | Debian GNU/Linux 13 (trixie)，内核 6.12 |
| 虚拟化 | VMware 虚拟机，`/dev/kvm` 可用 |

**并行度建议：`-j12`。** 经验值约 2 GB 内存 / 并行任务，31 GiB 内存跑 `-j16` 会触发 OOM。

---

## 2. 磁盘布局

```
/dev/sda  500 GiB  VMware Virtual S
├── sda1  466 GiB  ext4  →  /root/AutoSnapshotAndroid
│   ├── README.md, SUMMARY.md, docs/, dev/, tools/    ← 项目文件
│   └── aosp/                                          ← AOSP 源码树
└── sda2   34 GiB  swap

/dev/sdb   20 GiB  VMware Virtual S
├── sdb1  18.5 GiB ext4  →  /          （根分区，剩 ~16 GiB）
└── sdb5   1.1 GiB swap
```

**为什么要分两个分区**：swapfile 放在项目目录里会被工作区同步机制当成普通文件下载，
一个 34 GB 的文件会造成同步灾难。用独立 swap 分区彻底避开。

`/etc/fstab`：

```
UUID=ecbe983c-41a1-46eb-974b-dc61c52cdabe /root/AutoSnapshotAndroid ext4 defaults,noatime 0 2
UUID=e86f4d06-0dbc-4f6f-bf9b-19603ec93917 none swap sw,pri=10 0 0
```

`noatime` 减少构建期间的写入量。

---

## 3. 镜像源

**全部走国内源**，因为 `android.googlesource.com` 在本机不可达。

| 环节 | 源 |
|---|---|
| Debian apt | `https://mirrors.tuna.tsinghua.edu.cn/debian` |
| Docker CE 安装 | `https://mirrors.tuna.tsinghua.edu.cn/docker-ce` |
| Docker Hub 镜像拉取 | `https://docker.m.daocloud.io` |
| Ubuntu apt（容器内） | `https://mirrors.tuna.tsinghua.edu.cn/ubuntu` |
| pip | `https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple` |
| repo 工具 | `https://mirrors.tuna.tsinghua.edu.cn/git/git-repo` |
| AOSP 源码 | `https://mirrors.tuna.tsinghua.edu.cn/git/AOSP` |

### 两个关键坑

**坑 1：TUNA 的镜像是 git bare 仓库，不能用浏览器/HTTP 直链访问。**

`https://mirrors.tuna.tsinghua.edu.cn/git/AOSP/platform/manifest` 用 wget 访问会返回 404，
但用 `git ls-remote` 完全正常。**测连通性要用 git，不能用 wget。**

同理，`repo` 工具不能从 `.../git-repo/repo` 直接下载单文件，必须 `git clone`。

**坑 2：git 重定向配置**

容器内已配置系统级 `insteadOf`，所有 google 源自动走清华：

```bash
git config --system url.https://mirrors.tuna.tsinghua.edu.cn/git/AOSP/.insteadof \
    https://android.googlesource.com
git config --system url.https://mirrors.tuna.tsinghua.edu.cn/git/git-repo.insteadof \
    https://gerrit.googlesource.com/git-repo
```

再加环境变量 `REPO_URL` 指向清华的 git-repo 镜像，避免 repo 自更新时去连 gerrit。

---

## 4. 构建容器

宿主是 Debian 13（gcc 14 / Python 3.13），而 **AOSP 12 官方只支持 Ubuntu 20.04/22.04**。
直接编会踩一堆兼容性问题，所以用容器隔离。

```bash
docker exec -it autod-builder bash     # 进入构建环境
```

| 项 | 值 |
|---|---|
| 镜像 | `autod-aosp12-builder`（基于 `ubuntu:22.04`） |
| 容器 | `autod-builder` |
| 挂载 | `/root/AutoSnapshotAndroid/aosp` ↔ `/aosp` |
| Java | OpenJDK 11（**AOSP 12 要求 JDK 11，不是 17**） |
| Python | 3.10 |

### AOSP 12 的依赖坑

AOSP 官方文档列的 `unicode-terminfo` **在 Ubuntu 22.04 已不存在**，装了会直接失败。

32 位兼容包（`libc6-dev-i386`、`lib32ncurses5-dev` 等）在 22.04 里部分改名，
脚本里做成了可选组，装不上就跳过——arm64 目标构建基本用不到。

---

## 5. 目录结构

```
/root/AutoSnapshotAndroid/          458 GiB 挂载点
├── README.md                       项目索引
├── SUMMARY.md                      方案总结与决策记录
├── docs/                           专题文档
├── dev/                            三条开发轨道
├── tools/
│   ├── setup-host.sh               环境初始化（本次已执行）
│   └── repo-sync.sh                AOSP 源码同步
└── aosp/                           AOSP 源码树（~85 GB）
    ├── .repo/
    ├── build/
    ├── frameworks/
    └── ...
```

> ⚠️ **`aosp/` 必须独立成子目录。** 早期版本把源码树直接放在项目根目录，
> 结果 AOSP 的 `art/`、`bionic/`、`build/` 等目录和项目文件混在同一层。

---

## 6. 常用命令

### AOSP 源码

```bash
# 进入构建环境
docker exec -it autod-builder bash

# 在容器内
cd /aosp
source build/envsetup.sh
lunch aosp_arm64-userdebug       # 只编二进制，不需要 vendor blobs
m autod autodctl                 # 编我们自己的模块（分钟级）

# 产物
ls $ANDROID_PRODUCT_OUT/system/bin/
```

### 首次同步（已完成）

```bash
setsid nohup bash tools/repo-sync.sh > /var/log/aosp-sync.log 2>&1 &
tail -f /var/log/aosp-sync.log
```

### 查看同步进度

```bash
du -sh /root/AutoSnapshotAndroid/aosp
df -h /root/AutoSnapshotAndroid
docker exec autod-builder pgrep -f "repo/main.py" >/dev/null && echo 运行中 || echo 已停止
```

---

## 7. 注意事项

### ⚠️ 不要对工作区做全量同步

`aosp/` 里有 10 万+ 文件、~85 GB。工作区镜像机制会遍历整个目录树，
全量同步会试图下载 AOSP 源码。

**需要取文件时，用单文件下载（`rw_download`），不要用全量同步。**

### 并行度

内存 31 GiB，建议 `-j12`。`repo sync` 用 `-j4`（清华镜像站限流，调大会大量 503）。

### 磁盘占用预期

| 阶段 | 占用 |
|---|---|
| 源码（shallow） | ~85 GB |
| `out/` 只编 `autod` | ~35 GB |
| `out/` 全量编系统镜像 | ~100 GB |
| 可用空间 | **449 GB** |

空间充裕，不需要开 ccache 省，也不需要频繁清理。

---

## 8. 相关文档

- 硬件选型依据 → `04-hardware.md`
- 源码同步后的下一步 → `../dev/02-native-daemon/README.md`
- 方案总览 → `../SUMMARY.md`
