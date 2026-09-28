#!/bin/bash
# 文件管理的边界验证：放宽到存储根之后，越界必须仍然被拒
B=${REMOTE_CONTROL_BASE:-http://127.0.0.1:8088}/api/v1
pass=0; fail=0
j() { curl -s --max-time 10 "$@"; }
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
should_fail "列 /data"                 "$B/files?path=/data"
should_fail "列 /system"               "$B/files?path=/system"
should_fail "列 /"                     "$B/files?path=/"
should_fail "列 /data/local/tmp"       "$B/files?path=/data/local/tmp"
should_fail ".. 逃逸"                  "$B/files?path=/sdcard/../../data"
should_fail "相对路径 .. 逃逸"         "$B/files?path=../../data"
should_fail "删 /system/bin"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/system/bin","recursive":true}'
should_fail "mkdir 到 /data"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/data/evil"}'
should_fail "重命名跨出边界"           -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"rename","path":"/sdcard/x","to":"/data/evil"}'

echo
echo "── 软链接逃逸 ──"
# 在 /sdcard 下造一个指向 /data 的软链接，走它必须被拒
VAULT=$(j -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"mkdir","path":"/sdcard/remote-control-linktest"}')
LINK=$(adb ${REMOTE_CONTROL_SERIAL:+-s $REMOTE_CONTROL_SERIAL} shell "ln -s /data /sdcard/remote-control-linktest/escape 2>&1; echo rc=\$?" | tr -d '\r')
if echo "$LINK" | grep -q "rc=0"; then
  should_fail "穿过指向 /data 的软链接" "$B/files?path=/sdcard/remote-control-linktest/escape"
  adb ${REMOTE_CONTROL_SERIAL:+-s $REMOTE_CONTROL_SERIAL} shell "rm -f /sdcard/remote-control-linktest/escape" >/dev/null 2>&1
else
  echo "  （设备上建不了软链接，跳过：$LINK）"
fi
j -X POST "$B/files" -H 'Content-Type: application/json' -d '{"op":"delete","path":"/sdcard/remote-control-linktest","recursive":true}' >/dev/null

echo
echo "  通过 $pass 项，失败 $fail 项"
exit $(( fail > 0 ))
