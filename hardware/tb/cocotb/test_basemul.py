"""cocotb：NTT 域基乘对拍

三方比对 RTL == vectors/rtl/basemul.hex == ref_model.basemul，再加一道
**不同来源**的判据：把结果拿回 Z_q[x]/(x²−ζ) 上按定义展开的多项式乘法去比，
两者应当相差恰好一个 2⁻¹⁶ 因子（Montgomery 域的固有偏移）。
定义式不碰 Montgomery 约减，也不碰位并行技巧，所以它能证伪"公式自洽但写错"。
"""
import random

import cocotb
from cocotb.triggers import Timer

from tbutil import load, s16

from ref_model import Q, ZETAS, basemul  # noqa: E402


def basemul_definition(a0, a1, b0, b1, zeta):
    """按定义展开，ζ 先从 Montgomery 表示换回普通表示"""
    z = zeta * pow(2, -16, Q) % Q
    return (a0 * b0 + a1 * b1 * z) % Q, (a0 * b1 + a1 * b0) % Q


async def apply(dut, a0, a1, b0, b1, zeta):
    dut.a0.value = a0 & 0xFFFF
    dut.a1.value = a1 & 0xFFFF
    dut.b0.value = b0 & 0xFFFF
    dut.b1.value = b1 & 0xFFFF
    dut.zeta.value = zeta & 0xFFFF
    await Timer(1, unit="ns")
    return s16(int(dut.r0.value)), s16(int(dut.r1.value))


@cocotb.test()
async def test_basemul_vectors(dut):
    """1000 条向量三方一致"""
    rows = load("basemul.hex")
    assert len(rows) == 1000, f"向量条数不对：{len(rows)}"
    for i, row in enumerate(rows):
        a0, a1, b0, b1, z, e0, e1 = (s16(int(x, 16)) for x in row)
        got = await apply(dut, a0, a1, b0, b1, z)
        model = basemul(a0, a1, b0, b1, z)
        assert got == (e0, e1) == model, (
            f"第 {i} 条：RTL={got} 向量={(e0, e1)} 模型={model}")
    dut._log.info(f"mlkem_basemul: {len(rows)} 条三方一致")


@cocotb.test()
async def test_basemul_definition(dut):
    """性质断言：与按定义展开的乘法相差恰好一个 2⁻¹⁶ 因子"""
    rng = random.Random(20260729)
    inv_mont = pow(2, -16, Q)
    for _ in range(300):
        a0 = rng.randrange(-Q, Q)
        a1 = rng.randrange(-Q, Q)
        b0 = rng.randrange(-Q, Q)
        b1 = rng.randrange(-Q, Q)
        z = ZETAS[rng.randrange(len(ZETAS))]
        r0, r1 = await apply(dut, a0, a1, b0, b1, z)
        w0, w1 = basemul_definition(a0, a1, b0, b1, z)
        assert (r0 - w0 * inv_mont) % Q == 0, f"r0 与定义式不符：{(a0, a1, b0, b1, z)}"
        assert (r1 - w1 * inv_mont) % Q == 0, f"r1 与定义式不符：{(a0, a1, b0, b1, z)}"
        assert -2 * Q < r0 < 2 * Q and -2 * Q < r1 < 2 * Q, "输出超出 (−2q, 2q)"
    dut._log.info("mlkem_basemul: 与 Z_q[x]/(x²−ζ) 上的定义式一致，输出范围成立")


@cocotb.test()
async def test_basemul_zero(dut):
    """边界：任一乘数为零多项式时结果必须为零"""
    for z in (ZETAS[0], ZETAS[64], -ZETAS[64]):
        assert await apply(dut, 0, 0, 1234, -567, z) == (0, 0)
        assert await apply(dut, 1234, -567, 0, 0, z) == (0, 0)
    dut._log.info("mlkem_basemul: 零多项式边界成立")
