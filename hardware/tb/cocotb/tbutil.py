"""cocotb testbench 的公共辅助：向量文件读取与有符号数转换

黄金向量在仓库根的 vectors/rtl/ 下，由 hardware/model/export_vectors.py 生成
（vectors/ 不入库，属于可复现的派生产物）。
"""
from __future__ import annotations

import sys
from pathlib import Path

# parents[2] = hardware/
HW = Path(__file__).resolve().parents[2]
REPO = HW.parent
VEC = REPO / "vectors" / "rtl"

if str(HW / "model") not in sys.path:
    sys.path.insert(0, str(HW / "model"))


def s16(x: int) -> int:
    """16 位二进制补码 → 有符号整数"""
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def s32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= (1 << 31) else x


def load(name: str) -> list[list[str]]:
    """读向量文件，跳过注释；每行按空白切成字段"""
    path = VEC / name
    if not path.exists():
        raise FileNotFoundError(f"{path} 不存在 —— 先跑 hardware/model/export_vectors.py")
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        rows.append(line.split())
    return rows


def load_pairs(name: str) -> list[tuple[list[str], list[str]]]:
    """一行输入、一行输出交替的向量文件"""
    rows = load(name)
    return [(rows[i], rows[i + 1]) for i in range(0, len(rows) - 1, 2)]
