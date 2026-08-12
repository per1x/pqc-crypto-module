"""sha3_core 对拍 —— 黄金模型直接用 Python 的 hashlib。

hashlib 的 sha3_256/sha3_512/shake_128/shake_256 就是 FIPS 202 本体，
拿它当参考比自己写一份海绵靠谱得多：自己写的模型和 RTL 可能一起错
（同一个人对同一份规范的同一处误读会在两边同时出现），而 hashlib 是
独立实现、被无数人验证过的。这是"黄金模型要独立于被测物"的实际落点。

覆盖：四个参数集 × 一组消息长度，重点压在**边界长度**上 ——
rate-1（suffix 恰好落在 rate-1，与 0x80 合并成同一个字节）、
rate（消息正好填满一块，pad 要另起一块）、rate+1、以及空消息。
padding 的错误几乎全部藏在这几个长度里，中间长度反而最不容易出错。
"""

import hashlib
import os

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

CLK_NS = 10

# (名字, rate 字节数, 域分隔后缀)
PARAMS = {
    "shake128": (168, 0x1F),
    "shake256": (136, 0x1F),
    "sha3-256": (136, 0x06),
    "sha3-512": (72, 0x06),
}

# SHA3 是定长摘要，SHAKE 是任意长度。
#
# 这条区别在硬件里**不存在** —— sha3_core 是个裸海绵，你要多少它挤多少，
# 它压根不知道"SHA3-256 只有 32 字节"这回事。定长是调用方的约定：
# 数够 32 个字节就停。所以这张表属于测试台，不属于 RTL。
DIGEST_LEN = {"sha3-256": 32, "sha3-512": 64}


def golden(name: str, msg: bytes, outlen: int) -> bytes:
    if name == "shake128":
        return hashlib.shake_128(msg).digest(outlen)
    if name == "shake256":
        return hashlib.shake_256(msg).digest(outlen)
    if name == "sha3-256":
        return hashlib.sha3_256(msg).digest()[:outlen]
    if name == "sha3-512":
        return hashlib.sha3_512(msg).digest()[:outlen]
    raise ValueError(name)


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.zeroize.value = 0
    dut.in_valid.value = 0
    dut.in_data.value = 0
    dut.in_flush.value = 0
    dut.out_ready.value = 0
    dut.rate_bytes.value = 136
    dut.suffix.value = 0x1F
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def wait_in_ready(dut, what: str):
    """等到核心能收字节。

    **一定要带上限。** 第一版这里写的是裸 while True，核心一旦停住，
    仿真就闷头跑了一千六百万个周期直到被外面杀掉，什么信息都没留下 ——
    而真正的 bug（start 只在空闲时才认，挤压之后回不去空闲）三行就能看出来。
    带上限的等待把"挂死"变成"一条指出位置的失败信息"。
    """
    for _ in range(20000):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        if dut.in_ready.value == 1:
            return
    raise AssertionError(f"等 in_ready 超时（{what}）：核心卡在 state={int(dut.state.value)}")


async def run_hash(dut, name: str, msg: bytes, outlen: int, stall=0) -> bytes:
    """跑一次完整的 吸收→pad→挤压，返回 outlen 个字节。

    stall>0 时在喂字节之间插入空拍，用来验证背压：in_ready 为高才递字节，
    核心不该因为供数断续而算错。
    """
    rate, suffix = PARAMS[name]

    dut.rate_bytes.value = rate
    dut.suffix.value = suffix
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0

    # ---- 吸收 ----
    for i, b in enumerate(msg):
        # 等到核心能收（置换期间 in_ready 会拉低）
        await wait_in_ready(dut, f"{name} 第 {i} 字节")
        dut.in_valid.value = 1
        dut.in_data.value = b
        await RisingEdge(dut.clk)
        dut.in_valid.value = 0
        dut.in_data.value = 0
        for _ in range(stall):
            await RisingEdge(dut.clk)

    # ---- 结束消息 ----
    # in_flush 只在 in_valid 为低时被采样，上面每个字节之后都拉低了
    await wait_in_ready(dut, f"{name} flush 前")
    dut.in_flush.value = 1
    await RisingEdge(dut.clk)
    dut.in_flush.value = 0

    # ---- 挤压 ----
    out = bytearray()
    guard = 0
    while len(out) < outlen:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        guard += 1
        assert guard < 200000, f"挤压超时，只拿到 {len(out)}/{outlen} 字节"
        if dut.out_valid.value != 1:
            continue
        out.append(int(dut.out_data.value))
        dut.out_ready.value = 1
        await RisingEdge(dut.clk)
        dut.out_ready.value = 0
    return bytes(out)


@cocotb.test()
async def test_empty_message(dut):
    """空消息。FIPS 202 的第一条向量，也是最容易被实现漏掉的一条 ——
    很多实现的"最后一块"逻辑默认至少来过一个字节。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    for name in PARAMS:
        got = await run_hash(dut, name, b"", 32)
        exp = golden(name, b"", 32)
        assert got == exp, f"{name} 空消息：{got.hex()} != {exp.hex()}"


@cocotb.test()
async def test_boundary_lengths(dut):
    """边界长度：padding 的 bug 基本全藏在这里。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    for name, (rate, _suffix) in PARAMS.items():
        for n in (1, 8, rate - 2, rate - 1, rate, rate + 1, 2 * rate):
            msg = bytes((i * 37 + n) & 0xFF for i in range(n))
            got = await run_hash(dut, name, msg, 32)
            exp = golden(name, msg, 32)
            assert got == exp, (
                f"{name} 消息长 {n}（rate={rate}）：{got.hex()} != {exp.hex()}"
            )


@cocotb.test()
async def test_long_squeeze(dut):
    """挤压跨多个 rate 块 —— 验证挤压中途的置换接得上。
    SHAKE 的输出长度是任意的，这条路在 ML-KEM 的 ρ 展开里天天走。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    msg = b"pqc-hsm zu3eg"
    for name, (rate, _s) in PARAMS.items():
        outlen = 2 * rate + 5     # 跨三块，且不对齐
        if name.startswith("sha3"):
            continue              # SHA3 是定长的，不做任意长度挤压
        got = await run_hash(dut, name, msg, outlen)
        exp = golden(name, msg, outlen)
        assert got == exp, f"{name} 长挤压：{got.hex()} != {exp.hex()}"


@cocotb.test()
async def test_backpressure(dut):
    """供数断续不该影响结果。真系统里 PS 侧几乎不可能每拍都给一个字节。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    msg = bytes(range(200))
    got = await run_hash(dut, "shake256", msg, 64, stall=3)
    exp = golden("shake256", msg, 64)
    assert got == exp, f"背压下算错：{got.hex()} != {exp.hex()}"


@cocotb.test()
async def test_back_to_back(dut):
    """连着算两次。第二次必须从干净的海绵开始 ——
    start 若没真正清空 25 个 lane，第一次的残留会污染第二次，
    而单跑一次是发现不了的。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    a = await run_hash(dut, "sha3-256", b"first message", 32)
    assert a == golden("sha3-256", b"first message", 32)

    b = await run_hash(dut, "sha3-256", b"second", 32)
    assert b == golden("sha3-256", b"second", 32), "第二次算错：海绵没清干净"

    # 再算一次第一条，应与第一次完全一致
    c = await run_hash(dut, "sha3-256", b"first message", 32)
    assert c == a, "同样的输入两次结果不同"


@cocotb.test()
async def test_zeroize_clears_sponge(dut):
    """zeroize 要真把 25 个 lane 写成 0，不是只把状态机拨回空闲。

    这里特意在**置换途中**触发 —— keccak_f1600 在 busy 期间会默默丢弃写入，
    如果 zeroize 不等置换落地就去写 0，会全部落空，状态机却照样走完，
    表面清干净了，海绵里的消息原封不动。
    """
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    rate, suffix = PARAMS["shake256"]
    dut.rate_bytes.value = rate
    dut.suffix.value = suffix
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0

    # 灌满一整个 rate 块，逼它进入置换
    for i in range(rate):
        await wait_in_ready(dut, f"zeroize 测试第 {i} 字节")
        dut.in_valid.value = 1
        dut.in_data.value = 0xA5
        await RisingEdge(dut.clk)
        dut.in_valid.value = 0

    # 此刻大概率正在置换（in_ready 为低）。就在这时候擦。
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    was_permuting = dut.in_ready.value == 0
    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0

    # 等它回到空闲
    guard = 0
    while True:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        guard += 1
        assert guard < 1000, "zeroize 之后没能回到空闲"
        if dut.busy.value == 0:
            break

    dut._log.info(f"擦除时是否正在置换：{was_permuting}")

    # 擦干净的证据：重新算一条已知消息，结果必须与黄金模型一致。
    # 若上面那 136 个 0xA5 还留在海绵里，这里必然对不上。
    got = await run_hash(dut, "shake256", b"after zeroize", 32)
    exp = golden("shake256", b"after zeroize", 32)
    assert got == exp, f"zeroize 之后海绵不干净：{got.hex()} != {exp.hex()}"


@cocotb.test()
async def test_random_vectors(dut):
    """随机长度扫一遍，兜住上面几条没想到的组合。"""
    cocotb.start_soon(Clock(dut.clk, CLK_NS, unit="ns").start())
    await reset(dut)

    rng = __import__("random").Random(0xC0FFEE)   # 固定种子，失败可复现
    for _ in range(12):
        name = rng.choice(list(PARAMS))
        n = rng.randint(0, 300)
        msg = bytes(rng.getrandbits(8) for _ in range(n))
        outlen = rng.choice([16, 32, 48, 64])
        # SHA3 定长：超出摘要长度就没有参考值可比了（hashlib 也给不出来）
        outlen = min(outlen, DIGEST_LEN.get(name, outlen))
        got = await run_hash(dut, name, msg, outlen)
        exp = golden(name, msg, outlen)
        assert got == exp, f"{name} len={n} out={outlen}：{got.hex()} != {exp.hex()}"
