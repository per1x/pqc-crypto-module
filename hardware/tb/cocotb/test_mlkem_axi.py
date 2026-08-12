"""cocotb：ML-KEM 三个整核的 AXI4-Lite 从机

这一层验的不是算法（那在 test_mlkem_keygen / _encaps / _decaps 里逐系数验过了），
而是**软件视角下这块外设能不能用**：

  ① 三种模式各跑一遍，输出字节流与黄金模型逐字节一致 ——
     软件只往 IN_DATA 灌字节、只从 OUT_DATA 取字节，长度由 param_set 算，
     不用报长度也就报不错。
  ② KeyGen → Encaps → Decaps **首尾相接**：用上一步的产物喂下一步，
     最后解出来的 K 必须等于封装时的 K。这条比三个孤立的 KAT 更有力 ——
     它要求三个核对 ek/dk/c 的字节序理解完全一致。
  ③ 防火墙：non-secure 被拦且无副作用。
  ④ zeroize 把输入输出缓冲一起清掉（缓冲区里有 dk 的字节）。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mlkem_oracle import (  # noqa: E402
    PARAMS, mlkem_decaps, mlkem_encaps, mlkem_keygen,
)

VERSION, CTRL, STATUS, MODE = 0x00, 0x04, 0x08, 0x0C
IN_DATA, IN_PTR, OUT_DATA, OUT_LEN = 0x10, 0x14, 0x18, 0x1C
OUT_RD, VIOL_CNT, PARAM0 = 0x20, 0x24, 0x28

C_START, C_ZEROIZE, C_IN_RST, C_OUT_RST = 1 << 0, 1 << 1, 1 << 2, 1 << 3
ST_BUSY, ST_DONE, ST_HASHOK, ST_TAMPER = 1 << 0, 1 << 1, 1 << 2, 1 << 3

M_KEYGEN, M_ENCAPS, M_DECAPS = 0, 1, 2
PSET = {"ML-KEM-512": 0, "ML-KEM-768": 1, "ML-KEM-1024": 2}

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_DECERR = 0, 3


async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
    dut.s_axi_awaddr.value = 0
    dut.s_axi_awprot.value = 0
    dut.s_axi_awvalid.value = 0
    dut.s_axi_wdata.value = 0
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 0
    dut.s_axi_bready.value = 1
    dut.s_axi_araddr.value = 0
    dut.s_axi_arprot.value = 0
    dut.s_axi_arvalid.value = 0
    dut.s_axi_rready.value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def rd(dut, addr, prot=PROT_SECURE):
    dut.s_axi_araddr.value = addr
    dut.s_axi_arprot.value = prot
    dut.s_axi_arvalid.value = 1
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_arready.value):
            await RisingEdge(dut.clk)
            break
        await RisingEdge(dut.clk)
    else:
        raise AssertionError(f"读 0x{addr:02x}：arready 一直不来")
    dut.s_axi_arvalid.value = 0
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_rvalid.value):
            d = int(dut.s_axi_rdata.value)
            r = int(dut.s_axi_rresp.value)
            await RisingEdge(dut.clk)
            return d, r
        await RisingEdge(dut.clk)
    raise AssertionError(f"读 0x{addr:02x}：rvalid 一直不来")


async def wr(dut, addr, data, prot=PROT_SECURE):
    dut.s_axi_awaddr.value = addr
    dut.s_axi_awprot.value = prot
    dut.s_axi_awvalid.value = 1
    dut.s_axi_wdata.value = data
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 1
    aw = w = False
    for _ in range(64):
        await Timer(1, unit="ns")
        ta = int(dut.s_axi_awready.value) and not aw
        tw = int(dut.s_axi_wready.value) and not w
        await RisingEdge(dut.clk)
        if ta:
            aw = True
            dut.s_axi_awvalid.value = 0
        if tw:
            w = True
            dut.s_axi_wvalid.value = 0
        if aw and w:
            break
    else:
        raise AssertionError(f"写 0x{addr:02x}：没握上手")
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_bvalid.value):
            r = int(dut.s_axi_bresp.value)
            await RisingEdge(dut.clk)
            return r
        await RisingEdge(dut.clk)
    raise AssertionError(f"写 0x{addr:02x}：bvalid 一直不来")


async def run_op(dut, mode, name, payload: bytes, limit=400_000):
    """灌输入 → 启动 → 等完成 → 取输出。返回输出字节"""
    assert await wr(dut, MODE, mode | (PSET[name] << 2)) == RESP_OKAY
    assert await wr(dut, CTRL, C_IN_RST) == RESP_OKAY
    for b in payload:
        assert await wr(dut, IN_DATA, b) == RESP_OKAY
    p, _ = await rd(dut, IN_PTR)
    assert p == len(payload), f"IN_PTR = {p}，应当是 {len(payload)}"

    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
    else:
        raise AssertionError(f"{name} mode={mode}：一直没完成")

    n, _ = await rd(dut, OUT_LEN)
    out = bytearray()
    for _ in range(n):
        d, _ = await rd(dut, OUT_DATA)
        out.append(d & 0xFF)
    return bytes(out)


@cocotb.test()
async def test_keygen_over_axi(dut):
    """KeyGen：ek‖dk 逐字节与黄金模型一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    v, r = await rd(dut, VERSION)
    assert r == RESP_OKAY and v == 0x0001_0000, f"VERSION=0x{v:08x}"

    name = "ML-KEM-512"
    d = bytes(range(32))
    z = bytes(range(32, 64))
    ek, dk = mlkem_keygen(d, z, name)

    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert len(got) == len(ek) + len(dk), \
        f"输出 {len(got)} 字节，应当是 {len(ek) + len(dk)}"
    assert got[:len(ek)] == ek, "ek 与黄金模型不一致"
    assert got[len(ek):] == dk, "dk 与黄金模型不一致"

    dut._log.info(f"{name} KeyGen 走 AXI：ek {len(ek)} + dk {len(dk)} 字节逐字节一致")


@cocotb.test()
async def test_chain_keygen_encaps_decaps(dut):
    """三个核首尾相接：KeyGen → Encaps → Decaps，最后的 K 必须对上

    这条比三个孤立的 KAT 更有力：它要求三个核对 ek / dk / c 的字节序理解
    完全一致。任何一处字节序反了，孤立的 KAT 仍可能各自通过（因为各自都拿
    黄金模型比），但接起来就断。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d = bytes([0x11] * 32)
    z = bytes([0x22] * 32)
    m = bytes([0x33] * 32)

    kg = await run_op(dut, M_KEYGEN, name, d + z)
    k, _ = PARAMS[name]
    eklen = 384 * k + 32
    ek, dk = kg[:eklen], kg[eklen:]
    assert (ek, dk) == mlkem_keygen(d, z, name), "KeyGen 这一步就不对"

    enc = await run_op(dut, M_ENCAPS, name, m + ek)
    shared, ct = enc[:32], enc[32:]
    assert (shared, ct) == mlkem_encaps(ek, m, name), "Encaps 与黄金模型不一致"

    dec = await run_op(dut, M_DECAPS, name, dk + ct)
    assert len(dec) == 32, f"Decaps 输出 {len(dec)} 字节，应当是 32"
    assert dec == shared, (
        f"解出来的 K {dec.hex()} 与封装时的 {shared.hex()} 不一致 —— "
        "三个核对字节序的理解不一致")
    assert dec == mlkem_decaps(dk, ct, name), "与黄金模型的 Decaps 不一致"

    st, _ = await rd(dut, STATUS)
    assert st & ST_HASHOK, "自己生成的 dk，哈希自检却没过"

    dut._log.info("KeyGen → Encaps → Decaps 首尾相接：解出的 K 与封装时逐字节相同")


@cocotb.test()
async def test_implicit_reject_over_axi(dut):
    """改一个密文字节 → 走隐式拒绝，且与黄金模型一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z, m = bytes([1] * 32), bytes([2] * 32), bytes([3] * 32)
    ek, dk = mlkem_keygen(d, z, name)
    shared, ct = mlkem_encaps(ek, m, name)

    bad = bytearray(ct)
    bad[len(ct) // 2] ^= 0x01
    bad = bytes(bad)

    got = await run_op(dut, M_DECAPS, name, dk + bad)
    want = mlkem_decaps(dk, bad, name)
    assert want != shared, "黄金模型自检：改过的密文不该还解出原来的 K"
    assert got == want, f"隐式拒绝值 {got.hex()} 与黄金模型 {want.hex()} 不一致"

    dut._log.info("密文改一个字节：返回 J(z‖c)，与黄金模型逐字节一致")


@cocotb.test()
async def test_firewall_and_zeroize(dut):
    """non-secure 被拦且无副作用；zeroize 把缓冲区一起清掉"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([7] * 32), bytes([8] * 32)
    ek, dk = mlkem_keygen(d, z, name)

    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk

    # non-secure：读写都 DECERR
    _, r = await rd(dut, OUT_LEN, PROT_NONSEC)
    assert r == RESP_DECERR, "non-secure 读没被拦"
    assert await wr(dut, CTRL, C_ZEROIZE, PROT_NONSEC) == RESP_DECERR, \
        "non-secure 写没被拦"

    # 被拦的那笔没有副作用：输出还在，还读得出来
    n, _ = await rd(dut, OUT_LEN)
    assert n == len(ek) + len(dk), "non-secure 的写把输出缓冲清掉了"

    # 越界地址
    _, r = await rd(dut, 0x80)
    assert r == RESP_DECERR, "越界地址没被拦"

    # secure 的 zeroize：缓冲区清空
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    for _ in range(8):
        await RisingEdge(dut.clk)
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"zeroize 之后 OUT_LEN = {n}，应当是 0"
    p, _ = await rd(dut, IN_PTR)
    assert p == 0, f"zeroize 之后 IN_PTR = {p}，应当是 0"

    # 清完还能照常再跑一次
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk, "zeroize 之后再跑结果不对"

    dut._log.info("防火墙拦下 non-secure 与越界且无副作用；zeroize 连缓冲区一起清")


@cocotb.test()
async def test_repeat_same_core(dut):
    """同一个核**连跑两次，中间不 zeroize** —— 这是仿真曾经的盲区

    三个 ML-KEM 核的 done 是**电平**，保持到下一次 start 才清。而 mlkem_axi
    的 S_KICK 用非阻塞赋值拉 start，start 要到下一拍才有效。如果一进 S_RUN
    就检查 core_dn，第二次运行会当场读到上一次残留的 done，立刻结束、
    OUT_LEN 是 0。

    **第一次永远对，第二次必错** —— 这个 bug 是在真硬件上暴露的：
    每组 ACVP 向量的 tc0 通过、tc1 报"输出 0 字节"。

    仿真之前抓不到，是因为链式用例里每个核只跑一次，而"跑两次"那条用例
    中间隔着一次 zeroize（zeroize 会复位核，把 done 清掉）。这条用例专门
    补上那个缺口：同一个核连续两次，中间什么都不做。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    for i, (d, z) in enumerate([(bytes([0x41] * 32), bytes([0x42] * 32)),
                                (bytes([0x51] * 32), bytes([0x52] * 32))]):
        ek, dk = mlkem_keygen(d, z, name)
        got = await run_op(dut, M_KEYGEN, name, d + z)
        assert len(got) == len(ek) + len(dk), (
            f"第 {i+1} 次 KeyGen 输出 {len(got)} 字节，应当是 {len(ek)+len(dk)}"
            + ("  ← 第二次就是残留 done 那个 bug" if i == 1 else ""))
        assert got == ek + dk, f"第 {i+1} 次 KeyGen 与黄金模型不一致"

    # Encaps 也连跑两次
    ek, dk = mlkem_keygen(bytes([1] * 32), bytes([2] * 32), name)
    for i, m in enumerate([bytes([0x61] * 32), bytes([0x71] * 32)]):
        shared, ct = mlkem_encaps(ek, m, name)
        got = await run_op(dut, M_ENCAPS, name, m + ek)
        assert got == shared + ct, f"第 {i+1} 次 Encaps 不一致"

    dut._log.info("KeyGen 连跑两次、Encaps 连跑两次，中间不 zeroize —— 全对")
