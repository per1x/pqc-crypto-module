"""cocotb testbench：L0 算子与 L1 蝶形对拍

三方比对，这是 的要求：
    RTL 输出  ==  向量文件里的期望值  ==  Python 参考模型现算的值

只比对向量文件是不够的 —— 向量文件本身可能是错的；只比对现算模型也不够 ——
那样测试和模型会一起漂移。三边一致才说明 RTL 是对的。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.triggers import Timer

# parents[2] = hardware/；黄金向量在仓库根的 vectors/（C 侧测试也读它）
HW = Path(__file__).resolve().parents[2]
REPO = HW.parent
sys.path.insert(0, str(HW / "model"))

from ref_model import (  # noqa: E402
    barrett_reduce, ct_butterfly, gs_butterfly, montgomery_reduce,
)

VEC = REPO / "vectors" / "rtl"


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def s32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= (1 << 31) else x


def load(name: str):
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


@cocotb.test()
async def test_mont_reduce(dut):
    """Montgomery 约减：1000 条向量三方比对"""
    rows = load("mont_reduce.hex")
    assert len(rows) == 1000, f"向量条数不对：{len(rows)}"
    for i, (a_hex, exp_hex) in enumerate(rows):
        a = s32(int(a_hex, 16))
        exp = s16(int(exp_hex, 16))
        dut.a.value = a & 0xFFFFFFFF
        await Timer(1, unit="ns")
        got = s16(int(dut.t_out.value))
        model = montgomery_reduce(a)
        assert got == exp == model, (
            f"第 {i} 条：RTL={got} 向量={exp} 模型={model}（输入 {a}）")
    dut._log.info(f"mont_reduce: {len(rows)} 条三方一致")


@cocotb.test()
async def test_mont_definition(dut):
    """性质断言：out·2^16 ≡ in (mod q)。比逐条比对更能防住'表和实现一起错'"""
    Q = 3329
    for a in (0, 1, -1, 1234, -1234, Q * 100, -Q * 100, Q * (1 << 14)):
        dut.a.value = a & 0xFFFFFFFF
        await Timer(1, unit="ns")
        got = s16(int(dut.t_out.value))
        assert (got * (1 << 16) - a) % Q == 0, f"定义式不成立：a={a} out={got}"
        assert -Q < got < Q, f"输出超出 (-q, q)：{got}"
    dut._log.info("mont_reduce: 定义式与输出范围均成立")
