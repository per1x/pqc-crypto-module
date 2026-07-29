"""cocotb：256 点 NTT 核对拍 —— L2 层（路线图 §5.1.3 的 ntt_vectors）

三方比对：RTL == vectors/rtl/ntt.hex == model/ref_model.py 现算。
同时记录 cycle 数 —— 这就是无板阶段能拿到的性能数据（§5.3.3 那段注释）。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "model"))

from ref_model import invntt, ntt  # noqa: E402

VEC = ROOT / "vectors" / "rtl"


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def load_pairs(name: str):
    """文件里一行输入、一行输出，交替"""
    path = VEC / name
    if not path.exists():
        raise FileNotFoundError(f"{path} 不存在 —— 先跑 model/export_vectors.py")
    rows = [l.split() for l in path.read_text().splitlines()
            if l.strip() and not l.startswith("#")]
    return [(rows[i], rows[i + 1]) for i in range(0, len(rows) - 1, 2)]


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.wr_en.value = 0
    dut.inverse.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_poly(dut, coeffs):
    for i, c in enumerate(coeffs):
        dut.wr_en.value = 1
        dut.wr_addr.value = i
        dut.wr_data.value = c & 0xFFFF
        await RisingEdge(dut.clk)
    dut.wr_en.value = 0
    await RisingEdge(dut.clk)


async def run_transform(dut, inverse: int) -> int:
    dut.inverse.value = inverse
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    cycles = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        cycles += 1
        assert cycles < 20000, "NTT 超时，检查状态机"
    return cycles


async def read_poly(dut):
    out = []
    for i in range(256):
        dut.rd_addr.value = i
        await Timer(1, unit="ns")
        out.append(s16(int(dut.rd_data.value)))
    return out


@cocotb.test()
async def test_ntt_forward(dut):
    """正变换：与向量文件和 Python 模型三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("ntt.hex")
    assert len(pairs) == 20
    total_cycles = 0
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s16(int(x, 16)) for x in vin]
        expect = [s16(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        total_cycles += await run_transform(dut, 0)
        got = await read_poly(dut)
        model = ntt(list(coeffs))
        assert got == expect == model, (
            f"第 {idx} 组不匹配\n  RTL   前8={got[:8]}\n  向量  前8={expect[:8]}\n"
            f"  模型  前8={model[:8]}")
    dut._log.info(f"ntt 正变换 {len(pairs)} 组三方一致，平均 {total_cycles // len(pairs)} cycles/次")


@cocotb.test()
async def test_ntt_inverse(dut):
    """逆变换：同样三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("invntt.hex")
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s16(int(x, 16)) for x in vin]
        expect = [s16(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        await run_transform(dut, 1)
        got = await read_poly(dut)
        model = invntt(list(coeffs))
        assert got == expect == model, f"第 {idx} 组逆变换不匹配"
    dut._log.info(f"ntt 逆变换 {len(pairs)} 组三方一致")


@cocotb.test()
async def test_roundtrip(dut):
    """性质断言：invntt(ntt(x)) ≡ x·2^16 (mod q)，**不是恒等**"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    Q, MONT = 3329, 2285
    src = [((i * 37) % Q) - Q // 2 for i in range(256)]
    await load_poly(dut, src)
    c1 = await run_transform(dut, 0)
    fwd = await read_poly(dut)
    await load_poly(dut, fwd)
    c2 = await run_transform(dut, 1)
    back = await read_poly(dut)
    for a, b in zip(src, back):
        assert (a * MONT - b) % Q == 0, f"往返不符 x·mont：{a} -> {b}"
    assert fwd != src, "正变换没改变系数？"
    dut._log.info(f"往返性质成立；正变换 {c1} cycles，逆变换 {c2} cycles")
