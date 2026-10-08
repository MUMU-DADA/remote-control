#!/bin/bash
# 文件管理的边界验证：放宽到存储根之后，越界必须仍然被拒
B=${REMOTE_CONTROL_BASE:-http://127.0.0.1:8088}/api/v1
TOKEN=${REMOTE_CONTROL_TOKEN:-${RC_TOKEN:-}}
pass=0; fail=0

# 所有请求都带上令牌。
#
# ⚠️ 以前这里**没有**令牌，而交付镜像默认开鉴权 —— 后果不是"测试失败"，
#    而是**大面积假通过**：每一条"越界必须被拒"都拿到了 401，被当成
#    "拒绝成功"记成 ✓，13 个 ✓ 里没有一条真的验过边界。
#    这正是浏览器套件踩过的同一个坑（见 wctest_token.py）。
j() {
  if [ -n "$TOKEN" ]; then
    curl -s --max-time 10 -H "Authorization: Bearer $TOKEN" "$@"
  else
    curl -s --max-time 10 "$@"
  fi
}

# 没有令牌就别往下跑 —— 见上面那段。宁可红，不要绿得没有意义。
_probe=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "${B}/info")
if [ "$_probe" = "401" ] && [ -z "$TOKEN" ]; then
  echo "✗ 服务开了鉴权，但没给令牌：越界检查会全部假通过。"
  echo "  请设 REMOTE_CONTROL_TOKEN（或 RC_TOKEN）后重跑。"
  exit 2
fi
ok()  { echo "  ✓ $1"; pass=$((pass+1)); }
bad() { echo "  ✗ $1  ← $2"; fail=$((fail+1)); }

# 期望成功
should_ok() {
  local desc="$1"; shift
  local r; r=$(j "$@")
  case "$r" in *'"ok":true'*) ok "$desc";; *) bad "$desc" "$(echo "$r" | head -c 120)";; esac
}
# 期望被拒
should_fail() {
  local desc="$1"; shift
  local r; r=$(j "$@")
  case "$r" in *'"ok":true'*) bad "$desc（竟然通过了！）" "$(echo "$r" | head -c 120)";;
                *) ok "$desc → $(echo "$r" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("error","")[:60])
except: print("")' 2>/dev/null)";; esac
}

echo "── 允许的操作 ──"
should_ok   "在 /sdcard 建目录"        -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/sdcard/remote-control-testdir"}'
should_ok   "stat 它"                  "$B/files?path=/sdcard/remote-control-testdir"
should_ok   "重命名"                   -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"rename","path":"/sdcard/remote-control-testdir","to":"/sdcard/remote-control-testdir2"}'
should_ok   "相对路径仍相对 Download"  -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"sub1"}'
should_ok   "删除（递归）"             -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/sdcard/remote-control-testdir2","recursive":true}'
should_ok   "删除相对目录"             -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"sub1","recursive":true}'

echo
echo "── 越界必须被拒 ──"
should_fail "GET 不允许写操作"       "$B/files?op=delete&path=/sdcard"
should_fail "列 /data"                 "$B/files?path=/data"
should_fail "列 /system"               "$B/files?path=/system"
should_fail "列 /"                     "$B/files?path=/"
should_fail "列 /data/local/tmp"       "$B/files?path=/data/local/tmp"
should_fail ".. 逃逸"                  "$B/files?path=/sdcard/../../data"
should_fail "相对路径 .. 逃逸"         "$B/files?path=../../data"
should_fail "删 /system/bin"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/system/bin","recursive":true}'
should_fail "删共享存储根"               -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/sdcard","recursive":true}'
should_fail "mkdir 到 /data"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/data/evil"}'
should_fail "重命名跨出边界"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"rename","path":"/sdcard/x","to":"/data/evil"}'
should_fail "重命名共享存储根"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"rename","path":"/sdcard","to":"/sdcard/remote-control-root-renamed"}'
should_ok   "准备覆盖测试源目录"         -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/sdcard/remote-control-root-target-testdir"}'
should_fail "覆盖共享存储根"             -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"rename","path":"/sdcard/remote-control-root-target-testdir","to":"/sdcard"}'
should_ok   "清理覆盖测试源目录"         -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/sdcard/remote-control-root-target-testdir","recursive":true}'

echo
echo "── 软链接逃逸 ──"
# 在 /sdcard 下造一个指向 /data 的软链接，走它必须被拒
VAULT=$(j -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/sdcard/remote-control-linktest"}')
LINK=$(adb ${REMOTE_CONTROL_SERIAL:+-s $REMOTE_CONTROL_SERIAL} shell "ln -s /data /sdcard/remote-control-linktest/escape 2>&1; echo rc=\$?" | tr -d '\r')
if echo "$LINK" | grep -q "rc=0"; then
  should_fail "穿过指向 /data 的软链接" "$B/files?path=/sdcard/remote-control-linktest/escape"
  adb ${REMOTE_CONTROL_SERIAL:+-s $REMOTE_CONTROL_SERIAL} shell "rm -f /sdcard/remote-control-linktest/escape" >/dev/null 2>&1
else
  # 共享存储（/sdcard 是 FUSE）**根本建不了软链接** —— 报的是
  # "Function not implemented"。所以这条不是"没验到"，而是**这条路走不通**：
  # 攻击者也没法在允许的根里造出软链接来穿。真正的越界靠 O_NOFOLLOW
  # 逐级打开父目录挡着，那部分由上面那批 .. 逃逸用例覆盖。
  echo "  ⊘ 共享存储上建不了软链接（FUSE 不支持），这条路走不通："
  echo "     $(echo "$LINK" | head -1)"
fi
j -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/sdcard/remote-control-linktest","recursive":true}' >/dev/null

echo
echo "  通过 $pass 项，失败 $fail 项"
if [ -z "$TOKEN" ]; then
  echo "  （未鉴权模式：此时'被拒'是权限模型本身给的，不是边界检查给的）"
fi
exit $(( fail > 0 ))
