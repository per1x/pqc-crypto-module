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
  ④ zeroize 把输入输出缓冲一起清掉（缓冲区里有 dk 的字节）——
     判据是**读回 BRAM 每一个字节确认是 0**，不是"OUT_LEN 变成 0"。
     后者只证明目录页被撕了，正文还在不在它答不了。
  ⑤ 非法的 mode / pset（值 3）在 START 那一刻被拒，不启动任何核。
"""
import os
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
ST_WIPING, ST_PARAMERR = 1 << 4, 1 << 5

M_KEYGEN, M_ENCAPS, M_DECAPS = 0, 1, 2
PSET = {"ML-KEM-512": 0, "ML-KEM-768": 1, "ML-KEM-1024": 2}

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_SLVERR, RESP_DECERR = 0, 2, 3
# ============================================================================
# 【"被拒"在总线上长什么样：RAZ/WI，不是 DECERR】
# ============================================================================
# 防火墙拒绝一笔访问时**读回 0、写丢弃，响应仍是 OKAY** —— 不产生总线错误。
# 改动的理由在 hardware/rtl/bus/axi4lite_firewall.v 的文件头：DECERR 的
# posted 写会以 SError 回来，aarch64 的内核只能 panic，于是"写错一个地址"
# 的代价是一次断电。
#
# 用例里一律写 RESP_REFUSED，不写具体码值 —— **"被拒长什么样"是 RTL 的策略，
# 不该抄进每一条断言**。抄进去的后果这次已经见过了：策略一改，几十条断言
# 全得跟着动，而它们本来一条都不该动（它们要证的是"被拒了"，不是"回了 3"）。
#
# ⚠️ 读的断言必须**同时**查 rdata == 0。RAZ/WI 之后响应码不再区分放行与拒绝，
#    数据才是。只查 resp 的读用例现在等于什么都没查。
RESP_REFUSED = RESP_OKAY


async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
    # 译码器的违规计数是一个**输入端口**（板级顶层从 xbar 接过来）。
    # 单独跑这个从机时没人驱动它，Icarus 下它是 X —— 于是读 0x2C 拿到 X，
    # int() 直接抛异常。以前没暴露是因为没有用例读过 0x2C。
    dut.xbar_viol_count.value = 0
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
    rv, r = await rd(dut, OUT_LEN, PROT_NONSEC)
    assert r == RESP_REFUSED and rv == 0, f"non-secure 读没被拦：0x{rv:08x}"
    assert await wr(dut, CTRL, C_ZEROIZE, PROT_NONSEC) == RESP_REFUSED, \
        "non-secure 写没被拦"

    # 被拦的那笔没有副作用：输出还在，还读得出来
    n, _ = await rd(dut, OUT_LEN)
    assert n == len(ek) + len(dk), "non-secure 的写把输出缓冲清掉了"

    # 越界地址
    rv, r = await rd(dut, 0x80)
    assert r == RESP_REFUSED and rv == 0, f"越界地址没被拦：0x{rv:08x}"

    # secure 的 zeroize：缓冲区清空。
    # 擦 8192 个地址要 8192 拍，这期间设备**拒绝一切写并回 SLVERR** ——
    # 不是静默丢弃。板上软件的义务就是轮询 WIPING，这里照着做。
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    assert await wr(dut, IN_DATA, 0x99) == RESP_SLVERR, \
        "擦除期间的写没有被拒 —— 静默丢弃会让软件按错误长度启动"
    await _wait_wipe(dut)
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


# ---------------------------------------------------------------------------
# 以下三条针对的是"zeroize 只清指针不擦 BRAM"与"非法参数不被拒"
# ---------------------------------------------------------------------------

def _mem_nonzero(mem):
    """返回 (非零个数, 第一个非零的 (下标, 值))"""
    bad = 0
    first = None
    for i in range(len(mem)):
        v = int(mem[i].value)
        if v:
            bad += 1
            if first is None:
                first = (i, v)
    return bad, first


async def _wait_wipe(dut, limit=40000):
    # 上限随金库一起放大：金库从 16 KB 加到 64 KB（4→16 槽）之后，擦除机要走
    # 65536 拍 —— 三块 BRAM 里最大的那块决定节拍数。每次轮询是一笔 AXI 读、
    # 占好几拍，所以 40000 次轮询足够覆盖。
    # 这个上限被动过两次了，每次都是加金库容量的直接代价，不是偶然。
    """等 WIPING 落下来，顺便断言它确实曾经高过

    用软件可见的 STATUS 位来等，而不是偷看内部信号 —— 板上程序能依赖的
    就是这一位，测试也应该只依赖这一位。
    """
    st, _ = await rd(dut, STATUS)
    assert st & ST_WIPING, (
        "写了 ZEROIZE 之后 STATUS.WIPING 没有拉高 —— "
        "说明根本没有启动擦除，只是清了指针")
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            return
    raise AssertionError("WIPING 一直没落下来")


@cocotb.test()
async def test_zeroize_really_wipes_bram(dut):
    """zeroize 之后**读回两块 BRAM 的每一个字节**，必须全是 0

    这是这条用例与旧的 test_firewall_and_zeroize 的区别：那条只验了
    OUT_LEN == 0 与 IN_PTR == 0，也就是"软件读不到了"。而缓冲区里
    上一次 KeyGen 的 dk 一个字节都没少 —— 旧用例对此完全无感。

    残留分两种，都要覆盖：
      · **真实残留**：跑一次真的 KeyGen，outbuf 里就是真的 dk 字节；
      · **全量残留**：把两块 BRAM 的 8192 个地址全填上 0xAB，
        证明擦的是整个地址空间，不是"用到的那一段"。
        只擦用过的那一段是个很容易犯的错，而且看起来一样有效。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x11] * 32), bytes([0x22] * 32)
    ek, dk = mlkem_keygen(d, z, name)
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk

    inmem, outmem = dut.u_inbuf.mem, dut.u_outbuf.mem
    depth = len(inmem)
    assert depth == 8192 and len(outmem) == 8192

    # 真实残留确实在（先证明"有东西可擦"，否则后面全 0 的断言不值钱）
    live = sum(1 for i in range(len(ek) + len(dk)) if int(outmem[i].value))
    assert live > 1000, f"outbuf 里只有 {live} 个非零字节，残留没建立起来"

    # 全量残留：把两块 BRAM 填满
    for i in range(depth):
        inmem[i].value = 0xAB
        outmem[i].value = 0xCD
    await RisingEdge(dut.clk)

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    for label, mem in (("inbuf", inmem), ("outbuf", outmem)):
        bad, first = _mem_nonzero(mem)
        assert bad == 0, (
            f"{label} 擦除之后还有 {bad}/{depth} 个字节非零，"
            f"第一个在 [{first[0]}] = 0x{first[1]:02x}")

    # 元数据也清了，而且擦完还能照常再跑
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk, "擦除之后再跑结果不对"

    dut._log.info(f"zeroize 后两块 {depth} 字节 BRAM 逐字节读回，全为 0")


@cocotb.test()
async def test_tamper_wipes_bram_and_blocks_output(dut):
    """tamper 走同一台擦除机；擦除期间不给输出、不接受启动

    tamper 与软件 zeroize 的区别在于它是**锁存**的电平。擦除机若用电平触发，
    tamper 之后会永远重启擦除、WIPING 再也不会落下来 —— 所以这条用例
    专门等 WIPING 落地。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x33] * 32), bytes([0x44] * 32)
    ek, dk = mlkem_keygen(d, z, name)
    assert await run_op(dut, M_KEYGEN, name, d + z) == ek + dk

    inmem, outmem = dut.u_inbuf.mem, dut.u_outbuf.mem
    for i in range(len(inmem)):
        inmem[i].value = 0x5A
        outmem[i].value = 0xA5
    await RisingEdge(dut.clk)

    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0

    # 擦除期间：OUT_DATA 不给任何东西
    # （tamper 之后防火墙已锁存，读会 DECERR —— 这本身也是要证的：
    #  被 tamper 的模块对总线是完全关闭的，不只是"输出为 0"）
    rv, r = await rd(dut, OUT_DATA)
    assert r == RESP_REFUSED and rv == 0, \
        f"tamper 之后 OUT_DATA 还能读到 0x{rv:08x} —— 这正是 RAZ/WI 必须回 0 的原因"

    # 等擦完（tamper 后总线关了，只能看内部的 wiping —— 这一处是唯一
    # 无法从软件侧观测的，因为软件侧此时已经被整体拒绝了）
    # ⚠️ 这个循环是**逐时钟周期**轮询（不像别处是经 AXI 读 STATUS，
    # 一次读要好几拍）。所以上限必须大于擦除的拍数本身：金库 64 KB →
    # 65536 拍。上一版填 60000 就差在这里 —— 比 65536 少，
    # 于是报"WIPING 一直没落下来"，看着像触发方式写错了，其实只是没等够。
    for _ in range(80000):
        await RisingEdge(dut.clk)
        if not int(dut.wiping.value):
            break
    else:
        raise AssertionError("tamper 之后 WIPING 一直没落下来 —— "
                             "多半是用电平而不是上升沿触发擦除")

    for label, mem in (("inbuf", inmem), ("outbuf", outmem)):
        bad, first = _mem_nonzero(mem)
        assert bad == 0, (
            f"tamper 之后 {label} 还有 {bad} 个字节非零，"
            f"第一个在 [{first[0]}] = 0x{first[1]:02x}")

    dut._log.info("tamper 触发一次完整擦除，两块 BRAM 读回全 0，总线全程被拒")


@cocotb.test()
async def test_illegal_mode_and_pset_refused(dut):
    """mode=3 / pset=3 在 START 那一刻被拒，且**不启动任何核**

    这两个字段各 2 位而只有 0/1/2 有意义。值 3 不是"另一种配置"：
    模式选择落到 default（Decaps），长度按 pset==2 那条分支算，
    于是核按 1024 的长度等 Decaps 的输入 —— 喂不满就永远等下去。
    软件侧看到的是 BUSY 一直不落，与"算得慢"分不开。

    判据是三条一起：PARAM_ERR 置位、BUSY 从未拉起、OUT_LEN 仍是 0。
    只看 PARAM_ERR 是不够的 —— 先启动再报错同样能置位。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for mode, pset, why in ((3, 1, "mode=3"), (0, 3, "pset=3"), (3, 3, "两个都=3")):
        assert await wr(dut, MODE, mode | (pset << 2)) == RESP_OKAY
        # 读回确认这个非法值确实进到了寄存器里（否则下面测的是别的东西）
        m, _ = await rd(dut, MODE)
        assert m == (mode | (pset << 2)), f"{why}：MODE 回读 {m:#x}"

        assert await wr(dut, CTRL, C_IN_RST) == RESP_OKAY
        for b in bytes([0x77] * 64):
            assert await wr(dut, IN_DATA, b) == RESP_OKAY

        assert await wr(dut, CTRL, C_START) == RESP_OKAY

        busy_seen = False
        for _ in range(200):
            st, _ = await rd(dut, STATUS)
            if st & ST_BUSY:
                busy_seen = True
        st, _ = await rd(dut, STATUS)
        assert st & ST_PARAMERR, f"{why}：STATUS.PARAM_ERR 没置位（{st:#x}）"
        assert not busy_seen, f"{why}：核竟然被启动了（BUSY 拉起过）"
        assert not (st & ST_DONE), f"{why}：竟然报了 DONE"
        n, _ = await rd(dut, OUT_LEN)
        assert n == 0, f"{why}：OUT_LEN = {n}，核确实跑了"

    # 换回合法参数：错误位清掉，照常能跑
    name = "ML-KEM-512"
    d, z = bytes([0x61] * 32), bytes([0x62] * 32)
    ek, dk = mlkem_keygen(d, z, name)
    assert await run_op(dut, M_KEYGEN, name, d + z) == ek + dk
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_PARAMERR), "合法参数跑完之后 PARAM_ERR 还挂着"

    dut._log.info("mode=3 / pset=3 / 两者皆 3 全部被拒且未启动核；换回合法值照常")


@cocotb.test()
async def test_illegal_start_after_success_invalidates_result(dut):
    """非法 START **发生在一次成功运行之后** —— 这一条是上板才补的

    上面那条 negative test 从复位开始，OUT_LEN 本来就是 0，于是"拒绝之后
    还留着上一次的结果"这个形状根本没出现过。板上是连着跑的：

        跑一次 KeyGen-512  → DONE=1，OUT_LEN=2432
        写 MODE = mode:3   → 非法
        写 CTRL.START      → 被拒，PARAM_ERR=1，核确实没启动 ✓
        软件轮询 STATUS    → **DONE 还是 1**（上一次留下的）
        读 OUT_LEN         → **2432**，读 OUT_DATA → 上一次的 ek‖dk

    也就是说：软件拿着**上一次**的输出，当成这一次的结果。这比不报错更糟，
    因为它看起来成功了。所以一次 START 尝试就作废上一次的结果，
    不管这次是否被接受。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x81] * 32), bytes([0x82] * 32)
    ek, dk = mlkem_keygen(d, z, name)
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk

    st, _ = await rd(dut, STATUS)
    assert st & ST_DONE, "先决条件不成立：成功那次没有报 DONE"
    n, _ = await rd(dut, OUT_LEN)
    assert n == len(ek) + len(dk)

    # 紧接着来一次非法 START
    assert await wr(dut, MODE, 3 | (1 << 2)) == RESP_OKAY
    assert await wr(dut, CTRL, C_START) == RESP_OKAY

    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, f"非法 START 没置 PARAM_ERR（{st:#x}）"
    assert not (st & ST_BUSY), "非法 START 竟然启动了核"
    assert not (st & ST_DONE), (
        f"非法 START 之后 DONE 仍然是 1（STATUS={st:#x}）—— "
        "软件会拿上一次的输出当成这一次的结果")

    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, (
        f"非法 START 之后 OUT_LEN 还是 {n} —— 上一次的 ek‖dk 还摆在那里")

    # 换回合法参数，一切照常
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == ek + dk, "非法 START 之后再跑合法的，结果不对"
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_PARAMERR)

    dut._log.info("非法 START 作废上一次的 DONE 与 OUT_LEN，软件不会误读陈值")


@cocotb.test()
async def test_underfill_refused_and_z_not_stale(dut):
    """只喂 32 字节就 START —— 必须被拒，且**不能**用残留当 z

    这是一个 KAT 抓不到的安全 bug。KeyGen 要 d‖z 共 64 字节；只写 32 字节 d
    的话，z 取的是输入缓冲 32..63 的残留，冷启动/刚 zeroize 之后那一段全 0，
    于是 **z = 0**。

    z 是 ML-KEM 的隐式拒绝密钥：解封装失败返回 J(z‖c)。z 可预测，攻击者就能
    自己算出任意密文的隐式拒绝值，从而把"解封装失败"与"成功"区分开 ——
    而隐式拒绝存在的全部意义就是让这两者不可区分。

    判据分两层，第二层才是关键：
      ① START 被拒（PARAM_ERR=1、BUSY 从未拉起、上次结果作废）；
      ② **把同一个 d 配一个真正的 z 喂满再跑，输出必须与"欠填那次若被放行会
         得到的结果"不同** —— 也就是证明硬件确实没有把 z=0 那条路跑出来。
         只验 ① 是不够的：报了错也可能核已经跑过一轮。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d = bytes([0x5A] * 32)

    # ---- ① 只喂 32 字节 ----
    assert await wr(dut, MODE, M_KEYGEN | (PSET[name] << 2)) == RESP_OKAY
    assert await wr(dut, CTRL, C_IN_RST) == RESP_OKAY
    for b in d:
        assert await wr(dut, IN_DATA, b) == RESP_OKAY
    p, _ = await rd(dut, IN_PTR)
    assert p == 32, f"先决条件不成立：IN_PTR = {p}"

    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    busy_seen = False
    for _ in range(300):
        st, _ = await rd(dut, STATUS)
        if st & ST_BUSY:
            busy_seen = True
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, f"欠填没被拒（STATUS={st:#x}）—— z 会取到残留"
    assert not busy_seen, "欠填却启动了核 —— 残留已经进了运算"
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"欠填之后 OUT_LEN = {n}"

    # ---- ② 反证：喂满之后结果必须是"真 z"那一份 ----
    # 用 z = 全 0 跑一次（这正是欠填若被放行会得到的输入），
    # 再用 z = 真值跑一次，两者必须不同。若硬件当初放行了欠填，
    # 它算出来的就是第一份 —— 这一条把"拒绝"与"结果对不对"钉在一起。
    ek0, dk0 = mlkem_keygen(d, bytes(32), name)
    got0 = await run_op(dut, M_KEYGEN, name, d + bytes(32))
    assert got0 == ek0 + dk0, "z=全0 这一路本身与黄金模型不一致"

    z = bytes([0xA5] * 32)
    ek1, dk1 = mlkem_keygen(d, z, name)
    got1 = await run_op(dut, M_KEYGEN, name, d + z)
    assert got1 == ek1 + dk1, "z=真值这一路与黄金模型不一致"

    assert got0 != got1, (
        "z 全 0 与 z 真值算出同一个密钥对 —— 那说明 z 根本没进运算，"
        "这条用例就失去了意义")

    dut._log.info(
        "欠填被拒且核未启动；z=0 与 z=真值的输出确实不同（dk 差异证明 z 参与了运算）")


# ============================================================================
# 片内私钥金库：dk 从头到尾不越过总线
# ============================================================================
KEYSTAT = 0x30
KEYPSET = 0x34          # 16 槽之后 pset 单独一个寄存器
C_DK_LOCK = 1 << 4          # CTRL 的一次性闩锁
M_DK_TO_SLOT = 1 << 4       # MODE：KeyGen 把 dk 写进金库
M_DK_FROM_SLOT = 1 << 5     # MODE：Decaps 从金库取 dk


def mode_word(mode, name, *, to_slot=False, from_slot=False, slot=0):
    return (mode | (PSET[name] << 2)
            | (M_DK_TO_SLOT if to_slot else 0)
            | (M_DK_FROM_SLOT if from_slot else 0)
            | (slot << 6))   # SLOT 现在是 4 位 [9:6]


async def run_raw(dut, mword, payload, limit=400_000):
    """和 run_op 一样，但 MODE 整字由调用方给（要用到新的那几位）"""
    assert await wr(dut, MODE, mword) == RESP_OKAY
    assert await wr(dut, CTRL, C_IN_RST) == RESP_OKAY
    for b in payload:
        assert await wr(dut, IN_DATA, b) == RESP_OKAY
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
        if st & (1 << 5):
            return None                      # PARAM_ERR
    else:
        raise AssertionError("一直没完成")
    n, _ = await rd(dut, OUT_LEN)
    out = bytearray()
    for _ in range(n):
        d, _ = await rd(dut, OUT_DATA)
        out.append(d & 0xFF)
    return bytes(out)


@cocotb.test()
async def test_dk_stays_on_chip(dut):
    """KeyGen 存槽 → Decaps 用槽：dk 一个字节都没经过总线

    这条用例的判据分两半，两半都必须成立：

      · **出不来**：存槽那趟的 OUT_LEN 恰好等于 ek 的长度，一个字节不多。
        只查"OUT_LEN 变小了"不够 —— 得钉死在 eklen 上，否则少给几个字节
        也能通过，而那意味着 dk 的前半截仍然出来了。

      · **还能用**：拿槽里的 dk 做 Decaps，解出来的 K 与 Encaps 那一端一致。
        少了这一半，一个"把 dk 直接丢掉"的实现也能通过上一半 ——
        那才是最容易写出来的错误版本。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x21] * 32), bytes([0x22] * 32)
    ek_ref, dk_ref = mlkem_keygen(d, z, name)

    # ---- KeyGen，dk 进槽 2 ----
    out = await run_raw(dut, mode_word(0, name, to_slot=True, slot=2), d + z)
    assert out is not None, "存槽的 KeyGen 被拒了"
    assert len(out) == len(ek_ref), \
        f"存槽时 OUT_LEN={len(out)}，应当恰好是 ek 的 {len(ek_ref)} 字节"
    assert out == ek_ref, "ek 与黄金模型不一致"
    assert dk_ref not in out, "dk 出现在了输出里"

    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 2), f"槽 2 没被标成有效：KEYSTAT=0x{ks:08x}"
    kp, _ = await rd(dut, KEYPSET)
    assert (kp >> (2 * 2)) & 3 == PSET[name], "槽里记的参数集不对"

    # ---- Encaps（用刚拿到的 ek）----
    m = bytes([0x33] * 32)
    kc = await run_raw(dut, mode_word(1, name), m + ek_ref)
    assert kc is not None
    k_enc, ct = kc[:32], kc[32:]

    # ---- Decaps：只送 c，dk 由金库供 ----
    k_dec = await run_raw(dut, mode_word(2, name, from_slot=True, slot=2), ct)
    assert k_dec is not None, "用槽做 Decaps 被拒了"
    assert k_dec[:32] == k_enc, "两端共享密钥不一致 —— 金库里的 dk 是坏的"

    dut._log.info(f"dk 全程留在片内：OUT_LEN={len(out)}（正好 ek），"
                  f"用槽解出的 K 与 Encaps 一致")


@cocotb.test()
async def test_dk_lock_is_one_way(dut):
    """DK_LOCK 置上之后，软件再怎么写 MODE 都拿不到 dk，而且撤不回来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x41] * 32), bytes([0x42] * 32)
    ek_ref, _ = mlkem_keygen(d, z, name)

    # 闩锁之前：不设 DK_TO_SLOT 就该拿到 ek‖dk（ACVP 核对靠这条路）
    out = await run_raw(dut, mode_word(0, name), d + z)
    assert len(out) > len(ek_ref), "闩锁之前 dk 就出不来了，那 ACVP 没法核对"

    assert await wr(dut, CTRL, C_DK_LOCK) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 16), "闩锁没置上"

    # 闩锁之后：**同一个 MODE 字**，dk 不再出来
    out2 = await run_raw(dut, mode_word(0, name), d + z)
    assert len(out2) == len(ek_ref), \
        f"闩锁之后 OUT_LEN={len(out2)}，应当只剩 ek 的 {len(ek_ref)} 字节"
    assert out2 == ek_ref

    # 撤不回来：CTRL 没有清它的位，zeroize 也不清 —— zeroize 是擦秘密，
    # 不是撤防线。这两件事分开，是因为「擦完之后防线还在」才是想要的性质。
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    for _ in range(20000):
        st, _ = await rd(dut, STATUS)
        if not (st & (1 << 4)):
            break
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 16), "zeroize 把闩锁清掉了 —— 它不该有这个能力"
    assert (ks & 0xF) == 0, "zeroize 之后槽的有效位应当全清"

    dut._log.info("闩锁一次性生效：dk 不再出总线，zeroize 也撤不回来")


@cocotb.test()
async def test_decaps_from_empty_slot_refused(dut):
    """从空槽 / 参数集不匹配的槽做 Decaps：当场拒绝，不是让它跑到超时

    这条单列，是因为失败方式很要命：槽不对时长度算错，核会一直等着被喂满，
    软件看到的是 BUSY 永远不落 —— 和"算得慢"分不开。所以必须在 START
    那一刻判掉并报 PARAM_ERR。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    ctlen = 768

    r = await run_raw(dut, mode_word(2, name, from_slot=True, slot=1),
                      bytes(ctlen), limit=5000)
    assert r is None, "空槽的 Decaps 居然跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & (1 << 5), "空槽应当报 PARAM_ERR"
    assert not (st & 1), "空槽被拒之后不该还 BUSY"

    # 参数集不匹配：往槽 0 存一个 512 的 dk，再按 768 去用它
    d, z = bytes([0x51] * 32), bytes([0x52] * 32)
    out = await run_raw(dut, mode_word(0, name, to_slot=True, slot=0), d + z)
    assert out is not None
    r = await run_raw(dut, mode_word(2, "ML-KEM-768", from_slot=True, slot=0),
                      bytes(1088), limit=5000)
    assert r is None, "参数集不匹配的槽居然跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & (1 << 5), "参数集不匹配应当报 PARAM_ERR"

    dut._log.info("空槽与参数集不匹配都在 START 处被判掉，没有跑到超时")


# ============================================================================
# 安全世界暂存的种子（CODE-1 的 PL 侧落点）
# ============================================================================
# 判据分四条，缺一条这个口就是装饰品（RTL 文件头里逐条写了理由）：
#   ① 用暂存的种子跑出来的 ek‖dk，与"把同一份 d‖z 灌 IN_DATA"逐字节相同
#      —— 证明它真的走进了核，而不是"看起来跑通了"；
#   ② **非安全事务（AxPROT[1]=1）写种子口被拒**，且被拒计数涨；
#   ③ **读种子口恒为 0**，SEED_STAT 里一个种子字节都没有；
#   ④ **用一次即作废** —— 同一份暂存不能生第二把密钥。
SEED_DATA = 0x38
SEED_STAT = 0x3C
C_SEED_LOCK = 1 << 5
C_SEED_CLR  = 1 << 6
M_SEED_STAGED = 1 << 10
ST_SEED_ERR = 1 << 6
SS_READY = 1 << 8
SS_LOCK  = 1 << 9


async def stage_seed(dut, seed64: bytes, prot=PROT_SECURE):
    """把 64 字节 d‖z 按 16 个小端 32 位字写进暂存口"""
    assert len(seed64) == 64
    for i in range(16):
        w = int.from_bytes(seed64[4 * i:4 * i + 4], "little")
        r = await wr(dut, SEED_DATA, w, prot=prot)
        if prot != PROT_SECURE:
            return r
    return RESP_OKAY


@cocotb.test()
async def test_staged_seed_matches_in_data_path(dut):
    """①：暂存口送的 d‖z 与 IN_DATA 送的同一份，结果逐字节相同

    这一条是整个改动的"它真的接上了吗"。只查"跑出来了"不够 —— 得跟
    IN_DATA 那条已经被 ACVP 钉死的路做逐字节对拍，否则字节序装反、
    或者核其实读的是残留，都能"跑出来"。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d = bytes(range(0x10, 0x30))
    z = bytes(range(0x30, 0x50))
    ek_ref, dk_ref = mlkem_keygen(d, z, name)

    # 先走老路，确认参照
    old = await run_op(dut, M_KEYGEN, name, d + z)
    assert old == ek_ref + dk_ref, "IN_DATA 这条老路自己就不对，后面没法比"

    # 再走暂存口：**一个字节都不写 IN_DATA**
    await stage_seed(dut, d + z)
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 16, f"SEED_STAT 字计数 = {ss & 0x1F}，应当是 16"
    assert ss & SS_READY, "16 个字齐了却没报 READY"

    new = await run_raw(dut, mode_word(M_KEYGEN, name) | M_SEED_STAGED, b"")
    assert new is not None, "走暂存种子的 KeyGen 被拒了"
    assert new == old, "暂存口与 IN_DATA 两条路结果不一致 —— 多半是字节序"

    dut._log.info("暂存口送的 d‖z 与 IN_DATA 逐字节等价（%d 字节输出）" % len(new))


@cocotb.test()
async def test_seed_port_refuses_nonsecure(dut):
    """②：非安全事务写不进种子口 —— **两种 SECURE_ONLY 形态下都不行**

    这条要分形态说清楚，因为把关的不是同一个东西：

      · `SECURE_ONLY=1`（送检位流）：整块从机的**防火墙**先把非安全访问
        RAZ/WI 掉了，根本轮不到种子口自己那道门。响应是 OKAY + 写丢弃。
      · `SECURE_ONLY=0`（演示位流）：防火墙对普通世界是敞开的，这时**只剩
        种子口自己那道门**——它回 SLVERR 并把被拒计数推上去。

    也就是说第二种形态才真正测到本次新增的那道门。默认参数只跑得到第一种，
    所以 `tools/rtl_sim.sh` 里专门加了一条 `SECURE_ONLY=0` 的运行
    （`make PARAM_SECURE_ONLY=0`）。少了那一条，这道门等于没测。

    **两种形态共同的、也是真正要证的那句话：字计数一步都不动。**
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    secure_only = int(os.environ.get("PARAM_SECURE_ONLY", "1"))

    ss0, _ = await rd(dut, SEED_STAT)
    assert (ss0 >> 16) == 0, "复位后被拒计数就不是 0"

    for i in range(3):
        r = await wr(dut, SEED_DATA, 0xDEADBEEF + i, prot=PROT_NONSEC)
        if secure_only:
            # 防火墙的 RAZ/WI：不产生总线错误（理由见文件头 RESP_REFUSED）
            assert r == RESP_REFUSED, f"第 {i} 笔回了 {r}"
        else:
            assert r == RESP_SLVERR, \
                f"演示形态下非安全写种子口回了 {r}，应当是 SLVERR"

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 0, "被拒的写居然把字计数推上去了 —— 种子口漏了"

    if secure_only:
        # 防火墙拦下的访问计在 A_VIOL，不计在种子口自己的计数上
        v, _ = await rd(dut, VIOL_CNT)
        assert (v & 0xFFFF) >= 3, f"防火墙的写违规计数 = {v & 0xFFFF}，应当 ≥3"
        assert (ss >> 16) == 0, "被防火墙拦下的访问不该计进种子口的计数"
    else:
        assert (ss >> 16) == 3, f"种子口被拒计数 = {ss >> 16}，应当是 3"

    # 无论哪种形态，被拒之后仍然能正常 staging（拒绝没有副作用）
    await stage_seed(dut, bytes([0x5A] * 64))
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY, "被拒的写留下了副作用，之后 staging 不成了"

    dut._log.info("SECURE_ONLY=%d：非安全写种子口进不去，字计数一步没动",
                  secure_only)


@cocotb.test()
async def test_seed_port_never_reads_back(dut):
    """③：读种子口恒 0，SEED_STAT 里一个种子字节都没有

    判据是"读回 0"而不是"读被拒"：种子口压根没有读回路径 —— RTL 的读
    case 里就没有任何一条把 seed_stage 接到 f_rdata 上。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 用一份每个字节都不同的种子，任何一处泄漏都会长得很显眼
    seed = bytes((0x80 + i) & 0xFF for i in range(64))
    await stage_seed(dut, seed)

    for _ in range(4):
        v, resp = await rd(dut, SEED_DATA)
        assert resp == RESP_OKAY and v == 0, \
            f"读 SEED_DATA 得到 0x{v:08x} —— 种子口不该有任何读回路径"

    # SEED_STAT 低半字只该有字数与标志位，与种子内容无关
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xFFFF) == (16 | SS_READY), \
        f"SEED_STAT 低半字 = 0x{ss & 0xFFFF:04x}，只该有字数与标志"

    # 扫一遍**整个寄存器窗口**：那 64 个字节的任何一个 32 位字都不许出现。
    # 只查 SEED_DATA/SEED_STAT 不够 —— 泄漏也可能从别的寄存器溢出来。
    words = {int.from_bytes(seed[4 * i:4 * i + 4], "little") for i in range(16)}
    for off in range(0x00, 0x40, 4):
        # 0x18 OUT_DATA 跳过：它是输出缓冲 BRAM 的读口，这一刻还没写过任何
        # 东西，Icarus 下读出的是 X（不是 0），int() 转不了。它装的是核的
        # 输出字节，与种子暂存是两块存储 —— dk 那条路由 test_dk_stays_on_chip
        # 把关。为这一条把整个扫描去掉才是因噎废食。
        if off == OUT_DATA:
            continue
        v, _ = await rd(dut, off)
        assert v not in words, \
            f"寄存器 0x{off:02x} 读出 0x{v:08x}，那是种子的一个字"

    dut._log.info("整个寄存器窗口扫过：种子的 16 个字一个都读不到")


@cocotb.test()
async def test_staged_seed_is_consumed_once(dut):
    """④：一份暂存种子只能生一把密钥

    不这么做的话，安全世界那边早就把种子忘了（EL3 的栈上擦掉了），
    普通世界却还能拿着 PL 里那份残留反复生成 —— 那等于把"算完即弃"
    换成了"PL 替你记着"。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    seed = bytes(range(64))
    await stage_seed(dut, seed)

    # ---- 「START **当场**清」而不是「跑完才清」----
    # 这两者的差别不是洁癖：清在 S_FIN 的话，一次半途被 zeroize / 被参数错
    # 打断的运行会把种子**留在暂存里**，而安全世界那边已经把它忘了。
    # 判据只能在 BUSY 期间查 —— 跑完再查，两种实现看起来一模一样。
    assert await wr(dut, MODE,
                    mode_word(M_KEYGEN, name) | M_SEED_STAGED) == RESP_OKAY
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_BUSY, "START 之后立刻查，居然已经不 BUSY 了 —— 判据失效"
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 0 and not (ss & SS_READY), \
        f"运行途中 SEED_STAT=0x{ss:08x} —— 暂存不是在 START 那一刻清的"

    for _ in range(400_000):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
    else:
        raise AssertionError("走暂存种子的 KeyGen 一直没完成")
    n, _ = await rd(dut, OUT_LEN)
    assert n > 0, "跑完了却没有输出"

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 0, "用过之后暂存的字计数没清"
    assert not (ss & SS_READY), "用过之后还报 READY"

    # 第二次：没有备好的种子 → 当场拒绝，且点亮 SEED_ERR
    second = await run_raw(dut, mode_word(M_KEYGEN, name) | M_SEED_STAGED,
                           b"", limit=5000)
    assert second is None, "同一份暂存种子居然生出了第二把密钥"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR, "种子没备好却没报 SEED_ERR"
    assert st & ST_PARAMERR, "SEED_ERR 时 PARAM_ERR 这个总括位也该亮"
    assert not (st & ST_BUSY), "被拒之后不该还 BUSY"

    dut._log.info("暂存种子用一次即作废，第二次 START 报 SEED_ERR")


@cocotb.test()
async def test_seed_clr_and_partial_seed_refused(dut):
    """半份种子不许开跑；SEED_CLR 能把暂存作废

    半份种子是"喂不满让 z=0"那条老坑的新入口：只写 8 个字就 START 的话，
    另一半是残留（冷启动全 0），出来的密钥对**看起来完全合法**。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    # 只写 8 个字
    for i in range(8):
        assert await wr(dut, SEED_DATA, 0x01020304 + i) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 8 and not (ss & SS_READY)

    r = await run_raw(dut, mode_word(M_KEYGEN, name) | M_SEED_STAGED,
                      b"", limit=5000)
    assert r is None, "半份种子居然跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR, "半份种子应当报 SEED_ERR"

    # 补满 → 能跑
    for i in range(8):
        assert await wr(dut, SEED_DATA, 0x0A0B0C0D + i) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY

    # 收满之后再写：SLVERR，不静默吃掉
    assert await wr(dut, SEED_DATA, 0xFFFFFFFF) == RESP_SLVERR, \
        "收满之后的多余写应当回 SLVERR"

    # SEED_CLR 作废
    assert await wr(dut, CTRL, C_SEED_CLR) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 0 and not (ss & SS_READY), "SEED_CLR 没把暂存清掉"

    dut._log.info("半份种子被拒；收满后多写回 SLVERR；SEED_CLR 生效")


@cocotb.test()
async def test_seed_lock_is_one_way_and_independent_of_dk_lock(dut):
    """SEED_LOCK：置上之后 MODE 说了不算；zeroize 撤不回；**不连动 DK_LOCK**

    最后那半条是有意验的：DK_LOCK 与 SEED_LOCK 守的方向相反
    （FINAL-PLAN §7 V-04 要删掉前者），所以两者必须互相独立。
    一旦有人"顺手"把它们连起来，这条用例会立刻发现。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x61] * 32), bytes([0x62] * 32)

    # 闩锁之前：不置 SEED_STAGED 就走 IN_DATA（ACVP 那条路要留着）
    out = await run_op(dut, M_KEYGEN, name, d + z)
    assert out == b"".join(mlkem_keygen(d, z, name)), "闩锁之前老路就不对"

    assert await wr(dut, CTRL, C_SEED_LOCK) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 17), "SEED_LOCK 没置上"
    assert not (ks & (1 << 16)), \
        "SEED_LOCK 顺手把 DK_LOCK 也置上了 —— 两把闩方向相反，不能连动"
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_LOCK

    # 闩锁之后：**同一个不带 SEED_STAGED 的 MODE 字**也必须走暂存口，
    # 于是没备好种子就被拒 —— 这正是"MODE 说了不算"的判据。
    r = await run_raw(dut, mode_word(M_KEYGEN, name), d + z, limit=5000)
    assert r is None, "闩锁之后 MODE 清零居然还能退回 IN_DATA 那条路"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR

    # 备好种子就能跑
    await stage_seed(dut, d + z)
    r = await run_raw(dut, mode_word(M_KEYGEN, name), b"")
    assert r is not None and len(r) > 0

    # 撤不回来
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 17), "zeroize 把 SEED_LOCK 清掉了 —— 它不该有这个能力"

    dut._log.info("SEED_LOCK 一次性生效、zeroize 撤不回、且与 DK_LOCK 互相独立")


@cocotb.test()
async def test_zeroize_wipes_staged_seed(dut):
    """zeroize 要把暂存的种子一起擦掉 —— 它也是秘密

    判据不是"SEED_STAT 的字计数变 0"（那只是撕目录页），而是**擦完之后
    补满一份新的、跑出来的密钥对与旧种子无关**。这里用更直接的一条：
    擦完之后 READY 落下，且直接 START 会因为没种子被拒。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    await stage_seed(dut, bytes([0x77] * 64))
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x1F) == 0 and not (ss & SS_READY), \
        "zeroize 之后暂存的种子还在"
    r = await run_raw(dut, mode_word(M_KEYGEN, name) | M_SEED_STAGED,
                      b"", limit=5000)
    assert r is None, "zeroize 之后居然还能拿残留的种子生成密钥"

    dut._log.info("zeroize 把暂存的种子一起擦掉了")


@cocotb.test()
async def test_mode_reads_back_all_fields(dut):
    """MODE 整字回读（DOC-3）

    原来只回读 mode/pset，DK_TO_SLOT / DK_FROM_SLOT / SLOT 写进去就再也
    读不回来 —— 驱动没法核对自己写对了没有。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for word in (mode_word(M_DECAPS, "ML-KEM-1024", from_slot=True, slot=13),
                 mode_word(M_KEYGEN, "ML-KEM-768", to_slot=True, slot=5)
                 | M_SEED_STAGED,
                 mode_word(M_ENCAPS, "ML-KEM-512")):
        assert await wr(dut, MODE, word) == RESP_OKAY
        got, _ = await rd(dut, MODE)
        assert got == word, f"MODE 回读 0x{got:03x}，写进去的是 0x{word:03x}"

    dut._log.info("MODE 的 11 个位全部可回读")


# ============================================================================
# 核内 BRAM 的擦除（登记表 Z-03 / Z-04）—— 这以前**根本没有**
# ============================================================================
# 上一版的 zeroize 只擦 mlkem_axi 自己那两块 8 KB 缓冲与 16 槽金库。三个核
# **内部**的多项式银行（ŝ、ê、Â、重加密中间态）一个字节都没擦 —— 那些核连
# zeroize 端口都没有。于是每跑完一次 KeyGen/Decaps，私钥系数就原样留在核里，
# 直到下一次运算碰巧覆盖同一个地址为止，而 ZEROIZE 报告"擦完了"。
#
# 判据只能是**直接读那块存储**（cocotb 的层次化访问），不能靠 STATUS：
# 一个只把 WIPING 拉一下就落下的假实现，从寄存器看和真的一模一样。
# 这与本文件上面 test_zeroize_really_wipes_bram 的口径一致 ——
# "OUT_LEN 变 0"只证明目录页被撕了。
@cocotb.test()
async def test_zeroize_wipes_core_internal_bram(dut):
    """跑一次 KeyGen 让核内多项式银行装满，ZEROIZE 之后逐字读回必须全 0"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d, z = bytes([0x5C] * 32), bytes([0x6D] * 32)

    # 先跑一次：核内的多项式银行这时装着 ŝ / ê / Â（都是私钥派生量）
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == b"".join(mlkem_keygen(d, z, name)), "KeyGen 这一步就不对"

    banks = (("u_kg", dut.u_kg.u_bank.mem),
             ("u_en", dut.u_en.u_bank.mem),
             ("u_de", dut.u_de.u_bank.mem))

    # 反证：KeyGen 核那块**必须**先是脏的，否则这条用例什么都没测
    bad, _ = _mem_nonzero(banks[0][1])
    assert bad > 0, (
        "KeyGen 跑完之后核内多项式银行居然是全 0 —— 这条用例失去意义了"
        "（多半是层次路径写错，读到了别的东西）")
    dut._log.info("跑完 KeyGen：核内多项式银行有 %d 个非零字，确认是脏的", bad)

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    for label, mem in banks:
        bad, first = _mem_nonzero(mem)
        assert bad == 0, (
            f"{label} 的核内多项式银行擦除之后还有 {bad}/{len(mem)} 个非零，"
            f"第一个在 [{first[0]}] = 0x{first[1]:04x} —— "
            "展开态的私钥系数还留在 PL 里")

    # 擦完之后还能照常再跑（擦除不能把核弄坏）
    got = await run_op(dut, M_KEYGEN, name, d + z)
    assert got == b"".join(mlkem_keygen(d, z, name)), "擦除之后再跑结果不对"

    dut._log.info("三个核的内部多项式银行 zeroize 后逐字读回，全为 0")


@cocotb.test()
async def test_wiping_covers_the_cores_not_just_this_layer(dut):
    """STATUS.WIPING 必须**盖住核里那几台擦除机**，不能只报本层那台

    这一条单列，是因为漏掉它的后果很具体：软件轮询到 WIPING 落下就去发下一次
    START，而核里还在擦 —— 那一趟运算读到的是半擦的多项式银行，出来的是一个
    **看起来完全合法**的错结果。ML-DSA 那边同样的坑写在 mldsa_axi 的
    wiping_any 上。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY

    # 采样 WIPING 的持续时间。本层那台擦 64 KB 金库要 65536 拍；核里那三台
    # 各自 4096/2048 字。判据不是"恰好多少拍"（那会把用例钉死在实现细节上），
    # 而是**核的 wiping 确实被算进去了** —— 直接看那三根线。
    saw_core_wiping = False
    for _ in range(80000):
        if int(dut.core_wiping.value):
            saw_core_wiping = True
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            break
    else:
        raise AssertionError("WIPING 一直不落")

    assert saw_core_wiping, (
        "整个擦除过程里三个核的 wiping 一次都没起来 —— "
        "核内 BRAM 根本没被擦，STATUS.WIPING 报的只是本层那台")
    # 落下的那一刻，核里也必须已经擦完
    assert int(dut.core_wiping.value) == 0, \
        "本层报擦完了，核里还在擦 —— 下一次 START 会读到半擦的存储"

    dut._log.info("WIPING 覆盖了核里那三台擦除机，且三台都落下之后才报完成")
