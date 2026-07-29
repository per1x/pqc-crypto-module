"""cocotb：Compress_d 对拍（解压见 test_decompress.py）

输入域只有 q = 3329 种取值，所以这里是**穷举**而不是抽样：
每一个可能的输入都跑一遍 RTL，与向量文件、ref_model、以及按 FIPS 203 §4.2.1
用有理数算出的定义式四方比对。

乘倒数替代除法的等价性（floor(N/q) == (N·2580335) >> 33）正是靠这一遍穷举钉住的。

参数 D 由 Makefile 的 PARAM_D 经 Icarus 的 -P 传给顶层，测试从环境变量读同一个值。
"""
import math
import os
from fractions import Fraction

import cocotb
from cocotb.triggers import Timer

from tbutil import load

from ref_model import Q, compress  # noqa: E402

D = int(os.environ.get("PARAM_D", "10"))


def compress_definition(x: int, d: int) -> int:
    return math.floor(Fraction((1 << d) * (x % Q), Q) + Fraction(1, 2)) % (1 << d)


def vectors(kind: str) -> dict[int, int]:
    """从 compress.hex 里挑出本次 D 的那一组"""
    tag = "00" if kind == "compress" else "01"
    out = {}
    for c, d, vin, vout in load("compress.hex"):
        if c == tag and int(d, 16) == D:
            out[int(vin, 16)] = int(vout, 16)
    return out


@cocotb.test()
async def test_compress_exhaustive(dut):
    """全部 q 个输入逐值四方比对"""
    exp = vectors("compress")
    assert len(exp) == Q, f"向量里 D={D} 的压缩条数不对：{len(exp)}"
    for x in range(Q):
        dut.coeff.value = x
        await Timer(1, unit="ns")
        got = int(dut.val.value)
        assert got == exp[x] == compress(x, D) == compress_definition(x, D), (
            f"D={D} x={x}：RTL={got} 向量={exp[x]} "
            f"模型={compress(x, D)} 定义式={compress_definition(x, D)}")
    dut._log.info(f"mlkem_compress D={D}: 全部 {Q} 个输入四方一致（穷举）")


@cocotb.test()
async def test_compress_signed_input(dut):
    """负系数：数据通路里系数用 (−q, q) 的有符号表示，压缩前要折回 [0, q)"""
    for x in range(1, Q):
        dut.coeff.value = (-x) & 0xFFFF
        await Timer(1, unit="ns")
        got = int(dut.val.value)
        want = compress(Q - x, D)
        assert got == want, f"D={D} 输入 −{x}：RTL={got} 期望={want}"
    dut._log.info(f"mlkem_compress D={D}: 负系数折回 [0, q) 后结果一致")


@cocotb.test()
async def test_compress_range(dut):
    """输出必须落在 [0, 2^D)，且单调不减（压缩是非降的舍入映射）"""
    prev = -1
    for x in range(Q):
        dut.coeff.value = x
        await Timer(1, unit="ns")
        got = int(dut.val.value)
        assert 0 <= got < (1 << D), f"D={D} x={x}：输出 {got} 越界"
        # 只有末尾回绕那一步允许下降（mod 2^D）
        if got < prev:
            assert x > Q - Q // (1 << D) - 2, f"D={D} x={x}：非末尾处出现下降"
        prev = got
    dut._log.info(f"mlkem_compress D={D}: 输出范围与单调性成立")
