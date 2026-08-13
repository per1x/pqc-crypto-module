"""cocotb：key_vault 本体 —— tamper 与使用口的**同拍**关系

test_key_vault.py 测的是 key_vault_axi（总线视角：密钥读不回来）。
这一份测的是模块本体，只为一件事：**tamper 拉高的那一拍，使用口还给不给密钥。**

使用口（use_key / use_valid）不出芯片，接的是 PL 内部的 AES/SM4 核。它是组合的：
    use_valid = valid_map[use_sel] && !tamper_latched
而 tamper_latched 要到下一个时钟沿才变 1。于是 tamper 拉高的那一拍：

    · 槽位还没被清（清除也在下一个沿）；
    · use_valid 仍然是 1，整把 256 位密钥仍然摆在 use_key 上；
    · 正在取密钥的算法核这一拍就把它锁进去了。

篡改检测与算法核取密钥是两条独立的时间线，"恰好同拍"是可以凑出来的，
不是要等运气。判据用 (tamper || tamper_latched)，窗口宽度归零。

这条用例是**纯组合**的：把 tamper 抬起来，不推时钟，当场读 use_valid / use_key。
不涉及任何采样时机的争议 —— 要么这一拍给了，要么没给。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

WORDS = 8
KEY = [0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF,
       0x0F1E2D3C, 0x4B5A6978, 0x8796A5B4, 0xC3D2E1F0]
# 字序：先写进来的字在最高位（与 key_vault.v 的注释一致）
KEY_FLAT = int("".join(f"{w:08x}" for w in KEY), 16)


async def reset(dut):
    dut.rst_n.value = 0
    dut.ld_slot.value = 0
    dut.ld_begin.value = 0
    dut.ld_we.value = 0
    dut.ld_wdata.value = 0
    dut.ld_commit.value = 0
    dut.ld_lock.value = 0
    dut.ld_erase.value = 0
    dut.zeroize.value = 0
    dut.tamper.value = 0
    dut.use_sel.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def pulse(dut, name, **kw):
    for k, v in kw.items():
        getattr(dut, k).value = v
    getattr(dut, name).value = 1
    await RisingEdge(dut.clk)
    getattr(dut, name).value = 0
    await RisingEdge(dut.clk)


async def load_key(dut, slot):
    dut.ld_slot.value = slot
    await pulse(dut, "ld_begin")
    for w in KEY:
        await pulse(dut, "ld_we", ld_wdata=w)
    await pulse(dut, "ld_commit")


async def setup(dut):
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)


@cocotb.test()
async def test_use_port_serves_loaded_key(dut):
    """正例：装好的槽，使用口给出整把密钥

    先立这条 —— 否则后面"tamper 之后拿不到"可能只是因为本来就没装进去。
    """
    await setup(dut)
    await load_key(dut, 3)
    dut.use_sel.value = 3
    await Timer(1, unit="ns")

    assert int(dut.use_valid.value) == 1, "装好的槽 use_valid 是 0"
    assert int(dut.use_key.value) == KEY_FLAT, (
        f"use_key = 0x{int(dut.use_key.value):064x}，"
        f"应当是 0x{KEY_FLAT:064x}")

    # 别的槽是空的
    dut.use_sel.value = 4
    await Timer(1, unit="ns")
    assert int(dut.use_valid.value) == 0, "空槽 use_valid 竟然是 1"

    dut._log.info("使用口正确给出整把 256 位密钥；空槽无效")


@cocotb.test()
async def test_tamper_kills_use_port_in_same_cycle(dut):
    """关键档：tamper 拉高的**同一拍**，使用口必须立刻关

    不推时钟 —— 只把 tamper 抬起来然后当场读。
    旧 RTL 在这里 use_valid 仍是 1、use_key 仍是整把密钥。
    """
    await setup(dut)
    await load_key(dut, 2)
    dut.use_sel.value = 2
    await Timer(1, unit="ns")
    assert int(dut.use_valid.value) == 1 and int(dut.use_key.value) == KEY_FLAT

    # 同一拍：tamper 抬起来，时钟一个沿都不推
    dut.tamper.value = 1
    await Timer(1, unit="ns")

    assert int(dut.use_valid.value) == 0, (
        "tamper 拉高的同一拍 use_valid 仍然是 1 —— "
        "算法核在这一拍就能把密钥锁进去")
    got = int(dut.use_key.value)
    assert got == 0, (
        f"tamper 拉高的同一拍 use_key 仍然是 0x{got:064x} —— "
        "整把密钥还摆在线上")

    # 沿过去之后槽位真的被清了（锁存那一档，旧 RTL 也对，留作对照）
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    assert int(dut.valid_map.value) == 0, "tamper 之后 valid_map 没清"
    assert int(dut.tamper_latched.value) == 1, "tamper 没锁存"

    dut._log.info("tamper 同拍即关闭使用口：use_valid=0、use_key=0")


@cocotb.test()
async def test_tamper_same_cycle_blocks_load(dut):
    """同拍的装载也要被拒：tamper 与 ld_we 同一拍"""
    await setup(dut)
    dut.ld_slot.value = 5
    await pulse(dut, "ld_begin")

    # tamper 与 ld_we 同拍
    dut.tamper.value = 1
    dut.ld_wdata.value = 0xA5A5A5A5
    dut.ld_we.value = 1
    await RisingEdge(dut.clk)
    dut.ld_we.value = 0
    dut.tamper.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    dut.use_sel.value = 5
    await Timer(1, unit="ns")
    assert int(dut.use_valid.value) == 0
    assert int(dut.use_key.value) == 0, "同拍写进去的字还在"
    assert int(dut.sel_fill.value) == 0, (
        f"sel_fill = {int(dut.sel_fill.value)} —— "
        "tamper 同拍的那个字被收下了")

    dut._log.info("tamper 同拍的装载被拒，一个字都没进去")


@cocotb.test()
async def test_tamper_erases_all_slots(dut):
    """tamper 之后全部槽位清零、锁定也清、此后一切装载被拒（回归）"""
    await setup(dut)
    for slot in range(4):
        await load_key(dut, slot)
    assert int(dut.valid_map.value) == 0b1111

    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    assert int(dut.valid_map.value) == 0, "tamper 之后还有槽是 valid"
    assert int(dut.lock_map.value) == 0, "tamper 之后锁没清"
    for slot in range(4):
        dut.use_sel.value = slot
        await Timer(1, unit="ns")
        assert int(dut.use_key.value) == 0, f"槽 {slot} 还有密钥"

    # 之后一律拒绝装载
    dut.ld_slot.value = 0
    await pulse(dut, "ld_begin")
    await pulse(dut, "ld_we", ld_wdata=0x11223344)
    await pulse(dut, "ld_commit")
    dut.use_sel.value = 0
    await Timer(1, unit="ns")
    assert int(dut.use_valid.value) == 0, "tamper 之后还能装进新密钥"

    dut._log.info("tamper 清空全部槽位与锁，此后装载一律被拒")
