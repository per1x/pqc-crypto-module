"""cocotb：ML-DSA 的三种系数打包（FIPS 204 §7.1）

============================================================================
【判据：跟黄金模型的字节比，不跟自己的解包比】
============================================================================
打包最容易出的一类错是**变换漏了或方向反了**（t₀ 要先算 2^(D−1) − a，
s₁/s₂ 要先算 η − a）。这类错有个讨厌的性质：**打包→解包仍然能对上**，
因为两次错误互相抵消。所以自洽性测试对它完全无效。

这里一律拿 hardware/model/mldsa_oracle.py 里的 polyt1_pack / polyt0_pack /
polyeta_pack 出来的字节逐字节比 —— 那三个函数是对着 ACVP 的 pk/sk 验过的
（见 oracle_c），所以这条链一直连到标准向量。

【为什么还要专门测边界值】
随机系数几乎不会取到区间端点，而端点正是变换出错时最先崩的地方
（t₀ 的 −2^(D−1)+1 与 +2^(D−1)、s 的 ±η）。所以除了随机向量，
每种打包都单独喂一遍端点。
"""
import random
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import polyt1_pack, polyt0_pack, polyeta_pack   # noqa: E402

N = 256


async def reset(dut):
    dut.rst_n.value = 0
    dut.clr.value = 0
    dut.in_valid.value = 0
    dut.coef.value = 0
    # polyeta 顶层多一个运行时 η 口；另外两个顶层没有，所以要探一下再驱动
    if hasattr(dut, "eta"):
        dut.eta.value = 2
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def pack_poly(dut, coeffs, signed=False):
    """把一条多项式喂进去，收集吐出来的字节

    喂的时候必须看 in_ready —— 打包器在累加器攒够 8 位时会拉低它。
    不看的话会丢系数，而丢掉的是中间某一个，输出长度却仍然可能对，
    最难查的那种。
    """
    out = bytearray()
    dut.clr.value = 1
    await RisingEdge(dut.clk)
    dut.clr.value = 0
    await RisingEdge(dut.clk)

    i = 0
    idle = 0
    while i < len(coeffs) or idle < 40:
        if i < len(coeffs) and int(dut.in_ready.value):
            v = coeffs[i]
            dut.coef.value = (v & ((1 << 13) - 1)) if signed else v
            dut.in_valid.value = 1
            i += 1
            idle = 0
        else:
            dut.in_valid.value = 0
            idle += 1
        await RisingEdge(dut.clk)
        # ⚠️ 时钟沿之后要等一拍 delta 再读寄存器输出，否则读到的是这次沿
        # **之前**的值。踩过：不等的话收到的字节正好少一半 —— 而长度错了
        # 反而是幸运的，如果只是偶尔漏一个，比对时会指向"某个中间字节不对"，
        # 完全指错方向。仓库里其他用例也是这个写法。
        await Timer(1, unit="ns")
        if int(dut.out_valid.value):
            out.append(int(dut.out_byte.value) & 0xFF)
    dut.in_valid.value = 0
    return bytes(out)


@cocotb.test()
async def test_polyt1_pack(dut):
    """t₁：每系数 10 位，256 个系数 → 320 字节"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    random.seed(0x1D5A)
    for trial in range(3):
        p = [random.randrange(0, 1 << 10) for _ in range(N)]
        got = await pack_poly(dut, p)
        want = polyt1_pack(p)
        assert len(got) == 320, f"第 {trial} 条：出了 {len(got)} 字节，应当是 320"
        assert got == want, (
            f"第 {trial} 条与黄金模型不一致，首个不同在字节 "
            f"{next(i for i in range(320) if got[i] != want[i])}")

    # 端点：全 0 与全 1023
    for p in ([0] * N, [(1 << 10) - 1] * N):
        got = await pack_poly(dut, p)
        assert got == polyt1_pack(p), "端点值打包不对"

    dut._log.info("t₁ 打包：3 条随机 + 2 条端点，逐字节对上黄金模型")


@cocotb.test()
async def test_polyt0_pack(dut):
    """t₀：每系数 13 位，先算 2^(D−1) − a；256 个系数 → 416 字节"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    lo, hi = -(1 << 12) + 1, 1 << 12
    random.seed(0x0D5A)
    for trial in range(3):
        p = [random.randrange(lo, hi + 1) for _ in range(N)]
        got = await pack_poly(dut, p, signed=True)
        want = polyt0_pack(p)
        assert len(got) == 416, f"第 {trial} 条：出了 {len(got)} 字节，应当是 416"
        assert got == want, f"第 {trial} 条与黄金模型不一致"

    # 端点：区间两端与 0。变换写反或漏掉时，这三种最先崩。
    for p in ([lo] * N, [hi] * N, [0] * N):
        got = await pack_poly(dut, p, signed=True)
        assert got == polyt0_pack(p), f"端点 {p[0]} 打包不对"

    dut._log.info("t₀ 打包：含 2^(D−1)−a 变换，随机与端点都对上黄金模型")


@cocotb.test()
async def test_polyeta_pack(dut):
    """s₁/s₂：η−a 变换，位宽随 η（2→3 位，4→4 位）

    ⚠️ η 从**编译期参数**改成了**运行时端口**（运行时选 44/65/87 的前提：
    s₁/s₂ 的位宽随 η 变）。所以这条用例也跟着改成
    **同一次仿真里先 η=2 再 η=4**，两种都对上黄金模型 ——
    这正是"运行时可选"在叶子模块这一层的证据：
    分两次编译各跑一个 η 是证明不了运行时可切的。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for eta in (2, 4):
        dut.eta.value = eta
        await RisingEdge(dut.clk)
        nbytes = 96 if eta == 2 else 128
        random.seed(0xE7A + eta)
        for trial in range(3):
            p = [random.randrange(-eta, eta + 1) for _ in range(N)]
            got = await pack_poly(dut, p, signed=True)
            want = polyeta_pack(p, eta)
            assert len(got) == nbytes, \
                f"η={eta} 第 {trial} 条：出了 {len(got)} 字节，应当是 {nbytes}"
            assert got == want, f"η={eta} 第 {trial} 条与黄金模型不一致"

        for p in ([-eta] * N, [eta] * N, [0] * N):
            got = await pack_poly(dut, p, signed=True)
            assert got == polyeta_pack(p, eta), f"η={eta} 端点 {p[0]} 打包不对"

        dut._log.info(f"η={eta} 打包：{nbytes} 字节，随机与端点都对上黄金模型")

    dut._log.info("polyeta：同一次仿真里 η=2 与 η=4 都对上 —— 位宽确实是运行时可切的")
