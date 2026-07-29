"""cocotb：ML-DSA 数据通路对拍（NTT 核见 test_mldsa_ntt.py）

覆盖 L0 算子、蝶形、高低位拆分、提示位、两类拒绝采样与均匀采样收集器。
除了与向量文件和 mldsa_model 三方比对，每一项还配一道**不同来源**的判据：

  Montgomery 约减  定义式 out·2³² ≡ in (mod q) 与输出范围
  reduce32/caddq   同余关系与输出区间
  Power2Round      分解式 a = a₁·2¹³ + a₀ 与 a₀ 的取值范围
  Decompose        分解式 a = a₁·2γ₂ + a₀、a₁ 的取值集合
  提示位           FIPS 204 依赖的性质：|e| ≤ γ₂ 时
                   UseHint(a+e, MakeHint(a₀+e, a₁)) == a₁
  拒绝采样         接受门限逐值验证（q−1 收、q 与 q+1 丢）
  收集器           真实 SHAKE128 流跑完整的 RejNTTPoly，与独立实现逐系数比对
"""
import hashlib

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

from tbutil import load, s32

from mldsa_model import (  # noqa: E402
    GAMMA2_32, GAMMA2_88, Q, caddq, decompose, make_hint, montgomery_reduce,
    power2round, reduce32, rej_eta_coeff, rej_uniform_coeff, use_hint,
)

from mldsa_model import ct_butterfly, gs_butterfly  # noqa: E402


def s64(x: int) -> int:
    x &= (1 << 64) - 1
    return x - (1 << 64) if x >= (1 << 63) else x


@cocotb.test()
async def test_l0_ops(dut):
    """Montgomery 约减 / reduce32 / caddq：向量、模型、定义式四方一致"""
    rows = load("mldsa_ops.hex")
    counts = {0: 0, 1: 0, 2: 0}
    for row in rows:
        op = int(row[0], 16)
        if op == 0:
            a = s64(int(row[1], 16))
            dut.mont_a.value = a & ((1 << 64) - 1)
            await Timer(1, unit="ns")
            got = s32(int(dut.mont_out.value))
            assert got == s32(int(row[2], 16)) == montgomery_reduce(a), (
                f"mont_reduce({a})：RTL={got}")
            assert (got * (1 << 32) - a) % Q == 0, f"定义式不成立：a={a} out={got}"
            assert -Q < got < Q, f"输出超出 (−q, q)：{got}"
        elif op == 1:
            a = s32(int(row[1], 16))
            dut.red_a.value = a & 0xFFFFFFFF
            await Timer(1, unit="ns")
            got = s32(int(dut.red_out.value))
            assert got == s32(int(row[2], 16)) == reduce32(a), f"reduce32({a})：{got}"
            assert (got - a) % Q == 0, "reduce32 改变了同余类"
            assert -6283009 < got <= 6283008, f"reduce32 输出越界：{got}"
        else:
            a = s32(int(row[1], 16))
            dut.cadd_a.value = a & 0xFFFFFFFF
            await Timer(1, unit="ns")
            got = s32(int(dut.cadd_out.value))
            assert got == s32(int(row[2], 16)) == caddq(a), f"caddq({a})：{got}"
            assert (got - a) % Q == 0 and got >= 0, "caddq 未把系数折回非负"
        counts[op] += 1
    dut._log.info(f"mldsa L0 算子：mont {counts[0]} / reduce32 {counts[1]} / "
                  f"caddq {counts[2]} 条四方一致")


@cocotb.test()
async def test_butterfly(dut):
    """CT / GS 蝶形：向量与模型三方一致"""
    rows = load("mldsa_butterfly.hex")
    n = 0
    for kind, a_h, b_h, z_h, ao_h, bo_h in rows:
        a, b, z = s32(int(a_h, 16)), s32(int(b_h, 16)), s32(int(z_h, 16))
        dut.bf_a.value = a & 0xFFFFFFFF
        dut.bf_b.value = b & 0xFFFFFFFF
        dut.bf_zeta.value = z & 0xFFFFFFFF
        await Timer(1, unit="ns")
        if int(kind, 16) == 0:
            got = (s32(int(dut.ct_ao.value)), s32(int(dut.ct_bo.value)))
            model = ct_butterfly(a, b, z)
        else:
            got = (s32(int(dut.gs_ao.value)), s32(int(dut.gs_bo.value)))
            model = gs_butterfly(a, b, z)
        exp = (s32(int(ao_h, 16)), s32(int(bo_h, 16)))
        assert got == exp == model, f"蝶形 kind={kind} 不匹配：RTL={got} 向量={exp}"
        n += 1
    dut._log.info(f"mldsa 蝶形：{n} 条三方一致")


@cocotb.test()
async def test_rounding(dut):
    """Power2Round / Decompose：向量、模型三方一致，并验证分解式与取值范围"""
    rows = load("mldsa_rounding.hex")
    for row in rows:
        a = s32(int(row[0], 16))
        dut.rnd_a.value = a & 0xFFFFFFFF
        await Timer(1, unit="ns")

        p0 = s32(int(dut.p2r_a0.value))
        p1 = int(dut.p2r_a1.value)
        assert (p0, p1) == (s32(int(row[1], 16)), int(row[2], 16)) \
            == power2round(a), f"power2round({a})：RTL={(p0, p1)}"
        assert (p1 * (1 << 13) + p0 - a) % Q == 0, "Power2Round 分解式不成立"
        assert -(1 << 12) < p0 <= (1 << 12), f"a0 越界：{p0}"

        for gamma2, a0_sig, a1_sig, m, col in (
                (GAMMA2_88, dut.d88_a0, dut.d88_a1, 44, 3),
                (GAMMA2_32, dut.d32_a0, dut.d32_a1, 16, 5)):
            g0 = s32(int(a0_sig.value))
            g1 = int(a1_sig.value)
            assert (g0, g1) == (s32(int(row[col], 16)), int(row[col + 1], 16)) \
                == decompose(a, gamma2), f"decompose({a}, {gamma2})：RTL={(g0, g1)}"
            assert (g1 * 2 * gamma2 + g0 - a) % Q == 0, "Decompose 分解式不成立"
            assert 0 <= g1 < m, f"a1 越界：{g1}"
    dut._log.info(f"mldsa 高低位拆分：{len(rows)} 条三方一致，分解式与取值范围成立")


@cocotb.test()
async def test_hint(dut):
    """提示位：向量、模型三方一致，并验证 FIPS 204 依赖的还原性质"""
    rows = load("mldsa_hint.hex")
    n = 0
    for mode_h, a_h, e_h, h_s, used_h in rows:
        mode = int(mode_h, 16)
        gamma2 = GAMMA2_88 if mode == 0 else GAMMA2_32
        a = int(a_h, 16)
        e = s32(int(e_h, 16))
        a0, a1 = decompose(a, gamma2)

        dut.mh_a0.value = (a0 + e) & 0xFFFFFFFF
        dut.mh_a1.value = a1
        await Timer(1, unit="ns")
        hint = int((dut.mh88 if mode == 0 else dut.mh32).value)
        assert hint == int(h_s) == make_hint(a0 + e, a1, gamma2), (
            f"make_hint mode={mode} a={a} e={e}：RTL={hint}")

        dut.uh_a.value = (a + e) % Q
        dut.uh_hint.value = hint
        await Timer(1, unit="ns")
        used = int((dut.uh88 if mode == 0 else dut.uh32).value)
        assert used == int(used_h, 16) == use_hint((a + e) % Q, hint, gamma2), (
            f"use_hint mode={mode} a={a} e={e}：RTL={used}")
        # |e| ≤ γ₂ 时必须还原出原来的高位
        assert used == a1, f"提示位未能还原高位：mode={mode} a={a} e={e}"
        n += 1
    dut._log.info(f"mldsa 提示位：{n} 条三方一致，UseHint∘MakeHint 还原高位")


@cocotb.test()
async def test_sampling(dut):
    """两类拒绝采样：向量、模型三方一致；接受门限逐值验证"""
    rows = load("mldsa_sample.hex")
    n_u = n_e = 0
    for row in rows:
        if int(row[0], 16) == 0:
            v = int(row[1], 16)
            dut.ru_bytes.value = v
            await Timer(1, unit="ns")
            got = (int(dut.ru_cand.value), int(dut.ru_ok.value))
            exp = (int(row[2], 16), int(row[3]))
            model = rej_uniform_coeff(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)
            assert got == exp == model, f"均匀采样 {v:#08x}：RTL={got} 向量={exp}"
            n_u += 1
        else:
            eta = int(row[1], 16)
            nib = int(row[2], 16)
            dut.re_nib.value = nib
            await Timer(1, unit="ns")
            sig = (dut.re2_coeff, dut.re2_ok) if eta == 2 else (dut.re4_coeff, dut.re4_ok)
            got = (s32(int(sig[0].value)), int(sig[1].value))
            exp = (s32(int(row[3], 16)), int(row[4]))
            model = rej_eta_coeff(nib, eta)
            # 被拒绝时系数无意义（硬件照样算出一个值，调用方不会取用），
            # 所以只在接受时比对系数本身，拒绝与否则始终比对
            assert got[1] == exp[1] == model[1], f"有界采样 η={eta} nib={nib} 的接受判定不符"
            if got[1]:
                assert got[0] == exp[0] == model[0], f"有界采样 η={eta} nib={nib}：RTL={got}"
                assert -eta <= got[0] <= eta, f"η={eta} 采样值越界：{got[0]}"
            n_e += 1

    # 接受门限：q−1 收下，q 与 q+1 丢弃
    for target, want in ((Q - 1, 1), (Q, 0), (Q + 1, 0), (0x7FFFFF, 0)):
        dut.ru_bytes.value = target & 0xFFFFFF
        await Timer(1, unit="ns")
        assert int(dut.ru_cand.value) == target
        assert int(dut.ru_ok.value) == want, f"候选 {target} 的接受判定不对"
    dut._log.info(f"mldsa 采样：均匀 {n_u} 条 / 有界 {n_e} 条三方一致，门限逐值验证")


def rej_uniform_reference(stream: bytes) -> list[int]:
    """独立实现的 RejNTTPoly 收集逻辑，直接照 FIPS 204 Alg 30 写"""
    out = []
    pos = 0
    while len(out) < 256:
        t = (stream[pos] | (stream[pos + 1] << 8) | (stream[pos + 2] << 16)) & 0x7FFFFF
        pos += 3
        if t < Q:
            out.append(t)
    return out


@cocotb.test()
async def test_rej_uniform_buf(dut):
    """收集器：用真实 SHAKE128 流跑完整的 RejNTTPoly，与独立实现逐系数比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    dut.rst_n.value = 0
    dut.rb_start.value = 0
    dut.rb_valid.value = 0
    dut.rb_bytes.value = 0
    dut.rb_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)

    for trial in range(3):
        seed = bytes([trial]) * 32
        stream = hashlib.shake_128(seed + bytes([0, trial])).digest(168 * 8)
        want = rej_uniform_reference(stream)

        dut.rb_start.value = 1
        await RisingEdge(dut.clk)
        dut.rb_start.value = 0
        await Timer(1, unit="ns")
        assert int(dut.rb_done.value) == 0, "start 之后 done 应当已被清掉"
        assert int(dut.rb_count.value) == 0

        pos = 0
        while int(dut.rb_done.value) != 1:
            assert pos + 3 <= len(stream), "字节流耗尽而收集器仍未完成"
            dut.rb_valid.value = 1
            dut.rb_bytes.value = (stream[pos] | (stream[pos + 1] << 8)
                                  | (stream[pos + 2] << 16))
            await RisingEdge(dut.clk)
            await Timer(1, unit="ns")
            pos += 3
        dut.rb_valid.value = 0
        assert int(dut.rb_count.value) == 256

        got = []
        for i in range(256):
            dut.rb_addr.value = i
            await Timer(1, unit="ns")
            got.append(int(dut.rb_data.value))
        assert got == want, f"第 {trial} 组采样结果与独立实现不一致"
        assert all(0 <= c < Q for c in got), "采样结果越出 [0, q)"
    dut._log.info("mldsa_rej_uniform_buf: 3 组 SHAKE128 流的 RejNTTPoly 与独立实现一致")
