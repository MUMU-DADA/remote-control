#!/usr/bin/env bash
# check-no-build.sh —— 检查容器里是否有编译在跑
#
# 退出码：0 = 有编译在跑，1 = 没有
#
# 为什么要单独成文件：判断逻辑要排除僵尸进程，写成一行塞进别的脚本里
# 会变成引号地狱。容器的 PID 1 是 `sleep infinity`，它不回收子进程 ——
# 被强杀的 soong_build 会以 <defunct> 永久留在进程表里，
# 而 pgrep -f 照样能匹配到它，导致防重入检查一直误报。

set -uo pipefail

CONTAINER=${CONTAINER:-remote-control-builder}

docker exec "$CONTAINER" ps -eo stat,comm --no-headers 2>/dev/null \
    | awk '$1 !~ /^Z/ && $2 ~ /^(soong_ui|soong_build|ckati|ninja)$/ { found = 1 } END { exit !found }'
