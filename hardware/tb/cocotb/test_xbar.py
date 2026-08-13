"""cocotb：AXI4-Lite 地址译码（axi4lite_xbar）

这个模块以前**没有任何对拍**，而它恰好是"地址映射"这份文档的唯一执行者。
结果是一个安静的硬伤：第一版只用 addr[18:16] 选槽、只把 addr[7:0] 交给从机，
中间的 addr[15:8] 与 aperture 以上的高位整个被丢掉 —— 于是每个寄存器都有
成千上万个镜像地址。

**这里的判据不是"回没回 DECERR"，而是"从机有没有被碰到"。**
只看响应码是抓不到这个 bug 的：镜像地址在旧版里**也会**返回 OKAY，因为它
确实读到了那个寄存器。真正要证明的是「除了唯一那个地址，没有别的地址能
让下游从机看见一笔事务」，所以每条用例都带一个从机侧的监视器，
断言 m_awvalid / m_arvalid 在整笔事务里**一次都没有拉高过**。

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

RESP_OKAY, RESP_DECERR = 0, 3


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
    返回 OKAY。这里要求它 DECERR，**并且从机一次都没被碰到**。
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
        assert resp == RESP_DECERR, \
            f"写镜像地址 0x{addr:08x} 应当 DECERR，得到 {resp}"
        assert rec.touched() == 0, \
            f"写镜像地址 0x{addr:08x} 竟然到了从机：{rec.writes}"

        resp, data = await rd(dut, addr)
        assert resp == RESP_DECERR, \
            f"读镜像地址 0x{addr:08x} 应当 DECERR，得到 {resp}"
        assert rec.touched() == 0, \
            f"读镜像地址 0x{addr:08x} 竟然到了从机：{rec.reads}"
        assert data == 0, f"读镜像地址 0x{addr:08x} 还返回了数据 0x{data:08x}"

    dut._log.info(f"{len(aliases)} 个镜像地址全部 DECERR，从机零次被访问")


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
        assert await wr(dut, addr, 0x22222222) == RESP_DECERR, \
            f"写 aperture 外 0x{addr:08x} 应当 DECERR"
        assert rec.touched() == 0, f"写 0x{addr:08x} 到了从机：{rec.writes}"
        resp, _ = await rd(dut, addr)
        assert resp == RESP_DECERR, f"读 aperture 外 0x{addr:08x} 应当 DECERR"
        assert rec.touched() == 0, f"读 0x{addr:08x} 到了从机：{rec.reads}"

    dut._log.info(f"{len(outside)} 个 aperture 外地址全部 DECERR")


@cocotb.test()
async def test_slot_out_of_range(dut):
    """③ 槽号 ≥ NS：这一条旧版就是对的，留着防回归"""
    rec = await setup(dut)

    for slot in range(NS, 8):
        addr = BASE + (slot << 16)
        rec.writes.clear()
        rec.reads.clear()
        assert await wr(dut, addr, 0x33333333) == RESP_DECERR, \
            f"写不存在的槽 {slot} 应当 DECERR"
        assert rec.touched() == 0
        resp, _ = await rd(dut, addr)
        assert resp == RESP_DECERR, f"读不存在的槽 {slot} 应当 DECERR"
        assert rec.touched() == 0

    dut._log.info(f"槽 {NS}..7 全部 DECERR")


@cocotb.test()
async def test_unaligned(dut):
    """④ 未对齐：0x…05 在旧版里读到的是 0x…04 那个寄存器"""
    rec = await setup(dut)

    for off in (0x01, 0x05, 0x06, 0x07, 0x0F):
        addr = BASE + (2 << 16) + off
        rec.writes.clear()
        rec.reads.clear()
        assert await wr(dut, addr, 0x44444444) == RESP_DECERR, \
            f"写未对齐 0x{addr:08x} 应当 DECERR"
        assert rec.touched() == 0, f"写 0x{addr:08x} 到了从机：{rec.writes}"
        resp, _ = await rd(dut, addr)
        assert resp == RESP_DECERR, f"读未对齐 0x{addr:08x} 应当 DECERR"
        assert rec.touched() == 0, f"读 0x{addr:08x} 到了从机：{rec.reads}"

    dut._log.info("未对齐地址全部 DECERR")


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

    for off in range(0, 0x1_0000, 4):
        rec.reads.clear()
        resp, _ = await rd(dut, BASE + (slot << 16) + off)
        if rec.reads:
            reachable.append(off)
            assert resp == RESP_OKAY, f"到了从机却回 {resp}：off=0x{off:04x}"
        else:
            assert resp == RESP_DECERR, \
                f"没到从机却回 OKAY：off=0x{off:04x}"

    assert reachable == list(range(0, 0x100, 4)), (
        f"可达地址集合不对：{len(reachable)} 个，"
        f"头尾 {reachable[:4]}…{reachable[-4:]}")
    dut._log.info(f"槽 {slot} 的 16384 个字地址里，只有 {len(reachable)} 个可达"
                  f"（0x00..0xFC），其余全部 DECERR")
