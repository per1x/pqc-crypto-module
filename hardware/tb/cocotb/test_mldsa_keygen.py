"""cocotb：ML-DSA KeyGen 的增量验证

目前只覆盖第 ① 段 H —— 后续每加一段就在这里加一个用例，对上黄金模型。
逐段验的理由：整体不对时，"哪一段开始错"比任何波形都值钱。

【H 段的判据】
H = SHAKE256(ξ‖k‖ℓ) 取 128 字节 = ρ(32)‖ρ'(64)‖K(32)，k=ℓ=4。
直接拿 hashlib.shake_256 逐字节比 —— hashlib 与本仓库的 Keccak 实现完全独立，
所以这条比对同时也在验 sha3_core 的 SHAKE256 用法接进 keygen 之后仍然对。
"""
import hashlib

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.xi.value = 0
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

        for _ in range(4000):
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
