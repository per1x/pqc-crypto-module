"""cocotb：AXI4-Lite 控制面与 AXI4-Stream 数据面对拍

验证两件事：

一、**寄存器映射契约**。accel.h 里写死的那几条语义必须在总线上真的成立 ——
    START 自清、DONE 电平锁存且只由下一次 START 清除、STATUS/OUT_LEN/ERRCODE
    软件只读、VERSION 为常量、字节选通生效、未映射地址不报错。
    这些条目逐条对应一个测试，写错一条就有一条失败。

二、**数据面**。把 Keccak-f[1600] 与 NTT 两条命令完整跑通：AXI4-Stream 送入、
    AXI4-Lite 触发、轮询 DONE、AXI4-Stream 取回，结果与 Python 参考模型逐字节
    比对。握手的空拍（TVALID/TREADY 断续）单独覆盖，因为"只在满速下正确"的
    流接口在真实系统里必然出问题。

BFM 是手写的：不依赖任何第三方 AXI 库，与 RTL 一样保持零外部依赖。
"""
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ReadOnly, RisingEdge

from tbutil import s16

from ref_model import keccak_f1600, ntt  # noqa: E402

REG_CTRL    = 0x00
REG_STATUS  = 0x04
REG_MODE    = 0x08
REG_PARAM   = 0x0C
REG_IN_LEN  = 0x10
REG_OUT_LEN = 0x14
REG_ERRCODE = 0x18
REG_VERSION = 0x1C

CTRL_START      = 1 << 0
CTRL_SOFT_RESET = 1 << 1

ST_DONE = 1 << 0
ST_BUSY = 1 << 1
ST_ERR  = 1 << 2

MODE_NTT_FWD = 7
MODE_NTT_INV = 8
MODE_KECCAK  = 9

VERSION_CONST = 0x0001_0000


# ---------------------------------------------------------------- BFM

async def axil_write(dut, addr, data, strb=0xF):
    """AXI4-Lite 写一拍：AW / W 同时发起，等 B 响应"""
    dut.s_axi_awaddr.value = addr
    dut.s_axi_awvalid.value = 1
    dut.s_axi_wdata.value = data
    dut.s_axi_wstrb.value = strb
    dut.s_axi_wvalid.value = 1
    dut.s_axi_bready.value = 1

    aw_done = w_done = False
    resp = None
    for _ in range(200):
        await ReadOnly()
        aw = (not aw_done) and int(dut.s_axi_awready.value)
        w = (not w_done) and int(dut.s_axi_wready.value)
        b = int(dut.s_axi_bvalid.value)
        bresp = int(dut.s_axi_bresp.value)
        await RisingEdge(dut.clk)
        if aw:
            aw_done = True
            dut.s_axi_awvalid.value = 0
        if w:
            w_done = True
            dut.s_axi_wvalid.value = 0
        if b:
            resp = bresp
            break
    dut.s_axi_bready.value = 0
    assert resp is not None, f"写 {addr:#04x} 超时"
    return resp


async def axil_read(dut, addr):
    """AXI4-Lite 读一拍，返回 (data, resp)"""
    dut.s_axi_araddr.value = addr
    dut.s_axi_arvalid.value = 1
    dut.s_axi_rready.value = 1

    ar_done = False
    for _ in range(200):
        await ReadOnly()
        ar = (not ar_done) and int(dut.s_axi_arready.value)
        rv = int(dut.s_axi_rvalid.value)
        rdata = int(dut.s_axi_rdata.value)
        rresp = int(dut.s_axi_rresp.value)
        await RisingEdge(dut.clk)
        if ar:
            ar_done = True
            dut.s_axi_arvalid.value = 0
        if rv and ar_done:
            dut.s_axi_rready.value = 0
            return rdata, rresp
    raise AssertionError(f"读 {addr:#04x} 超时")


async def axis_send(dut, words, gap=0):
    """AXI4-Stream 送一个包，最后一拍带 TLAST"""
    for i, w in enumerate(words):
        dut.s_axis_tdata.value = w
        dut.s_axis_tvalid.value = 1
        dut.s_axis_tlast.value = 1 if i == len(words) - 1 else 0
        for _ in range(200):
            await ReadOnly()
            ready = int(dut.s_axis_tready.value)
            await RisingEdge(dut.clk)
            if ready:
                break
        else:
            raise AssertionError("输入流握手超时")
        if gap:
            dut.s_axis_tvalid.value = 0
            for _ in range(gap):
                await RisingEdge(dut.clk)
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tlast.value = 0


async def axis_recv(dut, gap=0, limit=4096):
    """AXI4-Stream 收一个包，直到 TLAST"""
    out = []
    dut.m_axis_tready.value = 1
    for _ in range(limit):
        await ReadOnly()
        valid = int(dut.m_axis_tvalid.value)
        data = int(dut.m_axis_tdata.value)
        last = int(dut.m_axis_tlast.value)
        await RisingEdge(dut.clk)
        if valid:
            out.append(data)
            if last:
                break
            if gap:
                dut.m_axis_tready.value = 0
                for _ in range(gap):
                    await RisingEdge(dut.clk)
                dut.m_axis_tready.value = 1
    else:
        raise AssertionError("输出流未在限定拍数内给出 TLAST")
    dut.m_axis_tready.value = 0
    return out


async def reset(dut):
    dut.rst_n.value = 0
    dut.s_axi_awaddr.value = 0
    dut.s_axi_awvalid.value = 0
    dut.s_axi_wdata.value = 0
    dut.s_axi_wstrb.value = 0
    dut.s_axi_wvalid.value = 0
    dut.s_axi_bready.value = 0
    dut.s_axi_araddr.value = 0
    dut.s_axi_arvalid.value = 0
    dut.s_axi_rready.value = 0
    dut.s_axis_tdata.value = 0
    dut.s_axis_tvalid.value = 0
    dut.s_axis_tlast.value = 0
    dut.m_axis_tready.value = 0
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def poll_done(dut, limit=4000):
    """按软件的方式轮询 STATUS.DONE"""
    for _ in range(limit):
        st, _ = await axil_read(dut, REG_STATUS)
        if st & ST_DONE:
            return st
    raise AssertionError("轮询 STATUS.DONE 超时")


def words_from_state(state_bytes):
    return [int.from_bytes(state_bytes[i:i + 4], "little")
            for i in range(0, len(state_bytes), 4)]


# ---------------------------------------------------------------- 寄存器契约

@cocotb.test()
async def test_reset_values(dut):
    """复位后各寄存器的取值，以及 VERSION 常量"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for reg, want in ((REG_STATUS, 0), (REG_MODE, 0), (REG_PARAM, 0),
                      (REG_IN_LEN, 0), (REG_OUT_LEN, 0), (REG_ERRCODE, 0)):
        val, resp = await axil_read(dut, reg)
        assert val == want, f"复位后 {reg:#04x} 应为 {want}，实际 {val:#x}"
        assert resp == 0, "响应应为 OKAY"

    ver, _ = await axil_read(dut, REG_VERSION)
    assert ver == VERSION_CONST, f"VERSION 应为常量 {VERSION_CONST:#x}，实际 {ver:#x}"
    dut._log.info("复位取值与 VERSION 常量成立")


@cocotb.test()
async def test_rw_registers(dut):
    """MODE / PARAM / IN_LEN 可读可写，且字节选通生效"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for reg in (REG_MODE, REG_PARAM, REG_IN_LEN):
        for val in (0x1234_5678, 0xFFFF_FFFF, 0):
            await axil_write(dut, reg, val)
            got, _ = await axil_read(dut, reg)
            assert got == val, f"{reg:#04x} 写 {val:#x} 读回 {got:#x}"

    # 只写最低字节，其余三字节必须保持
    await axil_write(dut, REG_PARAM, 0xAABB_CCDD)
    await axil_write(dut, REG_PARAM, 0x0000_0011, strb=0x1)
    got, _ = await axil_read(dut, REG_PARAM)
    assert got == 0xAABB_CC11, f"字节选通未生效：{got:#x}"
    dut._log.info("读写寄存器与字节选通成立")


@cocotb.test()
async def test_start_self_clearing(dut):
    """START 由硬件自清：写 1 之后读回恒为 0

    若不自清，软件任何一次"读改写 CTRL"都会再触发一次运算。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await axil_write(dut, REG_MODE, 1)          # 未实现的模式，立即结束
    await axil_write(dut, REG_CTRL, CTRL_START)
    for _ in range(4):
        ctrl, _ = await axil_read(dut, REG_CTRL)
        assert ctrl == 0, f"CTRL 读回 {ctrl:#x}，START 未自清"
    dut._log.info("START 自清成立")


@cocotb.test()
async def test_status_is_read_only(dut):
    """STATUS / OUT_LEN / ERRCODE 软件写入被忽略，且响应仍为 OKAY"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for reg in (REG_STATUS, REG_OUT_LEN, REG_ERRCODE, REG_VERSION):
        resp = await axil_write(dut, reg, 0xDEAD_BEEF)
        assert resp == 0, f"写只读寄存器 {reg:#04x} 的响应应为 OKAY"
        got, _ = await axil_read(dut, reg)
        assert got != 0xDEAD_BEEF, f"只读寄存器 {reg:#04x} 被软件改写了"
    dut._log.info("只读寄存器语义成立")


@cocotb.test()
async def test_done_is_latched(dut):
    """DONE 是电平锁存：反复读不清除，只有下一次 START 才清

    这条决定了软件"轮询 STATUS.DONE"能否工作 —— 脉冲语义下会漏采。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    state = bytes(random.Random(7).randrange(256) for _ in range(200))
    await axis_send(dut, words_from_state(state))
    await axil_write(dut, REG_MODE, MODE_KECCAK)
    await axil_write(dut, REG_IN_LEN, 200)
    await axil_write(dut, REG_CTRL, CTRL_START)
    await poll_done(dut)

    # 连读 20 次，DONE 必须一直在
    for i in range(20):
        st, _ = await axil_read(dut, REG_STATUS)
        assert st & ST_DONE, f"第 {i} 次读 STATUS 时 DONE 掉了"
        assert not (st & ST_BUSY), "命令已完成，BUSY 不应还在"

    # 下一次 START 才清
    await axil_write(dut, REG_CTRL, CTRL_START)
    st, _ = await axil_read(dut, REG_STATUS)
    assert not (st & ST_DONE) or (st & ST_BUSY), "START 之后 DONE 未被清除"
    dut._log.info("DONE 电平锁存、由 START 清除")


@cocotb.test()
async def test_unsupported_mode(dut):
    """未实现的操作码：置 ERR 且 ERRCODE 为 3，而不是悄悄回落"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for mode, in_len in ((1, 64), (5, 128), (MODE_KECCAK, 199), (MODE_NTT_FWD, 500)):
        await axil_write(dut, REG_MODE, mode)
        await axil_write(dut, REG_IN_LEN, in_len)
        await axil_write(dut, REG_CTRL, CTRL_START)
        st = await poll_done(dut)
        assert st & ST_ERR, f"模式 {mode} 长度 {in_len} 应当报错"
        err, _ = await axil_read(dut, REG_ERRCODE)
        assert err == 3, f"ERRCODE 应为 3，实际 {err}"
        olen, _ = await axil_read(dut, REG_OUT_LEN)
        assert olen == 0, "出错时 OUT_LEN 应为 0"
    dut._log.info("未实现的操作码如实报错")


# ---------------------------------------------------------------- 数据面

async def run_keccak(dut, state_bytes, gap=0):
    await axis_send(dut, words_from_state(state_bytes), gap=gap)
    await axil_write(dut, REG_MODE, MODE_KECCAK)
    await axil_write(dut, REG_IN_LEN, 200)
    await axil_write(dut, REG_CTRL, CTRL_START)
    st = await poll_done(dut)
    assert not (st & ST_ERR), "Keccak 命令返回了错误"
    olen, _ = await axil_read(dut, REG_OUT_LEN)
    assert olen == 200, f"OUT_LEN 应为 200，实际 {olen}"
    words = await axis_recv(dut, gap=gap)
    assert len(words) == 50, f"输出流应有 50 拍，实际 {len(words)}"
    return b"".join(w.to_bytes(4, "little") for w in words)


@cocotb.test()
async def test_keccak_datapath(dut):
    """Keccak-f[1600] 走完整的 AXI 路径，与参考模型逐字节比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    rng = random.Random(20260730)

    for trial in range(3):
        state = bytes(0 for _ in range(200)) if trial == 0 else \
            bytes(rng.randrange(256) for _ in range(200))
        got = await run_keccak(dut, state)

        lanes = [int.from_bytes(state[8 * i:8 * i + 8], "little") for i in range(25)]
        want = b"".join(x.to_bytes(8, "little") for x in keccak_f1600(lanes))
        assert got == want, f"第 {trial} 组 Keccak 结果与参考模型不一致"
    dut._log.info("Keccak 经 AXI4-Lite + AXI4-Stream 与参考模型逐字节一致")


@cocotb.test()
async def test_keccak_with_gaps(dut):
    """流接口的空拍：TVALID / TREADY 断续时结果必须不变"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    state = bytes((i * 7) & 0xFF for i in range(200))
    dense = await run_keccak(dut, state, gap=0)
    sparse = await run_keccak(dut, state, gap=3)
    assert dense == sparse, "带空拍的传输给出了不同结果"

    lanes = [int.from_bytes(state[8 * i:8 * i + 8], "little") for i in range(25)]
    want = b"".join(x.to_bytes(8, "little") for x in keccak_f1600(lanes))
    assert dense == want, "结果与参考模型不一致"
    dut._log.info("输入输出流的空拍不影响结果")


@cocotb.test()
async def test_ntt_datapath(dut):
    """NTT 正变换走完整的 AXI 路径，与参考模型逐系数比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    rng = random.Random(20260731)

    coeffs = [rng.randrange(-1664, 1665) for _ in range(256)]
    words = [((coeffs[2 * i] & 0xFFFF) | ((coeffs[2 * i + 1] & 0xFFFF) << 16))
             for i in range(128)]
    await axis_send(dut, words)
    await axil_write(dut, REG_MODE, MODE_NTT_FWD)
    await axil_write(dut, REG_IN_LEN, 512)
    await axil_write(dut, REG_CTRL, CTRL_START)
    st = await poll_done(dut)
    assert not (st & ST_ERR), "NTT 命令返回了错误"
    olen, _ = await axil_read(dut, REG_OUT_LEN)
    assert olen == 512, f"OUT_LEN 应为 512，实际 {olen}"

    out_words = await axis_recv(dut)
    assert len(out_words) == 128
    got = []
    for w in out_words:
        got.append(s16(w & 0xFFFF))
        got.append(s16(w >> 16))
    assert got == ntt(list(coeffs)), "NTT 结果与参考模型不一致"
    dut._log.info("NTT 经 AXI4-Lite + AXI4-Stream 与参考模型逐系数一致")


@cocotb.test()
async def test_busy_blocks_input(dut):
    """运算进行中输入流不接收：BUSY 期间 TREADY 必须落下"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    coeffs = [((i * 13) % 3329) - 1664 for i in range(256)]
    words = [((coeffs[2 * i] & 0xFFFF) | ((coeffs[2 * i + 1] & 0xFFFF) << 16))
             for i in range(128)]
    await axis_send(dut, words)
    await axil_write(dut, REG_MODE, MODE_NTT_FWD)
    await axil_write(dut, REG_IN_LEN, 512)
    await axil_write(dut, REG_CTRL, CTRL_START)

    # 命令刚开始，此时应当 BUSY 且不收数据
    saw_busy = False
    for _ in range(50):
        st, _ = await axil_read(dut, REG_STATUS)
        if st & ST_BUSY:
            saw_busy = True
            await ReadOnly()
            assert int(dut.s_axis_tready.value) == 0, "BUSY 期间输入流仍在收数据"
            await RisingEdge(dut.clk)
            break
    assert saw_busy, "整条命令期间没有观察到 BUSY"

    await poll_done(dut)
    await ReadOnly()
    assert int(dut.s_axis_tready.value) == 1, "命令结束后输入流应恢复接收"
    await RisingEdge(dut.clk)
    dut._log.info("BUSY 期间输入流关闭，结束后恢复")


@cocotb.test()
async def test_soft_reset(dut):
    """SOFT_RESET：清状态位与流指针，且自身自清"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    state = bytes((i * 3) & 0xFF for i in range(200))
    await run_keccak(dut, state)
    st, _ = await axil_read(dut, REG_STATUS)
    assert st & ST_DONE

    await axil_write(dut, REG_CTRL, CTRL_SOFT_RESET)
    ctrl, _ = await axil_read(dut, REG_CTRL)
    assert ctrl == 0, "SOFT_RESET 未自清"
    st, _ = await axil_read(dut, REG_STATUS)
    assert st == 0, f"SOFT_RESET 之后 STATUS 应清零，实际 {st:#x}"

    # 复位之后仍能正常跑一条命令
    got = await run_keccak(dut, state)
    lanes = [int.from_bytes(state[8 * i:8 * i + 8], "little") for i in range(25)]
    want = b"".join(x.to_bytes(8, "little") for x in keccak_f1600(lanes))
    assert got == want, "SOFT_RESET 之后的运算结果不对"
    dut._log.info("SOFT_RESET 清状态并自清，复位后可继续工作")
