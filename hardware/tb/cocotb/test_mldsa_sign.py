"""cocotb：ML-DSA Sign 的增量验证（ML-DSA-44，确定性优先）

目前只覆盖第 ① 段 skDecode —— 后续每加一段就在这里加一个用例，对上黄金模型
（mldsa_oracle.py 的 mldsa_sign，预言机 D）。逐段验的理由：整体不对时，
"哪一段开始错" 比任何波形都值钱。

【① skDecode 的判据】
sk = ρ(32)‖K(32)‖tr(64)‖s₁(ℓ·96)‖s₂(k·96)‖t₀(k·416)。用 ACVP siggen 的 sk
直接喂进 RTL，读出的 ρ/K/tr（端口）与 s₁/s₂/t₀ 系数（dbg 口）逐一对上 oracle
的 sk_decode(sk)。s₁/s₂ 每系数 3 位、逆变换 η−v；t₀ 每系数 13 位、逆变换 2^(D−1)−v。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import _load_records, sk_decode   # noqa: E402

SIGGEN_KAT = Path(__file__).resolve().parents[3] / "vectors" / "mldsa_siggen.kat"


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.sk_wr_en.value = 0
    dut.sk_wr_addr.value = 0
    dut.sk_wr_data.value = 0
    dut.msg_wr_en.value = 0
    dut.msg_wr_addr.value = 0
    dut.msg_wr_data.value = 0
    dut.ctx_wr_en.value = 0
    dut.ctx_wr_addr.value = 0
    dut.ctx_wr_data.value = 0
    dut.msg_len.value = 0
    dut.ctx_len.value = 0
    dut.rnd.value = 0
    dut.dbg_sel.value = 0
    dut.dbg_idx.value = 0
    dut.sig_addr.value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_buf(dut, we, addr, data, blob):
    """把一段字节预载进某个输入缓冲（每拍写一个字节）"""
    for i, b in enumerate(blob):
        we.value = 1
        addr.value = i
        data.value = int(b)
        await RisingEdge(dut.clk)
    we.value = 0
    await RisingEdge(dut.clk)


def to_bytes(val, nbytes):
    return int(val).to_bytes(nbytes, "little")


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


def first_d44(deterministic=True):
    recs = _load_records(SIGGEN_KAT)
    if not recs:
        raise AssertionError("找不到 vectors/mldsa_siggen.kat")
    for r in recs:
        if r["alg"] != "ML-DSA-44":
            continue
        if deterministic and r.get("deterministic", "") != "1":
            continue
        return r
    raise AssertionError("KAT 里没有合适的 ML-DSA-44 记录")


@cocotb.test()
async def test_sk_decode(dut):
    """① skDecode：ρ/K/tr + s₁/s₂/t₀ 逐一对上 oracle 的 sk_decode（用 ACVP sk）"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    sk = bytes.fromhex(rec["sk"])
    assert len(sk) == 2560

    await load_buf(dut, dut.sk_wr_en, dut.sk_wr_addr, dut.sk_wr_data, sk)

    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0

    for _ in range(200_000):
        await RisingEdge(dut.clk)
        if int(dut.done.value):
            break
    else:
        raise AssertionError("skDecode 一直没完成")
    await Timer(1, unit="ns")

    rho_w, key_w, tr_w, s1_w, s2_w, t0_w = sk_decode(sk, "ML-DSA-44")

    assert to_bytes(dut.rho.value, 32) == rho_w, "ρ 不一致"
    assert to_bytes(dut.key_out.value, 32) == key_w, "K 不一致"
    assert to_bytes(dut.tr_out.value, 64) == tr_w, "tr 不一致"

    for j in range(4):
        got = await read_poly(dut, 0b00000 | j)      # s₁[j]
        assert got == s1_w[j], (
            f"s₁[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != s1_w[j][i])} 个系数")
    for j in range(4):
        got = await read_poly(dut, 0b01000 | j)      # s₂[j]  (dbg_sel[3:2]=10)
        assert got == s2_w[j], f"s₂[{j}] 不一致"
    for j in range(4):
        got = await read_poly(dut, 0b01100 | j)      # t₀[j]  (dbg_sel[3:2]=11)
        assert got == t0_w[j], (
            f"t₀[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != t0_w[j][i])} 个系数")

    dut._log.info("① skDecode：ρ/K/tr + s₁/s₂/t₀ 全部对上 oracle 的 sk_decode")
