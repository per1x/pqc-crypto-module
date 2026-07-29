"""cocotb：Keccak-f[1600] 核对拍

【独立性怎么保证】
若黄金向量由本项目的参考模型生成，则"RTL == 向量 == 模型"实际只等价于
"RTL == 模型"，一份自洽但错误的实现照样能通过。因此这里使用**三个互相独立
的来源**：

  1. **官方公开向量**：全零态经一次 Keccak-f[1600] 的输出，25 个 lane 全都是
     Keccak 团队中间值文档里公开的已知量（首 lane = F1258F7940E1DDE7）。
     这是硬编码在本文件里的常量，不来自本项目的任何代码。
  2. **hashlib（完全独立的实现）**：在 RTL 核之上手工搭 SHAKE128/SHAKE256 的
     海绵结构，与 Python 标准库逐字节比对。hashlib 背后是 OpenSSL/tiny_sha3，
     与本项目毫无关系。**这一条是最强的**：它把 padding、rate、吸收/挤压、
     字节序全都验了，而不只是置换本身。
  3. hardware/model/ref_model.py 的 Python 置换（同源，只作辅助定位用）。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

# parents[2] = hardware/；黄金向量在仓库根的 vectors/（C 侧测试也读它）
HW = Path(__file__).resolve().parents[2]
REPO = HW.parent
sys.path.insert(0, str(HW / "model"))

from ref_model import keccak_f1600  # noqa: E402  （仅作辅助比对）

import hashlib  # noqa: E402

# 来源 1：Keccak 官方中间值文档公开的"全零态一次置换"结果。
# **硬编码常量，不由本项目任何代码生成** —— 这是独立性的第一道来源。
ALL_ZERO_PERMUTED = [
    0xF1258F7940E1DDE7, 0x84D5CCF933C0478A, 0xD598261EA65AA9EE,
    0xBD1547306F80494D, 0x8B284E056253D057, 0xFF97A42D7F8E6FD4,
    0x90FEE5A0A44647C4, 0x8C5BDA0CD6192E76, 0xAD30A6F71B19059C,
    0x30935AB7D08FFC64, 0xEB5AA93F2317D635, 0xA9A6E6260D712103,
    0x81A57C16DBCF555F, 0x43B831CD0347C826, 0x01F22F1A11A5569F,
    0x05E5635A21D9AE61, 0x64BEFEF28CC970F2, 0x613670957BC46611,
    0xB87C5A554FD00ECB, 0x8C3EE88A1CCF32C8, 0x940C7922AE3A2614,
    0x1841F924A2C509E4, 0x16F53526E70465C2, 0x75F644E97F30A13B,
    0xEAF1FF7B5CECA249,
]


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.wr_en.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_state(dut, lanes):
    for i, v in enumerate(lanes):
        dut.wr_en.value = 1
        dut.wr_addr.value = i
        dut.wr_data.value = v & ((1 << 64) - 1)
        await RisingEdge(dut.clk)
    dut.wr_en.value = 0
    await RisingEdge(dut.clk)


async def permute(dut) -> int:
    """跑一次置换，返回用了多少 cycle。

    done 是电平语义，所以要先确认它被这次 start 清掉，再等它重新拉高
    （与 ntt_core 同一套握手，理由见 test_ntt_core.py）。
    """
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"
    cycles = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        cycles += 1
        assert cycles < 200, "Keccak 置换超时"
    return cycles


async def read_state(dut):
    out = []
    for i in range(25):
        dut.rd_addr.value = i
        await Timer(1, unit="ns")
        out.append(int(dut.rd_data.value))
    return out


@cocotb.test()
async def test_official_all_zero_vector(dut):
    """★ 独立预言机 1：全零态置换 == Keccak 官方公开向量"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    await load_state(dut, [0] * 25)
    cycles = await permute(dut)
    got = await read_state(dut)
    for i, (g, w) in enumerate(zip(got, ALL_ZERO_PERMUTED)):
        assert g == w, f"lane {i}: RTL={g:016X} 官方={w:016X}"
    assert cycles == 24, f"单轮迭代应当正好 24 cycle，实测 {cycles}"
    dut._log.info(f"全零态置换与官方向量 25 个 lane 全部相符，{cycles} cycles")


@cocotb.test()
async def test_random_vs_model(dut):
    """随机态与 Python 模型比对（辅助定位用，非独立来源）"""
    import random
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    rng = random.Random(20260729)
    for t in range(12):
        st = [rng.getrandbits(64) for _ in range(25)]
        await load_state(dut, st)
        await permute(dut)
        got = await read_state(dut)
        want = keccak_f1600(list(st))
        assert got == want, f"第 {t} 组与模型不符"
    dut._log.info("12 组随机态与 Python 模型一致")


@cocotb.test()
async def test_double_permute(dut):
    """连续两次置换：等价于对同一状态做两轮 —— 验证核可以反复使用"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    await load_state(dut, [0] * 25)
    await permute(dut)
    first = await read_state(dut)
    assert first == ALL_ZERO_PERMUTED
    # 不重新写状态，直接再置换一次
    await permute(dut)
    second = await read_state(dut)
    assert second == keccak_f1600(list(first)), "第二次置换结果不对"
    assert second != first
    dut._log.info("连续置换正确（核可反复使用，状态在核内保持）")


@cocotb.test()
async def test_done_is_level(dut):
    """done 必须是电平：保持到下一次 start 才清（accel.h 的轮询契约）"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.done.value) == 0, "复位后 done 必须为 0"
    await load_state(dut, [0] * 25)
    await permute(dut)
    assert int(dut.done.value) == 1
    for _ in range(40):
        await RisingEdge(dut.clk)
        assert int(dut.done.value) == 1, "done 掉了 —— 应保持到下一次 start"
    await load_state(dut, [1] * 25)
    assert int(dut.done.value) == 1, "写状态不该清 done"
    dut._log.info("done 为电平语义")


# ---------------------------------------------------------------------------
# ★ 独立预言机 2：在 RTL 核之上搭海绵，与 hashlib 逐字节比对
# ---------------------------------------------------------------------------

async def rtl_shake(dut, msg: bytes, rate: int, suffix: int, outlen: int) -> bytes:
    """完全用 RTL 核做置换，手工实现 SHAKE 的吸收/挤压。

    这条路径验的不只是置换：padding（pad10*1 + 域分隔后缀）、rate、
    lane 的小端字节序、多块吸收与多块挤压，全都在内。
    """
    state = [0] * 25
    pad = bytearray(msg)
    pad.append(suffix)
    while len(pad) % rate:
        pad.append(0)
    pad[-1] ^= 0x80

    for off in range(0, len(pad), rate):
        blk = pad[off:off + rate]
        for i in range(rate // 8):
            lane = int.from_bytes(blk[i * 8:i * 8 + 8], "little")
            state[i] ^= lane
        await load_state(dut, state)
        await permute(dut)
        state = await read_state(dut)

    out = bytearray()
    while len(out) < outlen:
        # 每轮挤压吐出**整整 rate 字节**（rate 恰好是 8 的整数倍：168=21 lane，136=17 lane）。
        # 注意不能按累计长度提前 break，否则第二块起只会吐出一个 lane；
        # 单块输出（outlen <= rate）时这个错误不可见，只有多块挤压才会暴露。
        out += b"".join(state[i].to_bytes(8, "little") for i in range(rate // 8))
        if len(out) < outlen:
            await load_state(dut, state)
            await permute(dut)
            state = await read_state(dut)
    return bytes(out[:outlen])


@cocotb.test()
async def test_shake_against_hashlib(dut):
    """★ 独立预言机 2：RTL 核搭出来的 SHAKE == hashlib（完全独立的实现）

    这是最强的一条：hashlib 背后是 OpenSSL/tiny_sha3，与本项目毫无关系。
    覆盖跨块边界的长度（rate-1 / rate / rate+1）与多块挤压。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    cases = [
        (b"", 32),
        (b"abc", 32),
        (bytes(range(200)), 64),
    ]
    for msg, outlen in cases:
        # SHAKE-128：rate = 168，域分隔后缀 0x1F
        got = await rtl_shake(dut, msg, 168, 0x1F, outlen)
        want = hashlib.shake_128(msg).digest(outlen)
        assert got == want, (f"SHAKE128(len={len(msg)}, out={outlen}) 不符\n"
                             f"  RTL     ={got.hex()}\n  hashlib ={want.hex()}")
        # SHAKE-256：rate = 136，后缀同样是 0x1F
        got = await rtl_shake(dut, msg, 136, 0x1F, outlen)
        want = hashlib.shake_256(msg).digest(outlen)
        assert got == want, f"SHAKE256(len={len(msg)}, out={outlen}) 不符"

    # 跨块边界：rate-1 / rate / rate+1
    for mlen in (167, 168, 169):
        msg = bytes((i * 7 + 3) & 0xFF for i in range(mlen))
        got = await rtl_shake(dut, msg, 168, 0x1F, 32)
        want = hashlib.shake_128(msg).digest(32)
        assert got == want, f"SHAKE128 在 msg_len={mlen}（块边界）处不符"

    # 多块挤压：要求 200 字节 > rate
    msg = b"squeeze across blocks"
    got = await rtl_shake(dut, msg, 168, 0x1F, 200)
    want = hashlib.shake_128(msg).digest(200)
    assert got == want, "SHAKE128 多块挤压不符"

    # SHA3-256：rate = 136，域分隔后缀 0x06（与 SHAKE 不同，验的是域分隔）
    for msg in (b"", b"abc", bytes(range(150))):
        got = await rtl_shake(dut, msg, 136, 0x06, 32)
        want = hashlib.sha3_256(msg).digest()
        assert got == want, f"SHA3-256(len={len(msg)}) 不符"

    dut._log.info("RTL 核搭出的 SHAKE128/SHAKE256/SHA3-256 与 hashlib 逐字节一致")
