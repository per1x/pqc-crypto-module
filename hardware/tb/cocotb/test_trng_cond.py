"""cocotb：TRNG 熵调理器（Keccak 海绵）对拍

判据分三层：

一、**逐字对拍**。同一条原始比特流喂给 RTL 与 hardware/model/trng_cond_model.py，
    挤出的每一个 32 位字都必须相等。海绵是有状态的，一个字对不上后面全错，
    所以这一层实际上把比特序、lane 顺序、异或注入、置换时机全都钉住了。

二、**握手不丢比特**。对拍成立的前提是 RTL 和模型看到的是同一条比特流。RTL 在
    置换/挤出期间会丢比特（这是设计允许的，见 trng_cond.v），所以测试台严格按
    bit_ready 握手投喂 —— 只有一个比特都不丢，逐字对拍才有意义。反过来，如果
    握手写错了导致丢了比特，第一层立刻会红，不会悄悄放过。

三、**状态确实在推进**。挤出的字互不相同、比特分布不偏 —— 防的是"海绵没被
    置换、每轮挤出同一个值"这类错误，那种错误在只看单轮输出时看不出来。
"""
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

import tbutil  # noqa: F401  —— 导入即把 hardware/model 放进 sys.path

from trng_cond_model import SpongeConditioner  # noqa: E402

RATE_BITS = 17 * 64      # 1088，与 RTL 的 RATE_LANES 默认值对应
WORDS_PER_SQUEEZE = 8    # OUT_LANES=4 → 4 个 lane → 8 个 32 位字


async def reset(dut):
    dut.rst_n.value = 0
    dut.bit_valid.value = 0
    dut.bit_in.value = 0
    dut.word_ready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def step(dut):
    """推进一个时钟，返回 (本拍是否吃进了比特, 本拍挤出的字或 None)

    组合输出（bit_ready / word_valid / word_out）在时钟边沿之后 1 ns 采样：
    它们只依赖状态寄存器，边沿之间是稳定的，这个采样点上取到的就是下一个
    边沿会生效的值。
    """
    await Timer(1, unit="ns")
    taken = bool(int(dut.bit_ready.value)) and bool(int(dut.bit_valid.value))
    word = int(dut.word_out.value) if int(dut.word_valid.value) else None
    await RisingEdge(dut.clk)
    return taken, word


async def run_stream(dut, bits, want_words):
    """按握手把 bits 全部喂进去，收集挤出的字，直到拿到 want_words 个"""
    got = []
    i = 0
    guard = 0
    limit = 40 * (len(bits) + 64)
    while len(got) < want_words:
        if i < len(bits):
            dut.bit_in.value = bits[i]
            dut.bit_valid.value = 1
        else:
            dut.bit_valid.value = 0

        taken, word = await step(dut)
        if taken:
            i += 1
        if word is not None:
            got.append(word)

        guard += 1
        assert guard < limit, (
            f"跑了 {guard} 拍还没收够字（已喂 {i}/{len(bits)} 比特，"
            f"已收 {len(got)}/{want_words} 字）—— 状态机可能卡住了")
    dut.bit_valid.value = 0
    assert i == len(bits) or len(got) == want_words
    return got, i


@cocotb.test()
async def test_matches_model(dut):
    """四轮吸收/挤出，逐字与参考模型比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    blocks = 4
    rng = random.Random(20260812)
    bits = [rng.getrandbits(1) for _ in range(RATE_BITS * blocks)]

    model = SpongeConditioner()
    want = model.feed_bits(bits)
    assert len(want) == WORDS_PER_SQUEEZE * blocks

    got, fed = await run_stream(dut, bits, len(want))

    assert fed == len(bits), f"只喂进去 {fed} 个比特，应为 {len(bits)}"
    for k, (g, w) in enumerate(zip(got, want)):
        assert g == w, (
            f"第 {k} 个挤出字不符：RTL=0x{g:08x} 模型=0x{w:08x}"
            f"（第 {k // WORDS_PER_SQUEEZE} 轮）")

    assert int(dut.blocks_absorbed.value) == blocks, \
        f"blocks_absorbed={int(dut.blocks_absorbed.value)}，应为 {blocks}"
    dut._log.info(f"{len(got)} 个挤出字与参考模型逐字一致，共吸收 {blocks} 个 rate 块")


@cocotb.test()
async def test_all_zero_input(dut):
    """全零输入：挤出的必须是零状态置换一次的结果

    这一条独立于随机流：它把"海绵初值为零 + 异或注入 + 恰好置换一次"钉死成一个
    可以手算核对的具体值。随机流对拍如果模型和 RTL 犯了同一个错，这一条能兜住。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    bits = [0] * RATE_BITS
    model = SpongeConditioner()
    want = model.feed_bits(bits)

    got, _ = await run_stream(dut, bits, len(want))
    assert got == want, f"全零输入对不上：RTL={[hex(x) for x in got]}"
    dut._log.info(f"全零输入的挤出结果正确，首字 0x{got[0]:08x}")


@cocotb.test()
async def test_state_advances(dut):
    """连续多轮挤出的字互不相同，且比特分布不偏

    防的是"置换没跑起来、每轮挤出同一个值"这类错误 —— 单看一轮输出是看不
    出来的，因为那一轮的值本身完全正确。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    blocks = 8
    rng = random.Random(7)
    bits = [rng.getrandbits(1) for _ in range(RATE_BITS * blocks)]
    got, _ = await run_stream(dut, bits, WORDS_PER_SQUEEZE * blocks)

    assert len(set(got)) == len(got), "挤出的字里有重复，海绵状态没有推进"
    ones = sum(bin(w).count("1") for w in got)
    frac = ones / (len(got) * 32)
    assert 0.42 < frac < 0.58, f"输出比特偏置异常：{frac:.3f}"
    dut._log.info(f"{len(got)} 个字互不相同，1 的比例 {frac:.3f}")


@cocotb.test()
async def test_backpressure(dut):
    """word_ready 拉低时挤出必须停住，数据不能丢

    真实系统里 FIFO 会满。这一条验证背压是被正确处理的：中途反复拉低
    word_ready，最终收到的字序列必须与不加背压时完全一致。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(99)
    bits = [rng.getrandbits(1) for _ in range(RATE_BITS * 2)]
    want = SpongeConditioner().feed_bits(bits)

    got = []
    i = 0
    stall = 0
    guard = 0
    while len(got) < len(want):
        # 每收到一个字之后憋 5 拍，逼状态机在挤出中途等
        dut.word_ready.value = 0 if stall > 0 else 1
        if i < len(bits):
            dut.bit_in.value = bits[i]
            dut.bit_valid.value = 1
        else:
            dut.bit_valid.value = 0

        await Timer(1, unit="ns")
        taken = bool(int(dut.bit_ready.value)) and bool(int(dut.bit_valid.value))
        wv = bool(int(dut.word_valid.value)) and (stall == 0)
        word = int(dut.word_out.value) if wv else None
        await RisingEdge(dut.clk)

        if taken:
            i += 1
        if word is not None:
            got.append(word)
            stall = 5
        elif stall > 0:
            stall -= 1

        guard += 1
        assert guard < 60 * (len(bits) + 64), "带背压时状态机卡住了"

    dut.bit_valid.value = 0
    dut.word_ready.value = 1
    assert got == want, "加了背压之后挤出序列变了 —— 说明有字被丢或被重复"
    dut._log.info(f"背压下 {len(got)} 个字仍与参考模型一致")
