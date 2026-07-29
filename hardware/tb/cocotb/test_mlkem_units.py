"""cocotb：ML-KEM 数据通路小模块对拍

覆盖 CBD 采样、拒绝采样（取候选 + 收集器）、12 位编解码。
除了与向量文件和 ref_model 三方比对，每一项还配一道**不同来源**的判据：

  CBD        对着 FIPS 203 Alg 8 的逐比特汉明重量定义，位并行技巧不参与
  拒绝采样   用 SHAKE128 现场生成字节流跑完整的 SampleNTT，结果与 hashlib 驱动的
             独立实现逐系数比对；再验证输出全部落在 [0, q)
  编解码     往返还原 + 与按位拼接的定义式比对
"""
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

from tbutil import load, s16

from ref_model import Q, cbd2, cbd3, decode12, encode12, rej_pair  # noqa: E402


def cbd_definition(bits: int, eta: int, n: int) -> list[int]:
    """FIPS 203 Alg 8：前 η 位的汉明重量减后 η 位的汉明重量"""
    out = []
    for i in range(n):
        x = sum((bits >> (2 * i * eta + k)) & 1 for k in range(eta))
        y = sum((bits >> (2 * i * eta + eta + k)) & 1 for k in range(eta))
        out.append(x - y)
    return out


def unpack(word: int, n: int) -> list[int]:
    return [s16((word >> (16 * i)) & 0xFFFF) for i in range(n)]


@cocotb.test()
async def test_cbd_vectors(dut):
    """CBD：向量文件、ref_model、FIPS 203 定义式四方一致"""
    rows = load("cbd.hex")
    n2 = n3 = 0
    for row in rows:
        eta = int(row[0], 16)
        v = int(row[1], 16)
        exp = [s16(int(x, 16)) for x in row[2:]]
        if eta == 2:
            dut.cbd2_in.value = v
            await Timer(1, unit="ns")
            got = unpack(int(dut.cbd2_out.value), 8)
            assert got == exp == cbd2(v) == cbd_definition(v, 2, 8), (
                f"cbd2 输入 {v:#010x}：RTL={got} 向量={exp}")
            n2 += 1
        else:
            dut.cbd3_in.value = v
            await Timer(1, unit="ns")
            got = unpack(int(dut.cbd3_out.value), 4)
            assert got == exp == cbd3(v) == cbd_definition(v, 3, 4), (
                f"cbd3 输入 {v:#08x}：RTL={got} 向量={exp}")
            n3 += 1
    dut._log.info(f"cbd2/cbd3: {n2} + {n3} 条四方一致")


@cocotb.test()
async def test_cbd_bit_groups(dut):
    """每个系数占用的比特组穷举：组内正确 + 组间不串扰

    逐比特定义与位并行写法的等价性，靠"某一组取遍全部组合、其余位取零或随机"
    来钉住 —— 掩码写错一位、移位差一格，都会在这里暴露。
    """
    rng = random.Random(20260729)
    for eta, groups, width, sig, out_sig in (
            (2, 8, 32, dut.cbd2_in, dut.cbd2_out),
            (3, 4, 24, dut.cbd3_in, dut.cbd3_out)):
        span = (1 << (2 * eta)) - 1
        for g in range(groups):
            for pattern in range(1 << (2 * eta)):
                for filler in (0, (1 << width) - 1, rng.getrandbits(width)):
                    v = (filler & ~(span << (2 * eta * g))) | (pattern << (2 * eta * g))
                    sig.value = v
                    await Timer(1, unit="ns")
                    got = unpack(int(out_sig.value), groups)
                    assert got == cbd_definition(v, eta, groups), (
                        f"cbd{eta} 组 {g} 模式 {pattern:#x} 底 {filler:#x}：{got}")
        # 系数取值范围必须是 [−η, η]
        for _ in range(2000):
            v = rng.getrandbits(width)
            sig.value = v
            await Timer(1, unit="ns")
            for c in unpack(int(out_sig.value), groups):
                assert -eta <= c <= eta, f"cbd{eta} 输出 {c} 超出 [−η, η]"
    dut._log.info("cbd2/cbd3: 比特组穷举与取值范围成立")


@cocotb.test()
async def test_rej_pair(dut):
    """取候选：向量、ref_model 与按位定义三方一致，边界值单独覆盖"""
    rows = load("rej_pair.hex")
    for v_hex, d1_hex, d2_hex, ok1, ok2 in rows:
        v = int(v_hex, 16)
        dut.rej_in.value = v
        await Timer(1, unit="ns")
        got = (int(dut.rej_d1.value), int(dut.rej_d2.value))
        exp = (int(d1_hex, 16), int(d2_hex, 16))
        model = rej_pair(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)
        assert got == exp == model, f"{v:#08x}：RTL={got} 向量={exp} 模型={model}"
        assert int(dut.rej_ok1.value) == int(ok1) == (1 if exp[0] < Q else 0)
        assert int(dut.rej_ok2.value) == int(ok2) == (1 if exp[1] < Q else 0)
    # q−1 收下、q 与 q+1 丢弃：拒绝门限差一格在这里暴露
    for target, want_ok in ((Q - 1, 1), (Q, 0), (Q + 1, 0), (0xFFF, 0)):
        dut.rej_in.value = (target & 0xFF) | ((target >> 8) << 8)
        await Timer(1, unit="ns")
        assert int(dut.rej_d1.value) == target
        assert int(dut.rej_ok1.value) == want_ok, f"候选 {target} 的接受判定不对"
    dut._log.info(f"mlkem_rej_pair: {len(rows)} 条三方一致，拒绝门限逐值验证")


async def reset(dut):
    dut.rst_n.value = 0
    dut.rej_start.value = 0
    dut.rej_valid.value = 0
    dut.rej_bytes.value = 0
    dut.rej_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


def sample_ntt_reference(stream: bytes) -> list[int]:
    """独立实现的 SampleNTT 收集逻辑，直接照 FIPS 203 Alg 7 写"""
    out = []
    pos = 0
    while pos + 3 <= len(stream) and len(out) < 256:
        d1 = stream[pos] | ((stream[pos + 1] & 0x0F) << 8)
        d2 = (stream[pos + 1] >> 4) | (stream[pos + 2] << 4)
        pos += 3
        if d1 < Q:
            out.append(d1)
        if d2 < Q and len(out) < 256:
            out.append(d2)
    return out


@cocotb.test()
async def test_rej_uniform(dut):
    """收集器：用真实 SHAKE128 流跑完整的 SampleNTT，与独立实现逐系数比对"""
    import hashlib

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for trial in range(4):
        rho = bytes([trial]) * 32
        stream = hashlib.shake_128(rho + bytes([0, trial])).digest(168 * 4)
        want = sample_ntt_reference(stream)
        assert len(want) == 256, "SHAKE 流不够长，测试自身设置有误"

        dut.rej_start.value = 1
        await RisingEdge(dut.clk)
        dut.rej_start.value = 0
        await Timer(1, unit="ns")
        assert int(dut.rej_done.value) == 0, "start 之后 done 应当已被清掉"
        assert int(dut.rej_count.value) == 0

        pos = 0
        while int(dut.rej_done.value) != 1:
            assert pos + 3 <= len(stream), "字节流耗尽而收集器仍未完成"
            dut.rej_valid.value = 1
            dut.rej_bytes.value = (stream[pos] | (stream[pos + 1] << 8)
                                   | (stream[pos + 2] << 16))
            await RisingEdge(dut.clk)
            await Timer(1, unit="ns")
            pos += 3
        dut.rej_valid.value = 0
        assert int(dut.rej_count.value) == 256

        got = []
        for i in range(256):
            dut.rej_addr.value = i
            await Timer(1, unit="ns")
            got.append(int(dut.rej_data.value))
        assert got == want, f"第 {trial} 组采样结果与独立实现不一致"
        assert all(0 <= c < Q for c in got), "采样结果越出 [0, q)"
    dut._log.info("mlkem_rej_uniform: 4 组 SHAKE128 流的 SampleNTT 与独立实现一致")


@cocotb.test()
async def test_rej_uniform_backpressure(dut):
    """输入侧空拍不应推进收集，done 也不应被空拍误触发"""
    import hashlib

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    stream = hashlib.shake_128(b"backpressure").digest(168 * 4)

    dut.rej_start.value = 1
    await RisingEdge(dut.clk)
    dut.rej_start.value = 0
    await Timer(1, unit="ns")

    # 先喂两组，再空转 20 拍：计数必须原地不动
    pos = 0
    for _ in range(2):
        dut.rej_valid.value = 1
        dut.rej_bytes.value = (stream[pos] | (stream[pos + 1] << 8)
                               | (stream[pos + 2] << 16))
        await RisingEdge(dut.clk)
        pos += 3
    dut.rej_valid.value = 0
    await Timer(1, unit="ns")
    held = int(dut.rej_count.value)
    for _ in range(20):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        assert int(dut.rej_count.value) == held, "空拍推进了计数"
        assert int(dut.rej_done.value) == 0, "空拍误触发了 done"
    assert int(dut.rej_ready.value) == 1, "未完成时 in_ready 必须为 1"
    dut._log.info("mlkem_rej_uniform: 输入空拍不推进收集")


@cocotb.test()
async def test_encode_decode12(dut):
    """12 位编解码：向量、ref_model 与按位定义三方一致，且往返还原"""
    rows = load("encode12.hex")
    for c0_hex, c1_hex, b_hex in rows:
        c0, c1 = s16(int(c0_hex, 16)), s16(int(c1_hex, 16))
        dut.enc_c0.value = c0 & 0xFFFF
        dut.enc_c1.value = c1 & 0xFFFF
        await Timer(1, unit="ns")
        got = int(dut.enc_bytes.value)
        exp = int(b_hex, 16)
        b0, b1, b2 = encode12(c0, c1)
        assert got == exp == (b0 | (b1 << 8) | (b2 << 16)), (
            f"编码 {(c0, c1)}：RTL={got:#08x} 向量={exp:#08x}")

        dut.dec_bytes.value = got
        await Timer(1, unit="ns")
        back = (int(dut.dec_c0.value), int(dut.dec_c1.value))
        assert back == (c0 % Q, c1 % Q) == decode12(b0, b1, b2), (
            f"解码往返不还原：{(c0, c1)} → {back}")
    dut._log.info(f"mlkem_encode12/decode12: {len(rows)} 条三方一致，往返还原")
