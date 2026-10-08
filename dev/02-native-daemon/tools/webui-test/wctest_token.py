"""WebUI 测试的公共部分：把访问令牌喂给页面。

**为什么需要这个** —— 交付的 ROM 默认开启鉴权。页面把令牌存在
`localStorage['remote_control_token']` 里，没令牌时它的每个 API 调用都 401：

    #status: '取运行状态失败：Error: 需要访问令牌'

以前这一套脚本没有令牌支持，于是**大部分检查照样通过**（拖动条范围、
元素存在性这些是纯 UI 的，不需要活服务），只有依赖真实数据的少数几项
才失败 —— 看起来是"9 项里挂 1 项"，其实是**整套都在对着一个 401 的页面跑**。
这种假绿比直接报错更糟，所以令牌要么给上，要么明确报错。

令牌来源（优先级）：
  1. 命令行 `--token=xxx`
  2. 环境变量 `RC_TOKEN`
  3. 从设备上的配置文件读（用 `RC_SERIAL`，默认 emulator-5580；需要 adb root）
"""

import json
import os
import subprocess
import urllib.request

_TOKEN = None


def install_global_auth(token: str) -> None:
    """让本进程里**所有** urllib 请求自动带上令牌。

    为什么用这种"改动全局状态"的做法：这十个脚本里发 HTTP 请求的写法有两种
    （有的直接 `urlopen(BASE + path)`，有的自己包了个 `api()` 建 Request），
    一共十几处。逐处加 header 很容易漏 —— 而漏掉的表现是
    `HTTPError: 401`，看起来像服务坏了。

    只在测试进程里做，且只在真的拿到令牌时装。没令牌就什么都不改，
    让 401 照常暴露出来（那说明该给令牌了）。
    """
    if not token:
        return
    opener = urllib.request.build_opener()
    opener.addheaders = [("Authorization", "Bearer " + token)]
    urllib.request.urlopen = opener.open    # noqa: A001 — 故意的，见上


def resolve_token(explicit: str = "") -> str:
    global _TOKEN
    if explicit:
        return explicit
    if _TOKEN is not None:
        return _TOKEN

    tok = os.environ.get("RC_TOKEN", "")
    if not tok:
        serial = os.environ.get("RC_SERIAL", "emulator-5580")
        adb = os.environ.get("ADB", "adb")
        try:
            out = subprocess.run(
                [adb, "-s", serial, "shell",
                 "grep -E '^token' /data/misc/remote-control/remote-control.conf "
                 "2>/dev/null | head -1 | cut -d= -f2-"],
                capture_output=True, timeout=15).stdout.decode(errors="replace").strip()
            if out:
                tok = out
        except Exception:
            pass
    _TOKEN = tok
    install_global_auth(tok)
    return tok


def token_init_script(token: str) -> str:
    """在任何页面脚本之前把令牌塞进 localStorage。

    用 add_init_script 而不是"先开页面再 evaluate"：后者要跑两次导航，
    而且第一次导航时页面已经拿着空令牌打过一轮 API 了 —— 那一轮的错误
    会留在 #status 里，检查照样会失败。
    """
    if not token:
        return "/* 没拿到令牌：页面会用空令牌请求，若服务开了鉴权就会 401 */"
    return ("try { localStorage.setItem('remote_control_token', %s); } catch (e) {}"
            % json.dumps(token))


def new_page(browser, token: str, viewport=None, **kw):
    """建一个已经带好令牌的页面。"""
    ctx = browser.new_context(viewport=viewport or {"width": 900, "height": 1000}, **kw)
    ctx.add_init_script(token_init_script(token))
    return ctx.new_page()
