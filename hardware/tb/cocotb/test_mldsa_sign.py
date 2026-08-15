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
from mldsa_oracle import (  # noqa: E402
    _load_records, _mprime, h_shake256, sk_decode)

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
    """① skDecode：ρ/K/tr 逐字节对上 oracle 的 sk_decode（用 ACVP sk）

    s₁/s₂/t₀ 的系数在第 ③ 段被 NTT 就地覆盖成 ŝ₁/ŝ₂/t̂₀，done 时读到的已不是原始值 ——
    与 KeyGen 一样，它们的采样/解包正确性由 ③ 的 NTT 用例间接验（NTT 双射：
    ŝ == ntt(sk_decode 的 s) 成立就同时说明 skDecode 对、NTT 对）。这里只独立验
    不被覆盖的 ρ/K/tr。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    sk = bytes.fromhex(rec["sk"])
    assert len(sk) == 2560
    await load_buf(dut, dut.sk_wr_en, dut.sk_wr_addr, dut.sk_wr_data, sk)
    await run_to_done(dut)

    rho_w, key_w, tr_w, _, _, _ = sk_decode(sk, "ML-DSA-44")
    assert to_bytes(dut.rho.value, 32) == rho_w, "ρ 不一致"
    assert to_bytes(dut.key_out.value, 32) == key_w, "K 不一致"
    assert to_bytes(dut.tr_out.value, 64) == tr_w, "tr 不一致"

    dut._log.info("① skDecode：ρ/K/tr 逐字节对上 oracle（s₁/s₂/t₀ 由 ③ NTT 链间接验）")


async def preload_all(dut, rec):
    """把一条 KAT 的 sk/msg/ctx/rnd/lengths 全部载入 DUT"""
    sk = bytes.fromhex(rec["sk"])
    msg = bytes.fromhex(rec["msg"])
    ctx = bytes.fromhex(rec.get("context", "") or "")
    rnd = bytes.fromhex(rec["rnd"])
    assert len(sk) == 2560 and len(rnd) == 32
    await load_buf(dut, dut.sk_wr_en, dut.sk_wr_addr, dut.sk_wr_data, sk)
    if msg:
        await load_buf(dut, dut.msg_wr_en, dut.msg_wr_addr, dut.msg_wr_data, msg)
    if ctx:
        await load_buf(dut, dut.ctx_wr_en, dut.ctx_wr_addr, dut.ctx_wr_data, ctx)
    dut.msg_len.value = len(msg)
    dut.ctx_len.value = len(ctx)
    dut.rnd.value = int.from_bytes(rnd, "little")
    return sk, msg, ctx, rnd


async def run_to_done(dut, limit=1_000_000):
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    for _ in range(limit):
        await RisingEdge(dut.clk)
        if int(dut.done.value):
            break
    else:
        raise AssertionError("一直没完成")
    await Timer(1, unit="ns")


@cocotb.test()
async def test_derive_mu_rhopp(dut):
    """② μ = H(tr‖M')、ρ'' = H(K‖rnd‖μ) 对上 oracle（含非空 ctx / 多字节 msg）"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()      # 确定性向量：rnd 全零、msg 6597 B、ctx 96 B
    sk, msg, ctx, rnd = await preload_all(dut, rec)
    await run_to_done(dut)

    _, key_w, tr_w, _, _, _ = sk_decode(sk, "ML-DSA-44")
    mu_w = h_shake256(tr_w + _mprime(ctx, msg), 64)
    rhopp_w = h_shake256(key_w + rnd + mu_w, 64)

    assert to_bytes(dut.mu.value, 64) == mu_w, (
        f"μ 不一致，首个不同在字节 "
        f"{next(i for i in range(64) if to_bytes(dut.mu.value, 64)[i] != mu_w[i])}")
    assert to_bytes(dut.rhopp.value, 64) == rhopp_w, "ρ'' 不一致"

    dut._log.info("② μ、ρ'' 逐字节对上 oracle（M' 封装、非空 ctx、6597B msg 都验到）")


@cocotb.test()
async def test_ntt_prep(dut):
    """③ ŝ₁/ŝ₂/t̂₀ = NTT(s₁/s₂/t₀)：done 后读三个存储，对 ntt(sk_decode 出来的原始值)

    验的是整条链：sk 解包 → NTT。NTT 双射，所以 ŝ 对上 ntt(oracle_s) 同时说明
    skDecode 与 NTT 都对（① 的系数独立检查被这条覆盖，同 KeyGen 的做法）。
    """
    from mldsa_model import ntt as _ntt

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    sk = bytes.fromhex(rec["sk"])
    await load_buf(dut, dut.sk_wr_en, dut.sk_wr_addr, dut.sk_wr_data, sk)
    await run_to_done(dut)

    _, _, _, s1_w, s2_w, t0_w = sk_decode(sk, "ML-DSA-44")

    for j in range(4):
        got = await read_poly(dut, 0b00000 | j)         # ŝ₁[j]
        want = _ntt(list(s1_w[j]))
        assert got == want, (
            f"ŝ₁[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != want[i])} 个系数")
    for j in range(4):
        got = await read_poly(dut, 0b01000 | j)         # ŝ₂[j]
        assert got == _ntt(list(s2_w[j])), f"ŝ₂[{j}] 不一致"
    for j in range(4):
        got = await read_poly(dut, 0b01100 | j)         # t̂₀[j]
        want = _ntt(list(t0_w[j]))
        assert got == want, (
            f"t̂₀[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != want[i])} 个系数")

    dut._log.info("③ ŝ₁/ŝ₂/t̂₀ 全部对上 ntt(sk_decode)，整条 sk→解包→NTT 链都对")


def derive_mu(rec):
    """从一条 KAT 复现 μ = H(tr‖M')"""
    sk = bytes.fromhex(rec["sk"])
    msg = bytes.fromhex(rec["msg"])
    ctx = bytes.fromhex(rec.get("context", "") or "")
    _, _, tr_w, _, _, _ = sk_decode(sk, "ML-DSA-44")
    return h_shake256(tr_w + _mprime(ctx, msg), 64)


def derive_rhopp(rec):
    """从一条 KAT 复现 ρ''（ExpandMask 的种子）"""
    sk = bytes.fromhex(rec["sk"])
    key_w = sk_decode(sk, "ML-DSA-44")[1]
    rnd = bytes.fromhex(rec["rnd"])
    return h_shake256(key_w + rnd + derive_mu(rec), 64)


@cocotb.test()
async def test_ymask_ntt(dut):
    """④+⑤a ŷ = NTT(ExpandMask(ρ'', κ=0))：done 后读 y 存储（已被 NTT 覆盖）

    y 在 ⑤ 被就地 NTT 成 ŷ，done 时读到的是 ŷ。NTT 双射，所以 ŷ 对上
    ntt(expand_mask) 同时说明 ExpandMask 的 18 位解包 / γ₁−v 对、NTT 对，
    并间接验到整条 sk→μ→ρ'' 前置链（ρ'' 是 ExpandMask 的种子）。
    """
    from mldsa_oracle import expand_mask
    from mldsa_model import ntt as _ntt

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    await preload_all(dut, rec)
    await run_to_done(dut)

    y_w = expand_mask(derive_rhopp(rec), 0, 1 << 17, 4)   # κ=0, γ₁=2¹⁷, ℓ=4
    for j in range(4):
        got = await read_poly(dut, 0b10000 | j)           # dbg_sel[4:2]=100 → y[j]（=ŷ[j]）
        want = _ntt(list(y_w[j]))
        assert got == want, (
            f"ŷ[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got[i] != want[i])} 个系数")

    dut._log.info("④+⑤a ŷ[0..3] = NTT(ExpandMask(ρ'',0)) 全对上 oracle")


@cocotb.test()
async def test_w_decompose(dut):
    """⑤ w = invNTT(Â∘ŷ)、(w0,w1)=Decompose(caddq(w))：done 后读 w0/w1（κ=0 轮）

    这条把整条主循环前半段串起来验：Â 现采（ExpandA）、逐点 mont 乘累加、reduce32、
    invNTT、caddq、decompose 高低位拆分，全部对上 oracle κ=0 轮的 w0/w1。
    """
    from mldsa_oracle import expand_mask, rej_uniform_poly
    from mldsa_model import (ntt as _ntt, invntt_tomont as _intt,
                             montgomery_reduce as _mont, reduce32 as _r32,
                             caddq as _cad, decompose as _dec)

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    rho_w = bytes.fromhex(rec["sk"])[:32]     # sk 前 32 字节就是 ρ
    await preload_all(dut, rec)
    await run_to_done(dut)

    gamma2 = 95232
    y_w = expand_mask(derive_rhopp(rec), 0, 1 << 17, 4)
    yhat = [_ntt(list(p)) for p in y_w]

    for i in range(4):
        acc = [0] * 256
        for j in range(4):
            a = rej_uniform_poly(rho_w, (i << 8) + j)
            for n in range(256):
                acc[n] += _mont(a[n] * yhat[j][n])
        acc = _intt([_r32(x) for x in acc])
        w = [_cad(x) for x in acc]
        pair = [_dec(x, gamma2) for x in w]
        w0_want = [p[0] for p in pair]
        w1_want = [p[1] for p in pair]

        got_w0 = await read_poly(dut, 0b10100 | i)    # [4:2]=101 → w0[i]
        assert got_w0 == w0_want, (
            f"w0[{i}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got_w0[n] != w0_want[n])} 个系数")
        got_w1 = await read_poly(dut, 0b11000 | i)    # [4:2]=110 → w1[i]
        assert got_w1 == w1_want, (
            f"w1[{i}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got_w1[n] != w1_want[n])} 个系数")

    dut._log.info("⑤ w0/w1[0..3] 对上 oracle κ=0 轮（ExpandA+MAC+invNTT+caddq+decompose 全链）")


def oracle_w1_kappa0(rec):
    """复现 oracle κ=0 轮的 w1（列表 of k 条多项式），供 ⑥ 用"""
    from mldsa_oracle import expand_mask, rej_uniform_poly
    from mldsa_model import (ntt as _ntt, invntt_tomont as _intt,
                             montgomery_reduce as _mont, reduce32 as _r32,
                             caddq as _cad, decompose as _dec)
    rho_w = bytes.fromhex(rec["sk"])[:32]
    y_w = expand_mask(derive_rhopp(rec), 0, 1 << 17, 4)
    yhat = [_ntt(list(p)) for p in y_w]
    w1 = []
    for i in range(4):
        acc = [0] * 256
        for j in range(4):
            a = rej_uniform_poly(rho_w, (i << 8) + j)
            for n in range(256):
                acc[n] += _mont(a[n] * yhat[j][n])
        acc = _intt([_r32(x) for x in acc])
        w = [_cad(x) for x in acc]
        w1.append([_dec(x, 95232)[1] for x in w])
    return w1


@cocotb.test()
async def test_ctilde_and_c(dut):
    """⑥ c̃ = H(μ‖w1pack)、c = SampleInBall(c̃)（κ=0 轮），对上 oracle

    c̃ 走 ctilde 端口比对；c 经 dbg 组 111 读出比对。这条把 w₁ 的 6 位打包、
    μ‖w1pack 的 SHAKE256、以及 SampleInBall 的 τ=39 个 ±1 稀疏放置全验到。
    """
    from mldsa_oracle import polyw1_pack, sample_in_ball

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    await preload_all(dut, rec)
    await run_to_done(dut)

    mu_w = derive_mu(rec)
    w1 = oracle_w1_kappa0(rec)
    ctilde_w = h_shake256(mu_w + b"".join(polyw1_pack(w1[i], 95232) for i in range(4)), 32)
    c_w = sample_in_ball(ctilde_w, 39)

    got_ct = to_bytes(dut.ctilde.value, 32)
    assert got_ct == ctilde_w, (
        f"c̃ 不一致，首个不同在字节 "
        f"{next(i for i in range(32) if got_ct[i] != ctilde_w[i])}")

    got_c = await read_poly(dut, 0b11100)     # dbg_sel[4:2]=111 → c
    assert got_c == c_w, (
        f"c 不一致，首个不同在第 "
        f"{next(i for i in range(256) if got_c[i] != c_w[i])} 个系数")

    dut._log.info("⑥ c̃ 对上 H(μ‖w1pack)、c 对上 SampleInBall(c̃)（κ=0 轮，τ=39）")

