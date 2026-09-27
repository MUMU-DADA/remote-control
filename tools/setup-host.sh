#!/usr/bin/env bash
# =============================================================================
# setup-host.sh —— 宿主机开发环境初始化（全程国内镜像源）
#
# 在一台干净的 Debian 13 上装好 AOSP 开发所需的一切。
#
# 镜像源规划（全部国内）：
#   Debian apt        → 清华 TUNA（系统自带）
#   Docker CE 安装    → 清华 TUNA
#   Docker Hub 镜像   → docker.m.daocloud.io
#   Ubuntu apt（容器）→ 清华 TUNA
#   pip               → 清华 TUNA
#   repo 工具         → 清华 TUNA git-repo（git 协议，非 HTTP 直链）
#   AOSP 源码         → 清华 TUNA AOSP
#
# 用法:  bash tools/setup-host.sh 2>&1 | tee /var/log/autod-setup.log
# =============================================================================
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

TUNA=https://mirrors.tuna.tsinghua.edu.cn
AOSP_MIRROR="$TUNA/git/AOSP"
REPO_MIRROR="$TUNA/git/git-repo"
DOCKER_MIRROR=https://docker.m.daocloud.io

PROJECT_DIR=/root/AutoSnapshotAndroid
IMAGE_TAG=autod-aosp12-builder
CONTAINER=autod-builder

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m  ! %s\033[0m\n' "$*" >&2; }

# -----------------------------------------------------------------------------
log "1/6  宿主机基础工具"
# -----------------------------------------------------------------------------
apt-get update -qq
apt-get install -y -qq \
    ca-certificates curl wget gnupg lsb-release \
    git python3 python3-pip python3-venv \
    unzip zip rsync htop tree vim bc file jq \
    parted e2fsprogs
ok "基础工具就绪"

# -----------------------------------------------------------------------------
log "2/6  配置 pip 国内源"
# -----------------------------------------------------------------------------
mkdir -p /etc
cat > /etc/pip.conf <<EOF
[global]
index-url = $TUNA/pypi/web/simple
trusted-host = mirrors.tuna.tsinghua.edu.cn
timeout = 60
EOF
ok "pip → $TUNA/pypi/web/simple"

# -----------------------------------------------------------------------------
log "3/6  Docker（清华安装源 + 国内镜像加速）"
# -----------------------------------------------------------------------------
if ! command -v docker >/dev/null 2>&1; then
    install -m 0755 -d /etc/apt/keyrings
    wget -q -O /etc/apt/keyrings/docker.asc "$TUNA/docker-ce/linux/debian/gpg"
    chmod a+r /etc/apt/keyrings/docker.asc
    CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
    echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] \
$TUNA/docker-ce/linux/debian $CODENAME stable" > /etc/apt/sources.list.d/docker.list
    apt-get update -qq
    apt-get install -y -qq docker-ce docker-ce-cli containerd.io \
        docker-buildx-plugin docker-compose-plugin
    ok "Docker CE 已安装（来自 TUNA）"
else
    ok "Docker 已安装，跳过"
fi

# Docker Hub 镜像加速。默认源在国内基本拉不动。
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<EOF
{
  "registry-mirrors": ["$DOCKER_MIRROR"],
  "log-driver": "json-file",
  "log-opts": { "max-size": "50m", "max-file": "3" },
  "data-root": "/var/lib/docker"
}
EOF
systemctl enable --now docker
systemctl restart docker
sleep 4

if docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    ok "Docker 运行中 (v$(docker version --format '{{.Server.Version}}'))"
    ok "Registry 镜像 → $DOCKER_MIRROR"
else
    warn "Docker 未就绪: systemctl status docker"
    exit 1
fi

# -----------------------------------------------------------------------------
log "4/6  构建 Ubuntu 22.04 构建镜像"
# -----------------------------------------------------------------------------
# AOSP 12 官方只支持 Ubuntu 20.04/22.04。宿主是 Debian 13（gcc 14 / Python 3.13），
# 直接编会踩兼容性问题，所以用容器隔离。
if docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    ok "镜像 $IMAGE_TAG 已存在，跳过"
else
    WORK=$(mktemp -d)
    cat > "$WORK/Dockerfile" <<DOCKERFILE
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Shanghai
ENV LANG=C.UTF-8
ENV REPO_URL=$REPO_MIRROR

# ---- Ubuntu apt 换清华源 ----
RUN sed -i \
      -e 's@//.*archive.ubuntu.com@//mirrors.tuna.tsinghua.edu.cn@g' \
      -e 's@//.*security.ubuntu.com@//mirrors.tuna.tsinghua.edu.cn@g' \
      /etc/apt/sources.list

# ---- pip 换清华源 ----
RUN mkdir -p /etc && printf '[global]\nindex-url = $TUNA/pypi/web/simple\ntrusted-host = mirrors.tuna.tsinghua.edu.cn\ntimeout = 60\n' > /etc/pip.conf

# ---- AOSP 12 构建依赖 ----
# 必需组：装不上直接失败
# 可选组：32 位兼容库与老式 X11 头文件。Ubuntu 22.04 里部分已改名或移除，
#         arm64 目标构建基本用不到，装不上就跳过。
# 注：AOSP 官方文档列的 unicode-terminfo 在 Ubuntu 22.04 已不存在。
RUN apt-get update && apt-get install -y --no-install-recommends \
        git-core gnupg flex bison build-essential zip unzip curl zlib1g-dev \
        libxml2-utils xsltproc fontconfig \
        python3 python-is-python3 python3-pip \
        openjdk-11-jdk-headless \
        ccache rsync bc kmod cpio file less sudo \
    && ( apt-get install -y --no-install-recommends \
            libc6-dev-i386 libncurses5 lib32ncurses5-dev \
            x11proto-core-dev libx11-dev lib32z1-dev libgl1-mesa-dev \
         || echo "  [warn] 部分 32 位兼容包不可用，不影响 arm64 目标构建" ) \
    && rm -rf /var/lib/apt/lists/*

# ---- repo 工具：从清华 git-repo 镜像 clone ----
# 注意：TUNA 的镜像是 git bare 仓库，不能用 HTTP 直链下载单文件，
#       必须用 git clone。这也是 TUNA 官方帮助页推荐的方式。
RUN git clone --depth=1 $REPO_MIRROR /opt/git-repo \
    && ln -s /opt/git-repo/repo /usr/local/bin/repo \
    && chmod a+x /opt/git-repo/repo

# ---- git 全局配置：google 源全部重定向到清华镜像 ----
RUN git config --system \
      url.$AOSP_MIRROR/.insteadof https://android.googlesource.com \
 && git config --system \
      url.$REPO_MIRROR.insteadof https://gerrit.googlesource.com/git-repo \
 && git config --system user.name  "AOSP Builder" \
 && git config --system user.email "builder@localhost" \
 && git config --system core.compression 0

WORKDIR /aosp
DOCKERFILE

    docker build -t "$IMAGE_TAG" "$WORK"
    rm -rf "$WORK"
    ok "镜像 $IMAGE_TAG 构建完成"
fi

# -----------------------------------------------------------------------------
log "5/6  创建构建容器"
# -----------------------------------------------------------------------------
if docker container inspect "$CONTAINER" >/dev/null 2>&1; then
    docker rm -f "$CONTAINER" >/dev/null
fi

# --privileged 是 Cuttlefish 的硬前提，不是图省事。
#
# Cuttlefish 要的东西（已对照 device/google/cuttlefish 源码确认）：
#   /dev/kvm           KVM 加速；crosvm_manager 用它起虚拟机
#   /dev/net/tun       OpenTapInterface() 创建 TAP 网卡
#   /dev/vhost-net     crosvm 传 --vhost-net
#   /dev/vhost-vsock   guest 与 host 的 vsock 通信
#   CAP_NET_ADMIN      配置 TAP 和 DHCP
#
# 只加 --device /dev/kvm 是不够的 —— 那样编译没问题（编译根本不需要这些），
# 但 launch_cvd 会在创建网卡时失败。
docker run -d \
    --name "$CONTAINER" \
    --hostname builder \
    --privileged \
    -e REPO_URL="$REPO_MIRROR" \
    -v "$PROJECT_DIR/aosp":/aosp \
    -w /aosp \
    "$IMAGE_TAG" \
    sleep infinity
ok "容器 $CONTAINER 已创建（$PROJECT_DIR/aosp ↔ /aosp）"

docker exec "$CONTAINER" bash -lc '
  echo "  系统  : $(. /etc/os-release && echo $PRETTY_NAME)"
  echo "  Java  : $(java -version 2>&1 | head -1)"
  echo "  Python: $(python3 --version)"
  echo "  repo  : $(repo --version 2>&1 | head -1)"
  echo "  镜像  : $(git config --system --get-regexp "url\..*insteadof" | head -2 | tr "\n" " ")"
'

# -----------------------------------------------------------------------------
log "6/6  环境汇总"
# -----------------------------------------------------------------------------
echo
echo "宿主机 ($(. /etc/os-release && echo "$PRETTY_NAME")):"
printf "  %-10s %s\n" "CPU"   "$(nproc) 核"
printf "  %-10s %s\n" "内存"  "$(free -h | awk '/内存|Mem:/{print $2}')"
printf "  %-10s %s\n" "Swap"  "$(free -h | awk '/交换|Swap:/{print $2}')"
printf "  %-10s %s\n" "数据盘" "$(df -h $PROJECT_DIR | tail -1 | awk '{print $2", 可用 "$4}')"
echo
echo "镜像源:"
printf "  %-14s %s\n" "Debian apt"   "$TUNA/debian"
printf "  %-14s %s\n" "Docker CE"    "$TUNA/docker-ce"
printf "  %-14s %s\n" "Docker Hub"   "$DOCKER_MIRROR"
printf "  %-14s %s\n" "Ubuntu apt"   "$TUNA/ubuntu"
printf "  %-14s %s\n" "pip"          "$TUNA/pypi/web/simple"
printf "  %-14s %s\n" "repo 工具"    "$REPO_MIRROR"
printf "  %-14s %s\n" "AOSP 源码"    "$AOSP_MIRROR"
echo
echo "进入构建环境:"
echo "  docker exec -it $CONTAINER bash"
echo
echo "下一步（容器内）:"
echo "  cd /aosp"
echo "  repo init -u $AOSP_MIRROR/platform/manifest -b android-12.0.0_r34"
echo "  repo sync -c --depth=1 --no-tags -j4    # 镜像站限流，-j 不要超过 4"
echo
ok "全部完成"
