"""cocotb：ML-DSA 的两个多项式采样器（RejNTTPoly / RejBoundedPoly）

============================================================================
【为什么这一层非测不可】
============================================================================
底下的拒绝判据（mldsa_rej_uniform / mldsa_rej_eta）是纯组合的，已经被
test_mldsa_units 穷举过。这里测的是**外面那一圈**，而那一圈才是容易错的：

  · 头部字节序（ρ‖nonce，nonce 小端两字节）—— 写反了采出来的多项式
    完全合法、分布也对，**只是和标准的不一样**，自洽性测试永远发现不了；
  · 被拒的候选不占位置，所以要抽多少字节事先不知道；
  · η 采样一个字节出两个候选，第二个可能因为已经攒够 256 个而必须丢掉 ——
    不丢的话会写到下标 256（回绕成 0）把第一个系数覆盖掉，
    **而多项式长度看着仍然是对的**。

判据一律是与 mldsa_oracle.py 的 rej_uniform_poly / rej_eta_poly 逐系数比。
那两个函数用的是 hashlib 的 SHAKE，与本仓库的 Keccak 实现完全独立 ——
所以这条比对同时也在验 sha3_core 的 SHAKE128/256 两种用法。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import rej_uniform_poly, rej_eta_poly   # noqa: E402

N = 256


async def reset(dut):
    dut.rst_n.value = 0
    dut.u_start.value = 0
    dut.e_start.value = 0
    dut.u_seed.value = 0
    dut.e_seed.value = 0
    dut.u_nonce.value = 0
    dut.e_nonce.value = 0
    dut.u_rd_addr.value = 0
    dut.e_rd_addr.value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


def le_int(b: bytes) -> int:
    """字节串 → 整数，b[0] 落在最低 8 位（与 RTL 里 seed[i*8 +: 8] 一致）"""
    return int.from_bytes(b, "little")


async def read_poly(dut, addr_sig, data_sig, signed=False):
    out = []
    for i in range(N):
        addr_sig.value = i
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")          # 同步读，等一拍 delta
        v = int(data_sig.value)
        if signed:
            v = v - (1 << 32) if v >= (1 << 31) else v
        out.append(v)
    return out


@cocotb.test()
async def test_poly_uniform(dut):
    """RejNTTPoly：256 个系数逐个对上黄金模型"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for seed_byte, nonce in ((0x00, 0x0000), (0xA5, 0x0102), (0xFF, 0x0403)):
        seed = bytes([(seed_byte + i) & 0xFF for i in range(32)])
        dut.u_seed.value = le_int(seed)
        dut.u_nonce.value = nonce
        dut.u_start.value = 1
        await RisingEdge(dut.clk)
        dut.u_start.value = 0

        for _ in range(400_000):
            await RisingEdge(dut.clk)
            if int(dut.u_done.value):
                break
        else:
            raise AssertionError(f"nonce=0x{nonce:04x}：采样一直没结束")

        assert int(dut.u_count.value) == N, \
            f"只采到 {int(dut.u_count.value)} 个系数"
        got = await read_poly(dut, dut.u_rd_addr, dut.u_rd_data)
        want = rej_uniform_poly(seed, nonce)
        assert got == want, (
            f"nonce=0x{nonce:04x} 与黄金模型不一致，"
            f"首个不同在第 {next(i for i in range(N) if got[i] != want[i])} 个系数")

    dut._log.info("RejNTTPoly：3 组种子/nonce，每条 256 个系数全部对上黄金模型")


@cocotb.test()
async def test_poly_eta(dut):
    """RejBoundedPoly（η=2）：一个字节两个候选，最后一个不能溢出"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for seed_byte, nonce in ((0x00, 0x0000), (0x3C, 0x0007), (0x91, 0x00FF)):
        seed = bytes([(seed_byte + i) & 0xFF for i in range(64)])
        dut.e_seed.value = le_int(seed)
        dut.e_nonce.value = nonce
        dut.e_start.value = 1
        await RisingEdge(dut.clk)
        dut.e_start.value = 0

        for _ in range(400_000):
            await RisingEdge(dut.clk)
            if int(dut.e_done.value):
                break
        else:
            raise AssertionError(f"nonce=0x{nonce:04x}：采样一直没结束")

        assert int(dut.e_count.value) == N, \
            f"只采到 {int(dut.e_count.value)} 个系数"
        got = await read_poly(dut, dut.e_rd_addr, dut.e_rd_data, signed=True)
        want = rej_eta_poly(seed, nonce, 2)
        assert got == want, (
            f"nonce=0x{nonce:04x} 与黄金模型不一致，"
            f"首个不同在第 {next(i for i in range(N) if got[i] != want[i])} 个系数")
        # 单独查一遍取值域：万一比对用的黄金模型自己错了，这一条还能兜住
        assert all(-2 <= v <= 2 for v in got), "有系数落在 [−η, η] 之外"

    dut._log.info("RejBoundedPoly η=2：3 组种子/nonce 全部对上黄金模型，取值域也对")
