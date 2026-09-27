#!/usr/bin/env bash
# =============================================================================
# repo-sync.sh —— 拉取 AOSP 12 源码（全程清华 TUNA 镜像）
#
# 用法（宿主机）:
#   setsid nohup bash tools/repo-sync.sh > /var/log/aosp-sync.log 2>&1 &
#   tail -f /var/log/aosp-sync.log
#
# 规模: 1048 个项目，约 85 GB（shallow clone），视网速 1-4 小时
# =============================================================================
set -euo pipefail

CONTAINER=autod-builder
BRANCH=android-12.0.0_r34
AOSP_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/git/AOSP
REPO_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo
JOBS=4          # 清华镜像站限流，-j 超过 4 会大量 503

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

log "检查容器"
docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true \
    || { echo "容器 $CONTAINER 未运行，先跑 tools/setup-host.sh"; exit 1; }

# -----------------------------------------------------------------------------
log "repo init（$BRANCH, shallow）"
# -----------------------------------------------------------------------------
# 注意：--depth 和 --no-tags 是 repo init 的参数，不是 repo sync 的。
#       --depth=1 意味着浅克隆，省约 25 GB，但之后不能切分支。
docker exec "$CONTAINER" bash -lc "
    set -e
    cd /aosp
    export REPO_URL=$REPO_MIRROR

    # 已初始化则检查 depth 是否生效；没生效就清掉重来
    if [ -d .repo ]; then
        if grep -q 'depth = 1' .repo/manifests.git/config 2>/dev/null; then
            echo '  已初始化且 depth=1，跳过'
        else
            echo '  已有 .repo 但不是浅克隆，重建'
            rm -rf .repo
        fi
    fi

    if [ ! -d .repo ]; then
        repo init -u $AOSP_MIRROR/platform/manifest -b $BRANCH --depth=1 --no-tags
    fi
"

# -----------------------------------------------------------------------------
log "repo sync（-j$JOBS，预计 1-4 小时）"
# -----------------------------------------------------------------------------
docker exec "$CONTAINER" bash -lc "
    set -e
    cd /aosp
    repo sync -c --no-clone-bundle -j$JOBS
"

# -----------------------------------------------------------------------------
log "完成，统计"
# -----------------------------------------------------------------------------
docker exec "$CONTAINER" bash -lc "
    cd /aosp
    echo \"  项目数  : \$(ls -1 | wc -l)\"
    echo \"  源码    : \$(du -sh --exclude=.repo . 2>/dev/null | cut -f1)\"
    echo \"  .repo   : \$(du -sh .repo 2>/dev/null | cut -f1)\"
    echo
    echo '  下一步: 删除不需要的部分以节省空间'
    echo '    repo sync -c --no-clone-bundle -j4 --fail-fast'
"
