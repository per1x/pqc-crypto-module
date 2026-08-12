"""cocotb：SM3 杂凑核（GB/T 32905-2016）

  ① **GB/T 32905 附录 A**：`"abc"`（一块）与 `"abcd"×16`（64 字节，正好一块
     加一个纯填充块）两条官方向量。
  ② **填充的边界**：长度 55 / 56 / 63 / 64 / 65 字节。**56 是关键的那一个** ——
     0x80 之后剩不下 8 字节长度域，必须再补一整块。这个分支是填充里最常写错
     的地方，也是"短消息都对、某个长度突然错"这类 bug 的来源。
  ③ **空消息**与**多块长消息**。
  ④ **背压**：上游断续给字节，摘要必须逐字节不变。

另外量一遍拍数只由消息长度决定、与内容无关，以及 zeroize。
"""
import random
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from sym_oracle import SM3_ABC, SM3_ABCD16, sm3  # noqa: E402


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.in_valid.value = 0
    dut.in_data.value = 0
    dut.in_flush.value = 0
    dut.zeroize.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def hash_msg(dut, msg: bytes, feed=None):
    """喂一条消息，返回 (摘要, 拍数)"""
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 该被清掉"

    pos = 0
    ticks = 0
    flushed = False
    for t in range(2_000_000):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        push = pos < len(msg) and (feed is None or feed(t))
        # ⚠️ drive_flush 要在 pos 推进**之前**算，并且后面只能用它来判断
        # flush 有没有被吃掉。第一版写成"pos == len(msg) 就算 flush 完成"，
        # 于是吃下最后一个字节的那一轮 pos 刚好变成 len(msg)、就把 flushed
        # 置了位 —— 而那一轮 in_flush 驱的还是 0，核一次都没见过 flush，
        # 结果是所有用例一起挂在"核没有完成"。
        drive_flush = (pos == len(msg)) and not flushed
        dut.in_valid.value = 1 if push else 0
        dut.in_data.value = msg[pos] if push else 0
        dut.in_flush.value = 1 if drive_flush else 0
        await Timer(1, unit="ns")
        ready = int(dut.in_ready.value)
        if push and ready:
            pos += 1
        if drive_flush and ready:
            flushed = True
        ticks = t + 1
        if int(dut.done.value):
            break
    dut.in_valid.value = 0
    dut.in_flush.value = 0

    assert pos == len(msg), f"只吃进了 {pos} 字节，应当是 {len(msg)}"
    assert int(dut.done.value) == 1, "超时：核没有完成"
    return int(dut.digest.value).to_bytes(32, "big"), ticks


@cocotb.test()
async def test_gbt_vectors(dut):
    """GB/T 32905-2016 附录 A 的两条官方向量"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    d, _ = await hash_msg(dut, b"abc")
    assert d == SM3_ABC, f'SM3("abc") = {d.hex()}\n  应当是 {SM3_ABC.hex()}'

    d, _ = await hash_msg(dut, b"abcd" * 16)
    assert d == SM3_ABCD16, \
        f'SM3("abcd"×16) = {d.hex()}\n  应当是 {SM3_ABCD16.hex()}'

    dut._log.info("GB/T 附录 A：两条官方向量逐字节一致")


@cocotb.test()
async def test_padding_boundaries(dut):
    """填充的每一个边界长度

    **56 是关键的那一个**：0x80 写进去之后，本块只剩 7 个字节，装不下 8 字节
    的长度域，必须补满本块、压缩、再补一整块零加长度。这个分支写错的表现是
    "别的长度都对、偏偏 56~63 错"，而随手测几个短消息是碰不到的。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for n in (0, 1, 54, 55, 56, 57, 62, 63, 64, 65, 119, 120, 128, 200):
        msg = bytes((i * 7 + 3) & 0xFF for i in range(n))
        got, _ = await hash_msg(dut, msg)
        want = sm3(msg)
        assert got == want, (f"长度 {n}：{got.hex()}\n  应当是 {want.hex()}"
                             + ("  ← 就是放不下长度域的那个分支" if 56 <= n <= 63
                                else ""))

    dut._log.info("填充边界：0/1/54/55/56/57/62/63/64/65/119/120/128/200 字节全对")


@cocotb.test()
async def test_random_messages(dut):
    """随机长度、随机内容"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(20260812)
    for _ in range(10):
        n = rng.randrange(0, 300)
        msg = bytes(rng.randrange(256) for _ in range(n))
        got, _ = await hash_msg(dut, msg)
        assert got == sm3(msg), f"长度 {n} 的随机消息对不上"

    dut._log.info("随机 10 条（长度 0~299）：与参考实现逐字节一致")


@cocotb.test()
async def test_cycles_content_independent(dut):
    """拍数只由消息长度决定，与消息内容无关"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(11)
    for n in (32, 64, 100):
        seen = set()
        for _ in range(4):
            msg = bytes(rng.randrange(256) for _ in range(n))
            _, t = await hash_msg(dut, msg)
            seen.add(t)
        assert len(seen) == 1, f"长度 {n} 的消息，拍数随内容变了：{seen}"

    dut._log.info("拍数：同一长度下与消息内容无关")


@cocotb.test()
async def test_backpressure(dut):
    """上游断续给字节，摘要必须不变"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for n in (56, 64, 130):
        msg = bytes((i * 13 + 1) & 0xFF for i in range(n))
        a, _ = await hash_msg(dut, msg)
        b, _ = await hash_msg(dut, msg, feed=lambda t: (t % 3) != 0)
        assert a == b == sm3(msg), f"长度 {n}：背压下摘要变了"

    dut._log.info("背压：三拍给两拍，摘要逐字节不变")


@cocotb.test()
async def test_zeroize(dut):
    """zeroize 清掉中间态

    SM3 本身没有密钥，但在密码机里它用来算 HMAC 与 KDF，中间态里就是秘密。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    d, _ = await hash_msg(dut, b"abc")
    assert d == SM3_ABC

    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    assert int(dut.done.value) == 0, "zeroize 之后 done 该掉下来"
    assert int(dut.digest.value) == 0, "zeroize 之后摘要寄存器该清零"

    d, _ = await hash_msg(dut, b"abc")
    assert d == SM3_ABC, "zeroize 之后重新跑一条消息应当照常"

    dut._log.info("zeroize：中间态与摘要清空，清完照常工作")
