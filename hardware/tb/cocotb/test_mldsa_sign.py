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


_R0_REC = None


def first_d44(deterministic=True):
    """返回一条 **κ=0 就被接受** 的确定性 ML-DSA-44 向量。

    ⑩ 的拒绝循环让 done 落在「接受轮」而不是 κ=0 轮 —— 若 κ=0 被拒，done 时读到的
    中间量（z/w1/r0/hint/y…）是接受轮的、不是 κ=0 的。逐段用例都对 oracle 的 κ=0 轮
    比，所以这里挑一条恰好 κ=0 就通过的向量：此时接受轮 == κ=0 轮，done 的存储就是
    κ=0 的，用例仍成立（顺带让逐段用例只跑一轮、快）。哪条 κ=0 接受由 oracle 现算。
    """
    global _R0_REC
    if _R0_REC is not None:
        return _R0_REC
    recs = _load_records(SIGGEN_KAT)
    if not recs:
        raise AssertionError("找不到 vectors/mldsa_siggen.kat")
    cand = [r for r in recs if r["alg"] == "ML-DSA-44"
            and (not deterministic or r.get("deterministic", "") == "1")]
    for r in cand:
        if not oracle_round0(r)["reject"]:      # κ=0 轮未被拒 ⇒ 接受轮就是 κ=0
            _R0_REC = r
            return r
    _R0_REC = cand[0]                           # 兜底（不该发生）
    return _R0_REC


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
    # 用完整 msg/ctx/rnd（first_d44 保证 κ=0 就接受 ⇒ done 落在 κ=0 轮），
    # 这样 done 快且中间量存储 = κ=0。ρ/K/tr 与 msg 无关，值不受影响。
    await preload_all(dut, rec)
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
    await preload_all(dut, rec)      # 完整 msg ⇒ κ=0 接受 ⇒ done 落在 κ=0 轮
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
    """④ y = ExpandMask(ρ'',κ=0)（原值，组 100）+ ⑤a ŷ = NTT(y)（组 001）

    y 保留原值（⑦ z=y+cs₁ 要用），ŷ 另存。直接验 y 对上 expand_mask（18 位解包
    / γ₁−v），再验 ŷ 对上 ntt(y)。
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
        got_y = await read_poly(dut, 0b10000 | j)         # 组 100 → y[j]（原值）
        assert got_y == y_w[j], (
            f"y[{j}] 不一致，首个不同在第 "
            f"{next(i for i in range(256) if got_y[i] != y_w[j][i])} 个系数")
        got_yh = await read_poly(dut, 0b00100 | j)        # 组 001 → ŷ[j]
        want_yh = _ntt(list(y_w[j]))
        assert got_yh == want_yh, f"ŷ[{j}] 不一致"

    dut._log.info("④ y[0..3]=ExpandMask(ρ'',0) 原值 + ⑤a ŷ[0..3]=NTT(y) 全对上 oracle")


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
        w1_want = [p[1] for p in pair]

        # w0 在 ⑧ 被就地覆盖成 r₀，done 时读到的不是 w0；w0 的正确性由 test_r0_hint
        # 的 r₀=reduce32(w0−cs₂) 间接验（r₀ 对 ⇒ w0 对）。这里只验不被覆盖的 w1。
        got_w1 = await read_poly(dut, 0b11000 | i)    # [5:2]=0110 → w1[i]
        assert got_w1 == w1_want, (
            f"w1[{i}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got_w1[n] != w1_want[n])} 个系数")

    dut._log.info("⑤ w1[0..3] 对上 oracle κ=0 轮（ExpandA+MAC+invNTT+caddq+decompose；w0 由 ⑧ r₀ 间接验）")


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
    from mldsa_model import ntt as _ntt

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

    # c 在 ⑦ 被就地 NTT 成 ĉ，done 时读到的是 ĉ；NTT 双射 ⇒ ĉ==ntt(c) 说明
    # SampleInBall 的 c 对、c-NTT 对（c 的原值也由 ⑦ z 用例间接验）。
    got_chat = await read_poly(dut, 7 << 2)   # 组 7 → c（=ĉ）
    chat_w = _ntt(list(c_w))
    assert got_chat == chat_w, (
        f"ĉ 不一致，首个不同在第 "
        f"{next(i for i in range(256) if got_chat[i] != chat_w[i])} 个系数")

    dut._log.info("⑥ c̃ 对上 H(μ‖w1pack)；ĉ=NTT(SampleInBall(c̃)) 对上 oracle（κ=0，τ=39）")


async def read_flag(dut, group):
    """读一个标志/标量（dbg 组 group，idx=0）"""
    dut.dbg_sel.value = group << 2
    dut.dbg_idx.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    return int(dut.dbg_coef.value)


def oracle_round0(rec):
    """复现 oracle κ=0 轮的全部中间量：y, ŝ₁/ŝ₂/t̂₀, c, z, r0, hint, weight, 各拒绝判据"""
    from mldsa_oracle import expand_mask, rej_uniform_poly, sample_in_ball, polyw1_pack
    from mldsa_model import (ntt as _ntt, invntt_tomont as _intt,
                             montgomery_reduce as _mont, reduce32 as _r32,
                             caddq as _cad, decompose as _dec, chknorm as _chk,
                             make_hint as _mkh)
    sk = bytes.fromhex(rec["sk"])
    rho_w, key_w, tr_w, s1, s2, t0 = sk_decode(sk, "ML-DSA-44")
    gamma1, gamma2, beta, tau, omega = 1 << 17, 95232, 78, 39, 80
    s1h = [_ntt(list(p)) for p in s1]
    s2h = [_ntt(list(p)) for p in s2]
    t0h = [_ntt(list(p)) for p in t0]
    mu_w = derive_mu(rec)
    rhopp = derive_rhopp(rec)
    y = expand_mask(rhopp, 0, gamma1, 4)
    yhat = [_ntt(list(p)) for p in y]
    w0, w1 = [], []
    for i in range(4):
        acc = [0] * 256
        for j in range(4):
            a = rej_uniform_poly(rho_w, (i << 8) + j)
            for n in range(256):
                acc[n] += _mont(a[n] * yhat[j][n])
        acc = _intt([_r32(x) for x in acc])
        pr = [_dec(_cad(x), gamma2) for x in acc]
        w0.append([p[0] for p in pr]); w1.append([p[1] for p in pr])
    ctil = h_shake256(mu_w + b"".join(polyw1_pack(w1[i], gamma2) for i in range(4)), 32)
    c = sample_in_ball(ctil, tau)
    chat = _ntt(list(c))
    # z
    z = []
    for j in range(4):
        cs1 = _intt([_mont(chat[n] * s1h[j][n]) for n in range(256)])
        z.append([_r32(y[j][n] + cs1[n]) for n in range(256)])
    z_bad = any(_chk(x, gamma1 - beta) for p in z for x in p)
    # r0
    r0 = []
    for i in range(4):
        cs2 = _intt([_mont(chat[n] * s2h[i][n]) for n in range(256)])
        r0.append([_r32(w0[i][n] - cs2[n]) for n in range(256)])
    r0_bad = any(_chk(x, gamma2 - beta) for p in r0 for x in p)
    # hint
    hint, weight, ct0_bad = [], 0, False
    for i in range(4):
        ct0 = [_r32(x) for x in _intt([_mont(chat[n] * t0h[i][n]) for n in range(256)])]
        if any(_chk(x, gamma2) for x in ct0):
            ct0_bad = True
        a0 = [_r32(r0[i][n] + ct0[n]) for n in range(256)]
        hi = [_mkh(a0[n], w1[i][n], gamma2) for n in range(256)]
        weight += sum(hi); hint.append(hi)
    reject = z_bad or r0_bad or ct0_bad or (weight > omega)
    return dict(y=y, z=z, z_bad=z_bad, r0=r0, r0_bad=r0_bad, w1=w1, c=c,
               hint=hint, weight=weight, ct0_bad=ct0_bad, reject=reject)


@cocotb.test()
async def test_z_norm(dut):
    """⑦ z[j]=reduce32(y[j]+invNTT(ĉ∘ŝ₁[j])) 及 ‖z‖∞ 检查（κ=0 轮），对上 oracle"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    await preload_all(dut, rec)
    await run_to_done(dut)

    o = oracle_round0(rec)
    for j in range(4):
        got = await read_poly(dut, (8 << 2) | j)      # 组 8 → z[j]
        assert got == o["z"][j], (
            f"z[{j}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got[n] != o['z'][j][n])} 个系数")

    # κ=0 轮的 z-norm 拒绝判据（此段只算了 z，reject 只反映 ‖z‖∞）
    rej = await read_flag(dut, 10)
    assert bool(rej) == o["z_bad"], f"z-norm 拒绝标志不一致：RTL={rej} oracle={o['z_bad']}"

    dut._log.info(f"⑦ z[0..3] 对上 oracle κ=0 轮；‖z‖∞ 拒绝标志={bool(rej)}（=oracle）")


@cocotb.test()
async def test_r0_hint(dut):
    """⑧ r₀=w0−c·s₂ 及 ‖r₀‖∞ 检查、⑨ ct₀/MakeHint/权重（κ=0 轮），对上 oracle"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = first_d44()
    await preload_all(dut, rec)
    await run_to_done(dut)

    o = oracle_round0(rec)

    for i in range(4):
        got = await read_poly(dut, (5 << 2) | i)      # 组 5 → r₀（就地覆盖 w0）
        assert got == o["r0"][i], (
            f"r₀[{i}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got[n] != o['r0'][i][n])} 个系数")

    for i in range(4):
        got = await read_poly(dut, (9 << 2) | i)      # 组 9 → hint[i]
        want = o["hint"][i]
        assert got == want, (
            f"hint[{i}] 不一致，首个不同在第 "
            f"{next(n for n in range(256) if got[n] != want[n])} 位")

    w = await read_flag(dut, 11)
    assert w == o["weight"], f"hint 权重不一致：RTL={w} oracle={o['weight']}"

    # done 时 reject = z_bad | r0_bad | ct0_bad（权重>ω 的判定留到 ⑩）
    rej = await read_flag(dut, 10)
    exp = o["z_bad"] or o["r0_bad"] or o["ct0_bad"]
    assert bool(rej) == exp, f"拒绝标志不一致：RTL={rej} oracle(z|r0|ct0)={exp}"

    dut._log.info(f"⑧⑨ r₀[0..3]/hint[0..3]/权重={o['weight']} 全对上 oracle κ=0 轮；"
                  f"reject={bool(rej)}")


async def read_sig(dut, n):
    """done 之后经 sig 口读 n 个字节"""
    out = bytearray()
    for a in range(n):
        dut.sig_addr.value = a
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        out.append(int(dut.sig_data.value) & 0xFF)
    return bytes(out)


async def _run_acvp(dut, recs, label):
    n_ok = 0
    import os
    _lim = os.environ.get("ACVP_N")
    if _lim:
        recs = recs[:int(_lim)]
    for idx, rec in enumerate(recs):
        await reset(dut)
        await preload_all(dut, rec)
        await run_to_done(dut, limit=8_000_000)
        sig_w = bytes.fromhex(rec["sig"])
        sig = await read_sig(dut, len(sig_w))
        assert sig == sig_w, (
            f"[{label}] tcId={rec.get('tcid')}（第 {idx} 条）σ 与 ACVP 不一致，首个不同在字节 "
            f"{next(i for i in range(len(sig_w)) if sig[i] != sig_w[i])}"
            f"（len 得 {len(sig)} / 期望 {len(sig_w)}）")
        n_ok += 1
    dut._log.info(f"⑩ ML-DSA-44 {label} siggen 全对上 ACVP：{n_ok} 条签名逐字节一致")


@cocotb.test()
async def test_sign_acvp_det(dut):
    """⑩ 整体：确定性 ML-DSA-44 —— 拒绝循环跑到接受轮，σ 逐字节对上 **ACVP siggen**

    这是 Sign-44 的最终判据：不再只对 oracle 的中间量，而是用 ACVP 的 sk/msg/ctx/rnd
    驱动 RTL，读出的完整 2420 字节签名与 ACVP 期望签名逐字节一致。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    recs = [r for r in _load_records(SIGGEN_KAT)
            if r["alg"] == "ML-DSA-44" and r.get("deterministic", "") == "1"]
    assert recs, "找不到确定性 ML-DSA-44 向量"
    await _run_acvp(dut, recs, "确定性")


@cocotb.test()
async def test_sign_acvp_rnd(dut):
    """⑩ 整体：非确定性 ML-DSA-44 —— rnd 取自 KAT，σ 逐字节对上 **ACVP siggen**

    非确定性只是 rnd 非零；RTL 把 rnd 当输入喂进 ρ''，与确定性同一条路径。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    recs = [r for r in _load_records(SIGGEN_KAT)
            if r["alg"] == "ML-DSA-44" and r.get("deterministic", "") != "1"]
    assert recs, "找不到非确定性 ML-DSA-44 向量"
    await _run_acvp(dut, recs, "非确定性")

