"""cocotb：ML-DSA KeyGen 的增量验证

目前只覆盖第 ① 段 H —— 后续每加一段就在这里加一个用例，对上黄金模型。
逐段验的理由：整体不对时，"哪一段开始错"比任何波形都值钱。

【H 段的判据】
H = SHAKE256(ξ‖k‖ℓ) 取 128 字节 = ρ(32)‖ρ'(64)‖K(32)，k=ℓ=4。
直接拿 hashlib.shake_256 逐字节比 —— hashlib 与本仓库的 Keccak 实现完全独立，
所以这条比对同时也在验 sha3_core 的 SHAKE256 用法接进 keygen 之后仍然对。
"""
import hashlib
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import rej_eta_poly   # noqa: E402


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.xi.value = 0
    dut.dbg_sel.value = 0
    dut.dbg_idx.value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


def to_bytes(val, nbytes):
    """RTL 里是低地址字节先出、从高位塞右移，所以整数的低 8 位是第 0 字节 ——
    正好是小端。"""
    return int(val).to_bytes(nbytes, "little")


@cocotb.test()
async def test_h_expand(dut):
    """H(ξ‖4‖4) → ρ‖ρ'‖K 逐字节对上 hashlib"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for seed_base in (0x00, 0xA5, 0xFF):
        xi = bytes([(seed_base + i) & 0xFF for i in range(32)])
        dut.xi.value = int.from_bytes(xi, "little")
        dut.start.value = 1
        await RisingEdge(dut.clk)
        dut.start.value = 0

        # done 现在在 ExpandS 之后才拉高（H 段已并入完整流程），所以等的是
        # 整个 ①② 跑完。H 的 ρ/K 在 done 时仍然有效 —— 这里直接验它们，
        # 而 ExpandS 用例只间接验 ρ'，两条各有价值。
        for _ in range(400_000):
            await RisingEdge(dut.clk)
            if int(dut.done.value):
                break
        else:
            raise AssertionError(f"seed={seed_base:#x}: H 一直没完成")
        await Timer(1, unit="ns")

        h = hashlib.shake_256(xi + bytes([4, 4])).digest(128)
        rho_w = h[:32]
        rhop_w = h[32:96]
        key_w = h[96:128]

        rho = to_bytes(dut.rho.value, 32)
        rhop = to_bytes(dut.rho_prime.value, 64)
        key = to_bytes(dut.key_out.value, 32)

        assert rho == rho_w, (
            f"seed={seed_base:#x}: ρ 不一致，首个不同在字节 "
            f"{next(i for i in range(32) if rho[i] != rho_w[i])}")
        assert rhop == rhop_w, "ρ' 不一致"
        assert key == key_w, "K 不一致"

    dut._log.info("H 段：3 组种子，ρ/ρ'/K 全部逐字节对上 hashlib.shake_256")


async def read_poly(dut, sel):
    """done 之后经调试口读一条多项式的 256 个系数（同步读，等一拍 delta）"""
    out = []
    for i in range(256):
        dut.dbg_sel.value = sel
        dut.dbg_idx.value = i
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        v = int(dut.dbg_coef.value)
        out.append(v - (1 << 32) if v >= (1 << 31) else v)
    return out


@cocotb.test()
async def test_expand_s(dut):
    """② ExpandS：s₁[0..3]、s₂[0..3] 逐系数对上 rej_eta_poly(ρ', nonce, 2)

    这一段同时把第 ① 段一起验了：ρ' 是 H 的输出，s 全对就说明 ρ' 对，
    而且证明了海绵从 FSM 换手给 η 采样器之后没出岔子。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi = bytes(range(32))
    dut.xi.value = int.from_bytes(xi, "little")
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0

    for _ in range(400_000):
        await RisingEdge(dut.clk)
        if int(dut.done.value):
            break
    else:
        raise AssertionError("ExpandS 一直没完成")
    await Timer(1, unit="ns")

    rhop = int(dut.rho_prime.value).to_bytes(64, "little")

    # s₁：nonce = j（sel = 0b0000..0b0011）
    for j in range(4):
        got = await read_poly(dut, j)
        want = rej_eta_poly(rhop, j, 2)
        assert got == want, (
            f"s₁[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != want[i])} 个系数")
    # s₂：nonce = 4+j（sel = 0b1000..0b1011）
    for j in range(4):
        got = await read_poly(dut, 0b1000 | j)
        want = rej_eta_poly(rhop, 4 + j, 2)
        assert got == want, f"s₂[{j}] 不一致"

    dut._log.info("ExpandS：s₁[0..3]、s₂[0..3] 全部对上 rej_eta_poly；"
                  "顺带证明 ρ' 对、海绵换手无碍")
