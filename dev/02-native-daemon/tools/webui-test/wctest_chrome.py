"""这套 WebUI 测试共用的 Chromium 位置。

**为什么要有这个文件** —— 2026-10-09 实测，这个路径原来在 **10 个脚本里各写一份**，
而且有两种写法混着：

    # 4 个脚本：可覆盖（对的做法）
    CHROME = os.environ.get("CHROME", "/root/.cache/ms-playwright/chromium-1243/...")

    # 7 个脚本：写死字面量
    p.chromium.launch(headless=True,
        executable_path="/root/.cache/ms-playwright/chromium-1243/...")

两种写法混在一起，正是"同一句话抄到 N 处、只改了一处"的形状：
playwright 换了 Chromium 版本之后，改 4 个、忘 7 个，表现是**一部分脚本能跑、
一部分起不来** —— 而这比"全都起不来"更难查，因为它看起来像已经修好了。

所以：**默认值只有这一份**，别的文件一律 `from wctest_chrome import CHROME`。

覆盖方式：环境变量 `CHROME`（这一条原来就有，保留了）。

⚠️ 路径不存在时**在这里就报错**，而不是让 playwright 抛一句看不懂的
"Executable doesn't exist"。前提没立住就该当场说清楚。
"""

import os
import sys

_DEFAULT = "/root/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome"

def _resolve() -> str:
    """确认它真在，不在就明确报错并说清怎么解决。

    ⚠️ **在导入时调用**（见下面那行）。第一版只定义了这个函数却没人在任何地方
    调用它 —— 于是文件开头的说明"路径不存在时在这里就报错"**是句空话**：
    实际还是会抛 playwright 那句 "Executable doesn't exist"。
    注释承诺了行为、代码没实现，正是这个项目反复在修的那类问题。
    改成导入即校验之后，10 个脚本自动都拿到这个行为。
    """
    path = os.environ.get("CHROME", _DEFAULT)
    if os.path.isfile(path):
        return path
    sys.stderr.write(
        "[x] 找不到 Chromium：%s\n"
        "    这套测试需要一个 chromium。解法二选一：\n"
        "      · 装一个：python3 -m playwright install chromium\n"
        "      · 或指定现成的：CHROME=/path/to/chrome ./run-all.sh …\n"
        % path)
    raise SystemExit(2)


CHROME = _resolve()      # 导入即校验：前提没立住就别往下走
