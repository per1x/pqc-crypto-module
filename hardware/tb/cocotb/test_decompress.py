"""cocotb：Decompress_d 对拍（压缩见 test_compress.py）

输入域只有 2^D 种取值，全部跑一遍，与向量文件、ref_model、以及按 FIPS 203
§4.2.1 用有理数算出的定义式四方比对。

参数 D 由 Makefile 的 PARAM_D 经 Icarus 的 -P 传给顶层，测试从环境变量读同一个值。
"""
import math
import os
from fractions import Fraction

import cocotb
from cocotb.triggers import Timer

from tbutil import load, s16

from ref_model import Q, compress, decompress  # noqa: E402

D = int(os.environ.get("PARAM_D", "10"))


def decompress_definition(y: int, d: int) -> int:
    return math.floor(Fraction(Q * y, 1 << d) + Fraction(1, 2))


def vectors() -> dict[int, int]:
    return {int(vin, 16): int(vout, 16)
            for c, d, vin, vout in load("compress.hex")
            if c == "01" and int(d, 16) == D}


@cocotb.test()
async def test_decompress_exhaustive(dut):
    """全部 2^D 个输入逐值四方比对"""
    exp = vectors()
    assert len(exp) == (1 << D), f"向量里 D={D} 的解压条数不对：{len(exp)}"
    for y in range(1 << D):
        dut.val.value = y
        await Timer(1, unit="ns")
        got = s16(int(dut.coeff.value))
        assert got == exp[y] == decompress(y, D) == decompress_definition(y, D), (
            f"D={D} y={y}：RTL={got} 向量={exp[y]} "
            f"模型={decompress(y, D)} 定义式={decompress_definition(y, D)}")
        assert 0 <= got < Q, f"D={D} y={y}：解压结果 {got} 不在 [0, q)"
    dut._log.info(f"mlkem_decompress D={D}: 全部 {1 << D} 个输入四方一致（穷举）")


@cocotb.test()
async def test_decompress_error_bound(dut):
    """性质断言：Decompress(Compress(x)) 与 x 的距离不超过 round(q/2^(D+1))

    这是 FIPS 203 给压缩误差的理论界。它同时约束压缩与解压两侧的舍入方向 ——
    任一侧把"半值向上"写成"向下"或"截断"，这条就不成立。
    """
    bound = math.floor(Fraction(Q, 1 << (D + 1)) + Fraction(1, 2))
    worst = 0
    for x in range(Q):
        dut.val.value = compress(x, D)
        await Timer(1, unit="ns")
        e = (s16(int(dut.coeff.value)) - x) % Q
        if e > Q // 2:
            e -= Q
        worst = max(worst, abs(e))
        assert abs(e) <= bound, f"D={D} x={x}：误差 {e} 超出理论界 {bound}"
    dut._log.info(f"mlkem_decompress D={D}: 压缩往返最大误差 {worst}，理论界 {bound}")
