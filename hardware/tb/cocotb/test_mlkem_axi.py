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


async def _wait_wipe(dut, limit=4000):
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
    _, r = await rd(dut, OUT_DATA)
    assert r == RESP_DECERR, "tamper 之后 OUT_DATA 还能读"

    # 等擦完（tamper 后总线关了，只能看内部的 wiping —— 这一处是唯一
    # 无法从软件侧观测的，因为软件侧此时已经被整体拒绝了）
    for _ in range(9000):
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
