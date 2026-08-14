"""cocotb：AXI4-Lite 地址译码（axi4lite_xbar）

这个模块以前**没有任何对拍**，而它恰好是"地址映射"这份文档的唯一执行者。
结果是一个安静的硬伤：第一版只用 addr[18:16] 选槽、只把 addr[7:0] 交给从机，
中间的 addr[15:8] 与 aperture 以上的高位整个被丢掉 —— 于是每个寄存器都有
成千上万个镜像地址。

**这里的判据不是"回了什么响应码"，而是"从机有没有被碰到"。**
只看响应码是抓不到这个 bug 的：镜像地址在旧版里**也会**返回 OKAY，因为它
确实读到了那个寄存器。真正要证明的是「除了唯一那个地址，没有别的地址能
让下游从机看见一笔事务」，所以每条用例都带一个从机侧的监视器，
断言 m_awvalid / m_arvalid 在整笔事务里**一次都没有拉高过**。

**这个判据在 RAZ/WI 改动里一个字都没改** —— 这正是当初就该这么写的原因。
译码器不命中时的响应从 DECERR 改成了 OKAY（读回 0、写丢弃，理由见 RTL
文件头：DECERR 的 posted 写会以 SError 打穿内核）。如果当初的用例写的是
"断言 resp == DECERR"，这次就得整个重写，而且重写之后还是在测响应码、
测不到"从机没被碰到"这件真正要紧的事。

响应码这一侧现在补两条更弱但仍然必要的断言：**读必须回 0**（各从机的
VERSION 都是非零常量，所以"读到 0"本身就是"没命中"的信号），以及
**decode_viol_count 必须涨**（RAZ/WI 之后这是唯一留下的痕迹）。

覆盖的四类不命中，对应 hit_of() 的四条：
  ① 槽内偏移的高位非零 —— 0x8001_0110（旧版：读到 0x10 那个寄存器，OKAY）
  ② aperture 以外        —— 0x8010_0004（旧版：槽号取到 0，落到槽 0）
  ③ 槽号 ≥ NS           —— 0x8005_0000（这一条旧版就是对的，防回归）
  ④ 未对齐               —— 0x8001_0005（旧版：读到 0x04 那个寄存器）
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

NS = 5                       # 与 axi4lite_xbar 的默认参数一致
BASE = 0x8000_0000
ALL_READY = (1 << NS) - 1

RESP_OKAY = 0
RESP_DECERR = 3   # 只留作对照：改 RAZ/WI 之后本模块永不返回它


def _lane(vec, slot, width):
    """从扁平打包的下游总线里取出第 slot 个从机那一段"""
    return (int(vec.value) >> (width * slot)) & ((1 << width) - 1)


def _lowest_set(v):
    return (v & -v).bit_length() - 1


class Slaves:
    """五个"永远 ready"的从机 + 一个记录仪

    记录仪是这份测试的核心：它把**每一次**下游 valid 都记下来，
    包括那些本不该发生的。
    """

    def __init__(self):
        self.writes = []     # (slot, offset, data)
        self.reads = []      # (slot, offset)

    def touched(self):
        return len(self.writes) + len(self.reads)


async def wr_slave(dut, rec):
    dut.m_awready.value = ALL_READY
    dut.m_wready.value = ALL_READY
    dut.m_bvalid.value = 0
    dut.m_bresp.value = 0
    pend = 0
    while True:
        # 边沿后 1 ns 采样：此时组合输出已稳定，取到的就是本拍生效的值
        await Timer(1, unit="ns")
        awv = int(dut.m_awvalid.value)
        wv = int(dut.m_wvalid.value)
        if awv:
            s = _lowest_set(awv)
            rec.writes.append((s, _lane(dut.m_awaddr, s, 8),
                               _lane(dut.m_wdata, s, 32)))
        if pend and (pend & int(dut.m_bready.value)):
            pend = 0
        if awv and wv:
            pend = awv
        await RisingEdge(dut.clk)
        dut.m_bvalid.value = pend


async def rd_slave(dut, rec):
    dut.m_arready.value = ALL_READY
    dut.m_rvalid.value = 0
    dut.m_rresp.value = 0
    dut.m_rdata.value = 0
    pend = 0
    data = 0
    while True:
        await Timer(1, unit="ns")
        arv = int(dut.m_arvalid.value)
        if arv:
            s = _lowest_set(arv)
            off = _lane(dut.m_araddr, s, 8)
            rec.reads.append((s, off))
            # 回一个能认出"是谁答的"的值：槽号 + 偏移
            data = (0xA5000000 | (s << 16) | off) << (32 * s)
        if pend and (pend & int(dut.m_rready.value)):
            pend = 0
        if arv:
            pend = arv
        await RisingEdge(dut.clk)
        dut.m_rvalid.value = pend
        dut.m_rdata.value = data


async def reset(dut):
    dut.rst_n.value = 0
    dut.s_awaddr.value = 0
    dut.s_awprot.value = 0
    dut.s_awvalid.value = 0
    dut.s_wdata.value = 0
    dut.s_wstrb.value = 0xF
    dut.s_wvalid.value = 0
    dut.s_bready.value = 1
    dut.s_araddr.value = 0
    dut.s_arprot.value = 0
    dut.s_arvalid.value = 0
    dut.s_rready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def wr(dut, addr, data):
    """一笔写，返回 bresp"""
    dut.s_awaddr.value = addr
    dut.s_awvalid.value = 1
    dut.s_wdata.value = data
    dut.s_wvalid.value = 1
    got_aw = got_w = False
    for _ in range(64):
        await Timer(1, unit="ns")
        if not got_aw and int(dut.s_awready.value):
            got_aw = True
        if not got_w and int(dut.s_wready.value):
            got_w = True
        await RisingEdge(dut.clk)
        if got_aw:
            dut.s_awvalid.value = 0
        if got_w:
            dut.s_wvalid.value = 0
        if got_aw and got_w:
            break
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_bvalid.value):
            resp = int(dut.s_bresp.value)
            await RisingEdge(dut.clk)
            return resp
        await RisingEdge(dut.clk)
    raise AssertionError(f"写 0x{addr:08x} 没有响应")


async def rd(dut, addr):
    """一笔读，返回 (rresp, rdata)"""
    dut.s_araddr.value = addr
    dut.s_arvalid.value = 1
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_arready.value):
            await RisingEdge(dut.clk)
            dut.s_arvalid.value = 0
            break
        await RisingEdge(dut.clk)
    else:
        raise AssertionError(f"读 0x{addr:08x} 没被接受")
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_rvalid.value):
            resp, data = int(dut.s_rresp.value), int(dut.s_rdata.value)
            await RisingEdge(dut.clk)
            return resp, data
        await RisingEdge(dut.clk)
    raise AssertionError(f"读 0x{addr:08x} 没有响应")


async def setup(dut):
    rec = Slaves()
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    cocotb.start_soon(wr_slave(dut, rec))
    cocotb.start_soon(rd_slave(dut, rec))
    await reset(dut)
    return rec


# ---------------------------------------------------------------------------


@cocotb.test()
async def test_exact_address_reaches_slave(dut):
    """正例：表里写的那个地址，确实落到对应从机的对应偏移上

    先立这条，否则后面所有"没落到从机"的断言都可能只是因为整条路是死的。
    """
    rec = await setup(dut)

    for slot in range(NS):
        for off in (0x00, 0x04, 0x7C):
            addr = BASE + (slot << 16) + off
            rec.writes.clear()
            rec.reads.clear()

            resp = await wr(dut, addr, 0xDEAD0000 | off)
            assert resp == RESP_OKAY, f"写 0x{addr:08x} 应当 OKAY，得到 {resp}"
            assert rec.writes == [(slot, off, 0xDEAD0000 | off)], \
                f"写 0x{addr:08x} 落错了地方：{rec.writes}"

            resp, data = await rd(dut, addr)
            assert resp == RESP_OKAY, f"读 0x{addr:08x} 应当 OKAY，得到 {resp}"
            assert rec.reads == [(slot, off)], \
                f"读 0x{addr:08x} 落错了地方：{rec.reads}"
            assert data == (0xA5000000 | (slot << 16) | off), \
                f"读 0x{addr:08x} 拿到的是别人的数据：0x{data:08x}"

    dut._log.info(f"{NS} 个槽 × 3 个偏移，读写都落在唯一正确的位置")


@cocotb.test()
async def test_offset_alias_never_reaches_slave(dut):
    """① 槽内偏移的高位非零 —— 旧版的主症状

    0x8001_0110 在旧版里就是 0x8001_0010：addr[15:8] 被丢掉，从机看到 0x10，
    返回 OKAY。这里要求它读回 0、写被丢弃，**并且从机一次都没被碰到**。
    """
    rec = await setup(dut)

    aliases = [
        BASE + (1 << 16) + 0x0110,     # 审计里点名的那个
        BASE + (1 << 16) + 0x0100,
        BASE + (1 << 16) + 0x1100,
        BASE + (3 << 16) + 0xFF00,
        BASE + (0 << 16) + 0x8000,
    ]
    for addr in aliases:
        rec.writes.clear()
        rec.reads.clear()

        resp = await wr(dut, addr, 0x11111111)
        assert resp == RESP_OKAY, \
            f"写镜像地址 0x{addr:08x} 应当静默丢弃并回 OKAY，得到 {resp}"
        assert rec.touched() == 0, \
            f"写镜像地址 0x{addr:08x} 竟然到了从机：{rec.writes}"

        resp, data = await rd(dut, addr)
        assert resp == RESP_OKAY, \
            f"读镜像地址 0x{addr:08x} 应当回 OKAY（RAZ/WI），得到 {resp}"
        assert data == 0, f"读镜像地址 0x{addr:08x} 应当回 0，得到 0x{data:08x}"
        assert rec.touched() == 0, \
            f"读镜像地址 0x{addr:08x} 竟然到了从机：{rec.reads}"
        assert data == 0, f"读镜像地址 0x{addr:08x} 还返回了数据 0x{data:08x}"

    dut._log.info(f"{len(aliases)} 个镜像地址全部读回 0（OKAY，无总线错误），从机零次被访问")


@cocotb.test()
async def test_outside_aperture(dut):
    """② aperture 以外：旧版把高位丢掉，0x8010_0004 会落到槽 0"""
    rec = await setup(dut)

    outside = [
        BASE + 0x0010_0004,    # 高位被丢 → 旧版落槽 0
        BASE + 0x0100_0000,
        BASE + 0x1000_0004,
        0x0000_0004,           # 根本不在窗口里
    ]
    for addr in outside:
        rec.writes.clear()
        rec.reads.clear()
        assert await wr(dut, addr, 0x22222222) == RESP_OKAY, \
            f"写 aperture 外 0x{addr:08x} 应当静默丢弃并回 OKAY"
        assert rec.touched() == 0, f"写 0x{addr:08x} 到了从机：{rec.writes}"
        resp, data = await rd(dut, addr)
        assert resp == RESP_OKAY, f"读 aperture 外 0x{addr:08x} 应当回 OKAY"
        assert data == 0, f"读 aperture 外 0x{addr:08x} 应当回 0，得到 0x{data:08x}"
        assert rec.touched() == 0, f"读 0x{addr:08x} 到了从机：{rec.reads}"

    dut._log.info(f"{len(outside)} 个 aperture 外地址全部读回 0，无总线错误")


@cocotb.test()
async def test_slot_out_of_range(dut):
    """③ 槽号 ≥ NS：这一条旧版就是对的，留着防回归"""
    rec = await setup(dut)

    for slot in range(NS, 8):
        addr = BASE + (slot << 16)
        rec.writes.clear()
        rec.reads.clear()
        assert await wr(dut, addr, 0x33333333) == RESP_OKAY, \
            f"写不存在的槽 {slot} 应当静默丢弃并回 OKAY"
        assert rec.touched() == 0
        resp, data = await rd(dut, addr)
        assert resp == RESP_OKAY, f"读不存在的槽 {slot} 应当回 OKAY"
        assert data == 0, f"读不存在的槽 {slot} 应当回 0，得到 0x{data:08x}"
        assert rec.touched() == 0

    dut._log.info(f"槽 {NS}..7 全部读回 0，无总线错误")


@cocotb.test()
async def test_unaligned(dut):
    """④ 未对齐：0x…05 在旧版里读到的是 0x…04 那个寄存器"""
    rec = await setup(dut)

    for off in (0x01, 0x05, 0x06, 0x07, 0x0F):
        addr = BASE + (2 << 16) + off
        rec.writes.clear()
        rec.reads.clear()
        assert await wr(dut, addr, 0x44444444) == RESP_OKAY, \
            f"写未对齐 0x{addr:08x} 应当静默丢弃并回 OKAY"
        assert rec.touched() == 0, f"写 0x{addr:08x} 到了从机：{rec.writes}"
        resp, data = await rd(dut, addr)
        assert resp == RESP_OKAY, f"读未对齐 0x{addr:08x} 应当回 OKAY"
        assert data == 0, f"读未对齐 0x{addr:08x} 应当回 0，得到 0x{data:08x}"
        assert rec.touched() == 0, f"读 0x{addr:08x} 到了从机：{rec.reads}"

    dut._log.info("未对齐地址全部读回 0，无总线错误")


@cocotb.test()
async def test_no_alias_sweep(dut):
    """把整个 64 KB 槽扫一遍：能到达从机的地址有且只有 64 个

    这是"地址映射唯一"这句话的直接证明，不是抽样。
    扫的是槽 1 的全部 65536 个字节地址中按 4 字节步进的那 16384 个，
    加上每 4 字节里的 3 个未对齐地址各抽一个 —— 全扫 65536 个太慢，
    而未对齐那一类已经由 test_unaligned 定点覆盖。
    """
    rec = await setup(dut)
    slot = 1
    reachable = []

    # 判据仍然是"从机有没有被碰到"（rec.reads），响应码只是佐证 ——
    # RAZ/WI 之后两种情况的响应码都是 OKAY，**光看响应码已经区分不了**。
    # 区分开它们的是数据：到达从机的读回从机给的值，没到达的一律回 0。
    for off in range(0, 0x1_0000, 4):
        rec.reads.clear()
        resp, data = await rd(dut, BASE + (slot << 16) + off)
        assert resp == RESP_OKAY, f"任何地址都不该有总线错误：off=0x{off:04x} 得到 {resp}"
        if rec.reads:
            reachable.append(off)
        else:
            assert data == 0, \
                f"没到从机却回了非零数据 0x{data:08x}：off=0x{off:04x}"

    assert reachable == list(range(0, 0x100, 4)), (
        f"可达地址集合不对：{len(reachable)} 个，"
        f"头尾 {reachable[:4]}…{reachable[-4:]}")
    dut._log.info(f"槽 {slot} 的 16384 个字地址里，只有 {len(reachable)} 个可达"
                  f"（0x00..0xFC），其余全部读回 0")


@cocotb.test()
async def test_decode_viol_count(dut):
    """译码违规计数器：RAZ/WI 之后这是唯一留下的痕迹

    改成 RAZ/WI 换来了"谁都崩不了板"，代价是**走错地址变安静了**：
    读回 0、写被吞，总线上什么都不剩。所以计数器不是锦上添花 ——
    没有它，"从没走错过"和"走错过一千次"在硅上长得一模一样。

    这条用例证明三件事：
      ① 命中的访问**不**计数（否则正常流量就把它冲爆了，计数毫无意义）；
      ② 读和写都计数（写是那条会打穿内核的路径，漏了它等于没测）；
      ③ 到 0xFFFF 饱和、不回绕（回绕的话攻击者刷 65536 次就把痕迹抹了）。
    """
    rec = await setup(dut)

    def viol():
        """读写两个计数器之和。分成两个不是为了信息量 —— 它们在两个不同的
        always 块里，一个 reg 让两个块驱动是多驱动，Vivado 会拒收
        （lint 和 Icarus 都不报，见 tools/rtl_lint.sh 文件头）。"""
        return (int(dut.decode_viol_wr_count.value)
                + int(dut.decode_viol_rd_count.value))

    assert viol() == 0, "复位后计数应当是 0"

    # ① 命中的访问不该计数
    for off in (0x00, 0x40, 0xFC):
        await rd(dut, BASE + (1 << 16) + off)
        await wr(dut, BASE + (1 << 16) + off, 0x5A5A5A5A)
    assert viol() == 0, f"命中的访问不该计数，却涨到 {viol()}"

    # ② 四类不命中，读写各一笔 —— 每笔都该 +1
    misses = [
        BASE + (1 << 16) + 0x110,   # 槽内偏移高位非零
        BASE + 0x0010_0004,         # aperture 外
        BASE + (NS << 16),          # 槽号 >= NS
        BASE + (1 << 16) + 0x05,    # 未对齐
    ]
    # ① 里那几笔是**该**到达从机的，先把记录清掉，否则下面那条
    # "零次到达" 会被它们误伤。
    rec.reads.clear(); rec.writes.clear()

    n = 0
    for a in misses:
        await rd(dut, a)
        n += 1
        assert viol() == n, \
            f"读 0x{a:08x} 之后计数应当是 {n}，得到 {viol()}"
        assert int(dut.decode_viol_rd_count.value) == n // 2 + n % 2, \
            "读该记在读计数器上"
        await wr(dut, a, 0xDEADBEEF)
        n += 1
        assert viol() == n, \
            f"写 0x{a:08x} 之后计数应当是 {n}，得到 {viol()}"
        assert int(dut.decode_viol_wr_count.value) == n // 2, \
            "写该记在写计数器上"

    assert not rec.reads and not rec.writes, "不命中的事务不该有任何一笔到达从机"
    dut._log.info(f"四类不命中 x 读写 = {n} 笔，计数器逐笔跟上，从机零次被访问")


@cocotb.test()
async def test_decode_viol_saturates(dut):
    """计数器到 0xFFFF 就停住，不回绕

    回绕等于给了一条抹痕迹的路：刷满 65536 次，计数回到原值，
    审计看不出任何异常。饱和之后停住则至少留下"溢出过"这个事实。

    直接把内部计数器预置到 0xFFFE，再打三笔 —— 逐笔跑 65535 次太慢，
    而要验的是边界行为，不是计到 65534 的过程。
    """
    rec = await setup(dut)
    # 顶层就是 xbar，直接预置这两个 reg
    dut.decode_viol_rd_count.value = 0xFFFE
    dut.decode_viol_wr_count.value = 0xFFFE
    await RisingEdge(dut.clk)
    bad = BASE + (NS << 16)

    await rd(dut, bad)
    assert int(dut.decode_viol_rd_count.value) == 0xFFFF
    await rd(dut, bad)
    assert int(dut.decode_viol_rd_count.value) == 0xFFFF, "读计数应当饱和在 0xFFFF"
    await wr(dut, bad, 0)
    assert int(dut.decode_viol_wr_count.value) == 0xFFFF
    await wr(dut, bad, 0)
    assert int(dut.decode_viol_wr_count.value) == 0xFFFF, \
        "写计数也必须饱和，不能回绕到 0"
    assert not rec.reads and not rec.writes
    dut._log.info("计数器在 0xFFFF 饱和，读写两条路径都不回绕")
