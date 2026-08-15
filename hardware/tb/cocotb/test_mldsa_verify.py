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
    _load_records, hint_unpack, pk_decode, polyz_unpack, SIG_PARAMS)
from mldsa_model import chknorm, PARAMS  # noqa: E402

SIGVER_KAT = Path(__file__).resolve().parents[3] / "vectors" / "mldsa_sigver.kat"

# 参数集由 Makefile 的 MLDSA_ALG 环境变量选（与 -P 传给 RTL 的参数必须一致）
import os  # noqa: E402
ALG = os.environ.get("MLDSA_ALG", "ML-DSA-44")
_P, _S = PARAMS[ALG], SIG_PARAMS[ALG]
K, ELL = _P["k"], _P["l"]
GAMMA1, BETA, OMEGA, TAU = _S["gamma1"], _S["beta"], _S["omega"], _S["tau"]
CTB = _S["ctilde"]
ZBITS = 18 if GAMMA1 == (1 << 17) else 20
ZBYTES = 256 * ZBITS // 8          # 576 / 640
SIG_H0 = CTB + ELL * ZBYTES        # hint 段起点


# dbg_sel[6:3] 选组、[2:0] 选多项式
def dbg(group, poly=0):
    return (group << 3) | poly


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
    out = [r for r in recs if r["alg"] == ALG]
    assert out, f"KAT 里没有 {ALG} sigver 记录"
    return out


def oracle_decode(rec):
    """oracle 侧的 sigDecode/pkDecode 结果 + 两个结构标志"""
    sig = bytes.fromhex(rec["sig"])
    pk = bytes.fromhex(rec["pk"])
    ct = sig[:CTB]
    off = CTB
    z = []
    for _ in range(ELL):
        z.append(polyz_unpack(sig[off:off + ZBYTES], GAMMA1))
        off += ZBYTES
    h = hint_unpack(sig[off:off + OMEGA + K], K, OMEGA)
    zbad = any(chknorm(x, GAMMA1 - BETA) for p in z for x in p)
    _, t1 = pk_decode(pk, ALG)
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

        got_ct = to_bytes(dut.ctilde.value, CTB)
        assert got_ct == o["ctilde"], f"tcId={rec.get('tcid')}：c̃ 不一致"

        assert bool(int(dut.zbad.value)) == o["zbad"], (
            f"tcId={rec.get('tcid')}：zbad 不一致 RTL={int(dut.zbad.value)} "
            f"oracle={o['zbad']}")
        assert bool(int(dut.hbad.value)) == o["hbad"], (
            f"tcId={rec.get('tcid')}：hbad 不一致 RTL={int(dut.hbad.value)} "
            f"oracle={o['hbad']}")

        # z / t₁ 在 ③ 被就地 NTT 成 ẑ / t̂₁（t₁ 装载时左移 D），done 时读到的是变换后的。
        # NTT 双射 ⇒ ẑ==ntt(z) 同时说明 18 位解包对、NTT 对（t₁ 同理）。
        from mldsa_model import ntt as _ntt
        for j in range(ELL):
            got = await read_poly(dut, dbg(0, j))
            want = _ntt(list(o["z"][j]))
            assert got == want, (
                f"tcId={rec.get('tcid')}：ẑ[{j}] 不一致，首个不同在第 "
                f"{next(i for i in range(256) if got[i] != want[i])} 个系数")
        for i in range(K):
            got = await read_poly(dut, dbg(1, i))
            want = _ntt([x << 13 for x in o["t1"][i]])
            assert got == want, (
                f"tcId={rec.get('tcid')}：t̂₁[{i}] 不一致，首个不同在第 "
                f"{next(n for n in range(256) if got[n] != want[n])} 个系数")

        # hint 位：只有结构合法时 oracle 才给出 h（非法时返回 None，位内容无意义）
        if o["h"] is not None:
            for i in range(K):
                got = await read_poly(dut, dbg(2, i))
                assert got == o["h"][i], (
                    f"tcId={rec.get('tcid')}：hint[{i}] 不一致，首个不同在第 "
                    f"{next(n for n in range(256) if got[n] != o['h'][i][n])} 位")

        dut._log.info(f"  tcId={rec.get('tcid')} ok（zbad={o['zbad']} hbad={o['hbad']}）")

    dut._log.info("① sigDecode/pkDecode：c̃/ẑ/t̂₁/hint 与 zbad/hbad 全对上 oracle")


@cocotb.test()
async def test_tr_mu_c(dut):
    """②③ tr=H(pk)、μ=H(tr‖M')、ĉ=NTT(SampleInBall(c̃))，对上 oracle

    tr 吸收整个 pk（1312 字节）；μ 走 M' 封装（含非空 ctx、长 msg）。
    ĉ 由 c 就地 NTT 而来，双射 ⇒ 对上 ntt(sample_in_ball) 说明 c 与 NTT 都对。
    """
    from mldsa_oracle import h_shake256, _mprime, sample_in_ball
    from mldsa_model import ntt as _ntt

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 挑一条应通过的（c̃ 是真签名的 c̃，SampleInBall 才有意义）
    recs = d44_records()
    rec = next(r for r in recs if r.get("result", "").lower() == "pass")
    pk, sig, msg, ctx = await preload(dut, rec)
    await run_to_done(dut)

    tr_w = h_shake256(pk, 64)
    mu_w = h_shake256(tr_w + _mprime(ctx, msg), 64)

    got_tr = to_bytes(dut.tr_out.value, 64)
    assert got_tr == tr_w, (
        f"tr 不一致，首个不同在字节 "
        f"{next(i for i in range(64) if got_tr[i] != tr_w[i])}")
    got_mu = to_bytes(dut.mu.value, 64)
    assert got_mu == mu_w, (
        f"μ 不一致，首个不同在字节 "
        f"{next(i for i in range(64) if got_mu[i] != mu_w[i])}")

    c_w = sample_in_ball(sig[:CTB], TAU)
    chat_w = _ntt(list(c_w))
    got_chat = await read_poly(dut, dbg(3))
    assert got_chat == chat_w, (
        f"ĉ 不一致，首个不同在第 "
        f"{next(i for i in range(256) if got_chat[i] != chat_w[i])} 个系数")

    dut._log.info(f"②③ tr/μ 逐字节对上 oracle（pk 1312B 吸收、msg {len(msg)}B、"
                  f"ctx {len(ctx)}B）；ĉ=NTT(SampleInBall(c̃)) 对上 oracle")


@cocotb.test()
async def test_verify_acvp(dut):
    """④ 整体：ML-DSA-44 对上 **ACVP sigver** —— 应通过全 true、应拒绝全 false

    这是 Verify-44 的最终判据。15 条向量里 3 条应通过、12 条应拒绝（3 条 hint 结构
    非法、9 条 c̃ 不匹配），两类都必须判对 —— 「错误地返回 true」是最危险的失败模式。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    recs = d44_records()

    n_pass = n_fail = 0
    for rec in recs:
        await reset(dut)
        await preload(dut, rec)
        await run_to_done(dut, limit=6_000_000)
        expect = rec.get("result", "").lower() == "pass"
        got = bool(int(dut.valid.value))
        assert got == expect, (
            f"tcId={rec.get('tcid')}：verify={got} 期望={expect}"
            f"（zbad={int(dut.zbad.value)} hbad={int(dut.hbad.value)}）")
        if expect:
            n_pass += 1
        else:
            n_fail += 1

    dut._log.info(f"④ {ALG} 对上 ACVP sigver：应通过 {n_pass} 条全 true、"
                  f"应拒绝 {n_fail} 条全 false")


async def _preload_raw(dut, pk, sig, msg, ctx):
    await load_buf(dut, dut.pk_wr_en, dut.pk_wr_addr, dut.pk_wr_data, pk)
    await load_buf(dut, dut.sig_wr_en, dut.sig_wr_addr, dut.sig_wr_data, sig)
    if msg:
        await load_buf(dut, dut.msg_wr_en, dut.msg_wr_addr, dut.msg_wr_data, msg)
    if ctx:
        await load_buf(dut, dut.ctx_wr_en, dut.ctx_wr_addr, dut.ctx_wr_data, ctx)
    dut.msg_len.value = len(msg)
    dut.ctx_len.value = len(ctx)


@cocotb.test()
async def test_reject_selfmade(dut):
    """自造反例：ACVP **没覆盖**的两条拒绝路径必须真的生效

    ACVP 的 sigver 里没有「‖z‖∞ 越界」也没有「hint 累计计数非单调 / >ω」这两类
    （见 docs/reference/mldsa-verify-design.zh-CN.md 的分类表），但 FIPS 204 要求。
    不自造反例的话，这两条逻辑等于没验 —— 而它们错了会造成**假阳性**（放过坏签名）。

    做法：拿一条 ACVP 应通过的向量当阳性对照，然后只改一处：
      ① 把 z[0][0] 的 18 位字段改成 γ₁+131000 ⇒ z = −131000，|z| ≥ γ₁−β=130994；
      ② 把 hint 累计计数改成非单调（count[1] < count[0]）；
      ③ 把 hint 累计计数改成 > ω。
    每条都必须 valid=false，且对应的标志（zbad / hbad）真的置起来 ——
    只看 valid=false 不够，c̃ 不匹配也会让 valid=false，那样等于没验到这条路径。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    recs = d44_records()
    rec = next(r for r in recs if r.get("result", "").lower() == "pass")
    pk = bytes.fromhex(rec["pk"])
    sig0 = bytes.fromhex(rec["sig"])
    msg = bytes.fromhex(rec["msg"])
    ctx = bytes.fromhex(rec.get("context", "") or "")

    # 阳性对照：不改，必须通过
    await reset(dut)
    await _preload_raw(dut, pk, sig0, msg, ctx)
    await run_to_done(dut, limit=6_000_000)
    assert bool(int(dut.valid.value)), "阳性对照居然没通过，反例无意义"
    dut._log.info(f"  阳性对照 tcId={rec.get('tcid')}：valid=true ✓")

    # ① ‖z‖∞ 越界：z[0][0] 的 18 位字段 = γ₁ + 131000 ⇒ z = −131000
    v = GAMMA1 + (GAMMA1 - BETA + 6)   # |z| = γ₁−β+6 ≥ γ₁−β，必被拒
    s = bytearray(sig0)
    # z[0][0] 占 z 区（从 sig[CTB] 起）最低的 ZBITS 位，低位在前；
    # 前两字节整个属于它，第三字节只占低 (ZBITS-16) 位，其余位要原样保留。
    _hi = ZBITS - 16                     # 2（γ₁=2¹⁷）或 4（γ₁=2¹⁹）
    _hm = (1 << _hi) - 1
    s[CTB + 0] = v & 0xFF
    s[CTB + 1] = (v >> 8) & 0xFF
    s[CTB + 2] = (s[CTB + 2] & ~_hm & 0xFF) | ((v >> 16) & _hm)
    await reset(dut)
    await _preload_raw(dut, pk, bytes(s), msg, ctx)
    await run_to_done(dut, limit=6_000_000)
    assert not bool(int(dut.valid.value)), "①：z 越界的签名居然通过了"
    assert bool(int(dut.zbad.value)), "①：z 越界但 zbad 没置起来（这条路径没验到）"
    dut._log.info("  ① ‖z‖∞ 越界：valid=false 且 zbad=1 ✓")

    # ② hint 累计计数非单调：count[1] < count[0]
    s = bytearray(sig0)
    s[SIG_H0 + OMEGA + 0] = 5
    s[SIG_H0 + OMEGA + 1] = 3
    await reset(dut)
    await _preload_raw(dut, pk, bytes(s), msg, ctx)
    await run_to_done(dut, limit=6_000_000)
    assert not bool(int(dut.valid.value)), "②：计数非单调的签名居然通过了"
    assert bool(int(dut.hbad.value)), "②：计数非单调但 hbad 没置起来"
    dut._log.info("  ② hint 累计计数非单调：valid=false 且 hbad=1 ✓")

    # ③ hint 累计计数 > ω
    s = bytearray(sig0)
    s[SIG_H0 + OMEGA + 0] = OMEGA + 1
    await reset(dut)
    await _preload_raw(dut, pk, bytes(s), msg, ctx)
    await run_to_done(dut, limit=6_000_000)
    assert not bool(int(dut.valid.value)), "③：计数 >ω 的签名居然通过了"
    assert bool(int(dut.hbad.value)), "③：计数 >ω 但 hbad 没置起来"
    dut._log.info("  ③ hint 累计计数 >ω：valid=false 且 hbad=1 ✓")

    dut._log.info("自造反例：ACVP 未覆盖的 z-norm / hint 计数两条拒绝路径均已验到")
