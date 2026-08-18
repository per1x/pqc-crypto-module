#!/usr/bin/env python3
"""结构性检查：shell 脚本里 $VAR 后面不许紧跟多字节字符。

============================================================================
【这条检查为什么存在】
============================================================================
`$VAR` 后面**紧挨着**一个 UTF-8 多字节字符（中文全角括号、顿号、破折号……）时，
某些 shell / locale 组合会把那个字符的首字节（0xEF 之类）当成变量名的合法字节：

    ok "已存到 $TOKEN_FILE（0600）"
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
    bad = '也许 $VAR（括号）\n'.encode()
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


def main():
    if '--self-test' in sys.argv:
        return self_test()
    if self_test():
        return 1

    files = subprocess.check_output(['git', 'ls-files']).decode().split()
    hits = 0
    for f in files:
        if not (f.endswith('.sh') or f.endswith('.py') or '/scripts/' in f):
            continue
        try:
            data = open(f, 'rb').read()
        except OSError:
            continue
        for lineno, name in scan(data):
            print(f"  ✗ {f}:{lineno}  ${name} 后面紧跟多字节字符 → 改成 ${{{name}}}")
            hits += 1

    if hits:
        print(f"\n共 {hits} 处。修法：$VAR → ${{VAR}}")
        return 1
    print("  ✓ 没有 $VAR 紧跟多字节字符的写法")
    return 0


if __name__ == '__main__':
    sys.exit(main())
