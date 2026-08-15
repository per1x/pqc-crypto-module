"""cocotb：ML-DSA Verify 的增量验证（ML-DSA-44）

逐段对 oracle（mldsa_oracle.py 的 mldsa_verify，预言机 E），最后整体对 **ACVP sigver**。

【① sigDecode/pkDecode 的判据】
σ = c̃(32) ‖ z(ℓ·576) ‖ hint(ω+k=84)，pk = ρ(32) ‖ t₁(k·320)。
c̃、z 系数、t₁ 系数、hint 位逐一对 oracle 的 polyz_unpack / polyt1_unpack / hint_unpack；
zbad/hbad 两个结构标志对 oracle 的 chknorm / hint_unpack==None。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import (  # noqa: E402
    _load_records, hint_unpack, pk_decode, polyz_unpack)
from mldsa_model import chknorm  # noqa: E402

SIGVER_KAT = Path(__file__).resolve().parents[3] / "vectors" / "mldsa_sigver.kat"

GAMMA1, BETA, OMEGA, K, ELL = 1 << 17, 78, 80, 4, 4
ZBYTES = 256 // 4 * 9        # 576
SIG_H0 = 32 + ELL * ZBYTES   # 2336


async def reset(dut):
    dut.rst_n.value = 0
    for s in ("start", "pk_wr_en", "pk_wr_addr", "pk_wr_data",
              "sig_wr_en", "sig_wr_addr", "sig_wr_data",
              "msg_wr_en", "msg_wr_addr", "msg_wr_data",
              "ctx_wr_en", "ctx_wr_addr", "ctx_wr_data",
              "msg_len", "ctx_len", "dbg_sel", "dbg_idx"):
        getattr(dut, s).value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_buf(dut, we, addr, data, blob):
    for i, b in enumerate(blob):
        we.value = 1
        addr.value = i
        data.value = int(b)
        await RisingEdge(dut.clk)
    we.value = 0
    await RisingEdge(dut.clk)


async def preload(dut, rec):
    pk = bytes.fromhex(rec["pk"])
    sig = bytes.fromhex(rec["sig"])
    msg = bytes.fromhex(rec["msg"])
    ctx = bytes.fromhex(rec.get("context", "") or "")
    await load_buf(dut, dut.pk_wr_en, dut.pk_wr_addr, dut.pk_wr_data, pk)
    await load_buf(dut, dut.sig_wr_en, dut.sig_wr_addr, dut.sig_wr_data, sig)
    if msg:
        await load_buf(dut, dut.msg_wr_en, dut.msg_wr_addr, dut.msg_wr_data, msg)
    if ctx:
        await load_buf(dut, dut.ctx_wr_en, dut.ctx_wr_addr, dut.ctx_wr_data, ctx)
    dut.msg_len.value = len(msg)
    dut.ctx_len.value = len(ctx)
    return pk, sig, msg, ctx


async def run_to_done(dut, limit=4_000_000):
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    for _ in range(limit):
        await RisingEdge(dut.clk)
        if int(dut.done.value):
            break
    else:
        raise AssertionError("verify 一直没完成")
    await Timer(1, unit="ns")


def to_bytes(val, nbytes):
    return int(val).to_bytes(nbytes, "little")


async def read_poly(dut, sel, n=256):
    out = []
    for i in range(n):
        dut.dbg_sel.value = sel
        dut.dbg_idx.value = i
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        v = int(dut.dbg_coef.value)
        out.append(v - (1 << 32) if v >= (1 << 31) else v)
    return out


def d44_records():
    recs = _load_records(SIGVER_KAT)
    if not recs:
        raise AssertionError("找不到 vectors/mldsa_sigver.kat")
    out = [r for r in recs if r["alg"] == "ML-DSA-44"]
    assert out, "KAT 里没有 ML-DSA-44 sigver 记录"
    return out


def oracle_decode(rec):
    """oracle 侧的 sigDecode/pkDecode 结果 + 两个结构标志"""
    sig = bytes.fromhex(rec["sig"])
    pk = bytes.fromhex(rec["pk"])
    ct = sig[:32]
    off = 32
    z = []
    for _ in range(ELL):
        z.append(polyz_unpack(sig[off:off + ZBYTES], GAMMA1))
        off += ZBYTES
    h = hint_unpack(sig[off:off + OMEGA + K], K, OMEGA)
    zbad = any(chknorm(x, GAMMA1 - BETA) for p in z for x in p)
    _, t1 = pk_decode(pk, "ML-DSA-44")
    return dict(ctilde=ct, z=z, t1=t1, h=h, zbad=zbad, hbad=(h is None))


@cocotb.test()
async def test_sigdecode(dut):
    """① sigDecode/pkDecode：c̃/z/t₁/hint 与 zbad/hbad 全对上 oracle

    取三条有代表性的 ACVP 向量：一条应通过（结构合法）、一条 hint 下标非严格递增、
    一条 hint 填充区非零 —— 后两条正是 hbad 必须置起来的场景。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    recs = d44_records()
    picks = []
    for want_h in (False, True):
        for r in recs:
            o = oracle_decode(r)
            if o["hbad"] == want_h:
                picks.append(r)
                break
    # 再补一条 hint 非法但成因不同的（填充非零 vs 下标非递增）
    seen = {id(r) for r in picks}
    for r in recs:
        if id(r) in seen:
            continue
        if oracle_decode(r)["hbad"]:
            picks.append(r)
            break

    for rec in picks:
        await reset(dut)
        await preload(dut, rec)
        await run_to_done(dut)
        o = oracle_decode(rec)

        got_ct = to_bytes(dut.ctilde.value, 32)
        assert got_ct == o["ctilde"], f"tcId={rec.get('tcid')}：c̃ 不一致"

        assert bool(int(dut.zbad.value)) == o["zbad"], (
            f"tcId={rec.get('tcid')}：zbad 不一致 RTL={int(dut.zbad.value)} "
            f"oracle={o['zbad']}")
        assert bool(int(dut.hbad.value)) == o["hbad"], (
            f"tcId={rec.get('tcid')}：hbad 不一致 RTL={int(dut.hbad.value)} "
            f"oracle={o['hbad']}")

        for j in range(ELL):
            got = await read_poly(dut, (0 << 2) | j)
            assert got == o["z"][j], (
                f"tcId={rec.get('tcid')}：z[{j}] 不一致，首个不同在第 "
                f"{next(i for i in range(256) if got[i] != o['z'][j][i])} 个系数")
        for i in range(K):
            got = await read_poly(dut, (1 << 2) | i)
            assert got == o["t1"][i], (
                f"tcId={rec.get('tcid')}：t₁[{i}] 不一致，首个不同在第 "
                f"{next(n for n in range(256) if got[n] != o['t1'][i][n])} 个系数")

        # hint 位：只有结构合法时 oracle 才给出 h（非法时返回 None，位内容无意义）
        if o["h"] is not None:
            for i in range(K):
                got = await read_poly(dut, (2 << 2) | i)
                assert got == o["h"][i], (
                    f"tcId={rec.get('tcid')}：hint[{i}] 不一致，首个不同在第 "
                    f"{next(n for n in range(256) if got[n] != o['h'][i][n])} 位")

        dut._log.info(f"  tcId={rec.get('tcid')} ok（zbad={o['zbad']} hbad={o['hbad']}）")

    dut._log.info("① sigDecode/pkDecode：c̃/z/t₁/hint 与 zbad/hbad 全对上 oracle")
