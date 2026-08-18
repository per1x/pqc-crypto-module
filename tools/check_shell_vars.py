#!/usr/bin/env python3
"""结构性检查：shell 脚本里 $VAR 后面不许紧跟多字节字符。

============================================================================
【这条检查为什么存在】
============================================================================
`$VAR` 后面**紧挨着**一个 UTF-8 多字节字符（中文全角括号、顿号、破折号……）时，
某些 shell / locale 组合会把那个字符的首字节（0xEF 之类）当成变量名的合法字节：

    ok "已存到 $TOKEN_FILE（0600）"          <- 反例示例
                 ^^^^^^^^^^^ ^
                 变量名被吃成 TOKEN_FILE\\xef

配上 `set -u` 就是当场退出：`TOKEN_FILE<0xef>: unbound variable`。

⚠️ **它是环境相关的** —— 同一个脚本在一台机器上跑得好好的，换台机器就炸。
所以"我这边测过了"不构成证据，这也正是它值得做成自动检查的理由。

这个坑在本仓库**已经踩过两次**：
  · 2026-08-17 commit 5605bc9 —— demo_remote.sh 的 --fetch-token 路径，
    用户在自己机器上报的错，全仓库扫出 29 处；
  · 2026-08-18 —— demo_remote.sh 重写时又引入 2 处，同样是用户先撞上。

两次都是"下次记得写 ${}"这个办法没兜住。所以改成结构性检查，由 ctest 驱动。

修法永远是一样的：`$VAR` → `${VAR}`。

用法：
    python3 tools/check_shell_vars.py [--self-test]
"""
import re
import subprocess
import sys
from pathlib import Path

# $NAME 后面紧跟一个 >= 0x80 的字节（即 UTF-8 多字节序列的首字节）
PAT = re.compile(rb'\$([A-Za-z_][A-Za-z0-9_]*)(?=[\x80-\xff])')


def scan(data):
    """返回 [(行号, 变量名)]"""
    out = []
    for lineno, line in enumerate(data.split(b'\n'), 1):
        for m in PAT.finditer(line):
            out.append((lineno, m.group(1).decode('ascii')))
    return out


def self_test():
    """先在合成样本上验一遍。
    一个什么都找不到的扫描器，在报告"干净"时什么也证明不了。"""
    bad = '也许 $VAR（括号）\n'.encode()   # 反例示例
    good = '也许 ${VAR}（括号）\n'.encode()
    ascii_ok = 'echo $VAR (paren)\n'.encode()
    fails = 0
    if len(scan(bad)) != 1:
        print("  ✗ 自测：该抓的没抓到")
        fails += 1
    if scan(good):
        print("  ✗ 自测：${VAR} 被误报")
        fails += 1
    if scan(ascii_ok):
        print("  ✗ 自测：ASCII 上下文被误报")
        fails += 1
    if fails:
        return 1
    print("  ✓ 自测通过（能抓、不误报）")
    return 0


# 扫描范围的下界。低于它就说明**根本没扫到东西**，而不是"仓库很干净"。
# 这一条是补上来的：`git ls-files` 取的是**当前工作目录**下的文件，而 ctest
# 是在 build/ 里跑的 —— 于是列表恒为空，这条检查一直在"通过"，却一个文件都
# 没看过。自测那行 ✓ 让它看起来是活的，其实只证明了扫描器本身能用。
#
# 所以现在两件事一起做：路径锚到仓库根（而不是 cwd），并且把"扫到的文件太少"
# 当成失败。一个什么都没扫的检查，报告"干净"时什么也没证明。
MIN_FILES = 40

ROOT = Path(__file__).resolve().parent.parent


def targets(argv):
    """要扫哪些文件。给了参数就只扫参数，否则扫仓库里所有 shell/python 脚本。"""
    named = [a for a in argv[1:] if not a.startswith('-')]
    if named:
        return [Path(a) for a in named]
    out = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files']).decode()
    return [ROOT / f for f in out.split()
            if f.endswith('.sh') or f.endswith('.py') or '/scripts/' in f]


# 反例本身要写得出来，否则这份文件解释不了自己在查什么。所以留一个显式豁免：
# 同一行里写上 EXEMPT 那个词就跳过。**必须是显式的、逐行的** —— 整个文件加白
# 名单才是真正危险的做法：那样这个文件里以后新写的真 bug 也一起被放过了。
EXEMPT = '反例示例'


def main():
    if '--self-test' in sys.argv:
        return self_test()
    if self_test():
        return 1

    files = targets(sys.argv)
    scanned = 0
    hits = 0
    for f in files:
        try:
            data = f.read_bytes()
        except OSError:
            continue
        scanned += 1
        try:
            shown = f.relative_to(ROOT)
        except ValueError:
            shown = f
        lines = data.split(b'\n')
        for lineno, name in scan(data):
            if EXEMPT.encode() in lines[lineno - 1]:
                continue
            print(f"  ✗ {shown}:{lineno}  ${name} 后面紧跟多字节字符 → 改成 ${{{name}}}")
            hits += 1

    named = [a for a in sys.argv[1:] if not a.startswith('-')]
    if not named and scanned < MIN_FILES:
        print(f"  ✗ 只扫到 {scanned} 个文件（至少该有 {MIN_FILES} 个）——"
              f" 扫描范围坏了，不是仓库干净")
        print(f"    仓库根当作了 {ROOT}")
        return 1

    if hits:
        print(f"\n共 {hits} 处。修法：$VAR → ${{VAR}}")
        return 1
    print(f"  ✓ 没有 $VAR 紧跟多字节字符的写法（扫了 {scanned} 个文件）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
