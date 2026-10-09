"""回显外部内容（页面文字 / 接口返回 / 子进程输出）时的统一写法。

**为什么需要它** —— `run-all.sh` 会数输出里"行首（可带缩进）是 ✗"的行，
用来兜住"脚本判据不完整、退出码可能仍是 0"的情况。

2026-10-09 实测踩到的坑：`upload_test.py` 把页面的多行消息原样打出来，

         │ ✗ 安装失败：安装失败: Exception occurred while executing 'install':
    java.lang.IllegalArgumentException: Error: Failed to parse APK file: …

**前缀只加在了第一行**，第二行从第 0 列开始。于是只要外部内容里某一行以 ✗ 开头，
runner 就会把它当成"脚本判定失败" —— **误报**。

（而且那条消息里的 ✗ 本身是**预期**的：大文件那档喂的是随机字节，安装必然失败。）

所以：**回显外部内容一律走 `echo_block()`**，它给**每一行**都加前缀。
自己的判定行（`  ✗ …`）不要用它 —— 那是给 runner 看的。
"""

import sys

PREFIX = "     │ "


def echo_block(text: str, indent: str = PREFIX) -> None:
    """把外部内容按行加前缀打出来。空内容也会打一行，免得看起来像没输出。"""
    lines = str(text).splitlines() or [""]
    for line in lines:
        print(indent + line)
        sys.stdout.flush()
