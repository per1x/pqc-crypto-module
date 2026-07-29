#!/usr/bin/env python3
"""结构性回归：KDR 的铁律是"只进不出"——公共头里不得存在任何读出根密钥的接口。

这是 那条"PS 侧无读回路径（地址译码上物理不存在）"在软件阶段的对应物：
与其靠人记得别加，不如让 CI 每次都扫一遍。

注释里提到这些名字是允许的（本文件会先剥掉注释），只有**真实声明**才算违规。
"""
import re
import sys
from pathlib import Path

# 真实声明里出现即违规
FORBIDDEN = [
    "pqc_kdr_get(",
    "pqc_kdr_get (",
    "pqc_kdr_read",
    "pqc_kdr_export",
    "pqc_kdr_copy",
    "pqc_kdr_root",
    "get_root",
    "root_out",
]
# 必须存在，否则说明扫错了文件
REQUIRED = ["pqc_kdr_derive"]


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)   # 块注释
    src = re.sub(r"//[^\n]*", " ", src)                # 行注释
    return src


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: check_no_readback.py <header>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.exists():
        print(f"找不到 {path}", file=sys.stderr)
        return 2
    code = strip_comments(path.read_text(encoding="utf-8"))

    bad = [f for f in FORBIDDEN if f in code]
    if bad:
        print(f"✗ {path.name} 出现疑似根密钥读出接口: {bad}", file=sys.stderr)
        print("  KDR 只进不出：外部只能拿派生出来的子密钥。", file=sys.stderr)
        return 1
    missing = [r for r in REQUIRED if r not in code]
    if missing:
        print(f"✗ {path.name} 缺少 {missing} —— 这个检查可能扫错了文件", file=sys.stderr)
        return 1
    print(f"✓ {path.name}: 无根密钥读出接口，派生接口存在")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
