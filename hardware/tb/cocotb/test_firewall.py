"""cocotb：AXI4-Lite 防火墙 —— 重点是 tamper 的**同拍**行为

防火墙以前只作为别的模块的一部分被间接测到（mlkem_axi / key_vault_axi /
pqc_accel_axi 里各有一份），那些用例验的是"tamper 之后被拒"。
"之后"这两个字里藏着一拍：

    第 N 拍：tamper 拉高。tamper_latched 这时**还是 0**（要下一个沿才更新）。
             如果判据只看 tamper_latched，这一拍到达的事务照常放行。
    第 N+1 拍：tamper_latched 变 1，从此全拒。

窗口只有一拍，但它不是小概率事件 —— 篡改检测（开盖、电压/温度越界）与总线
事务是两条互不相干的时间线，攻击者可以主动去凑：拔盖的那一刻正在扫描寄存器，
就会有一笔访问踩在这一拍上。

所以这里的用例分三档，缺一不可：
  · **同拍**（tamper 与握手同一拍）—— 这一档是新的，旧 RTL 在这里会放行；
  · **早一拍**（tamper 先于握手）—— 旧 RTL 也拒，留作对照，
    证明同拍那一档的失败确实来自"同拍"，而不是整条 tamper 路径都坏了；
  · **正例**（不 tamper 的合法事务）—— 证明这条路本来是通的。

判据同样是"下游有没有被碰到"，不是"回了什么响应码"。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

RESP_OKAY, RESP_DECERR = 0, 3
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
PROT_SECURE, PROT_NONSEC = 0b000, 0b010


class Down:
    """下游（被保护的从机）侧的记录仪"""

    def __init__(self):
        self.writes = []
        self.reads = []

    def touched(self):
        return len(self.writes) + len(self.reads)


async def down_model(dut, rec):
    dut.m_awready.value = 1
    dut.m_wready.value = 1
    dut.m_arready.value = 1
    dut.m_bvalid.value = 0
    dut.m_bresp.value = 0
    dut.m_rvalid.value = 0
    dut.m_rresp.value = 0
    dut.m_rdata.value = 0
    bpend = rpend = 0
    while True:
        await Timer(1, unit="ns")
        awv, wv = int(dut.m_awvalid.value), int(dut.m_wvalid.value)
        arv = int(dut.m_arvalid.value)
        if awv:
            rec.writes.append((int(dut.m_awaddr.value), int(dut.m_wdata.value)))
        if arv:
            rec.reads.append(int(dut.m_araddr.value))
        if bpend and int(dut.m_bready.value):
            bpend = 0
        if rpend and int(dut.m_rready.value):
            rpend = 0
        if awv and wv:
            bpend = 1
        if arv:
            rpend = 1
        await RisingEdge(dut.clk)
        dut.m_bvalid.value = bpend
        dut.m_rvalid.value = rpend
        dut.m_rdata.value = 0xD0D0BEEF


async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
    dut.s_awaddr.value = 0
    dut.s_awprot.value = PROT_SECURE
    dut.s_awvalid.value = 0
    dut.s_wdata.value = 0
    dut.s_wstrb.value = 0xF
    dut.s_wvalid.value = 0
    dut.s_bready.value = 1
    dut.s_araddr.value = 0
    dut.s_arprot.value = PROT_SECURE
    dut.s_arvalid.value = 0
    dut.s_rready.value = 1
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def setup(dut):
    rec = Down()
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    cocotb.start_soon(down_model(dut, rec))
    await reset(dut)
    return rec


async def await_bresp(dut):
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_bvalid.value):
            r = int(dut.s_bresp.value)
            await RisingEdge(dut.clk)
            return r
        await RisingEdge(dut.clk)
    raise AssertionError("写事务没有响应")


async def await_rresp(dut):
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_rvalid.value):
            r, d = int(dut.s_rresp.value), int(dut.s_rdata.value)
            await RisingEdge(dut.clk)
            return r, d
        await RisingEdge(dut.clk)
    raise AssertionError("读事务没有响应")


async def write_with_tamper_at(dut, addr, data, tamper_offset):
    """发一笔写；tamper_offset 是相对握手拍的偏移

    偏移 0 = **与握手同一拍**（这是关键的那一档）
    偏移 -1 = 早一拍
    偏移 None = 全程不 tamper
    """
    if tamper_offset == -1:
        dut.tamper.value = 1
        await RisingEdge(dut.clk)
        dut.tamper.value = 0

    dut.s_awaddr.value = addr
    dut.s_awvalid.value = 1
    dut.s_wdata.value = data
    dut.s_wvalid.value = 1
    if tamper_offset == 0:
        # AW/W 两条通道在 W_IDLE 里都是 ready 的，所以下一个沿就是握手拍。
        # 与 valid 同时把 tamper 抬起来 —— 判据在这个沿上取样。
        dut.tamper.value = 1

    await RisingEdge(dut.clk)
    dut.s_awvalid.value = 0
    dut.s_wvalid.value = 0
    if tamper_offset == 0:
        dut.tamper.value = 0

    return await await_bresp(dut)


async def read_with_tamper_at(dut, addr, tamper_offset):
    if tamper_offset == -1:
        dut.tamper.value = 1
        await RisingEdge(dut.clk)
        dut.tamper.value = 0

    dut.s_araddr.value = addr
    dut.s_arvalid.value = 1
    if tamper_offset == 0:
        dut.tamper.value = 1

    await RisingEdge(dut.clk)
    dut.s_arvalid.value = 0
    if tamper_offset == 0:
        dut.tamper.value = 0

    return await await_rresp(dut)


# ---------------------------------------------------------------------------


@cocotb.test()
async def test_legal_transaction_passes(dut):
    """正例：secure 的读写照常穿过去，落在下游正确的地址上"""
    rec = await setup(dut)

    assert await write_with_tamper_at(dut, 0x10, 0xCAFEBABE, None) == RESP_OKAY
    assert rec.writes == [(0x10, 0xCAFEBABE)], f"写没穿过去：{rec.writes}"

    rec.writes.clear()
    resp, data = await read_with_tamper_at(dut, 0x14, None)
    assert resp == RESP_OKAY
    assert rec.reads == [0x14], f"读没穿过去：{rec.reads}"
    assert data == 0xD0D0BEEF

    dut._log.info("合法事务照常穿过防火墙")


@cocotb.test()
async def test_nonsecure_refused(dut):
    """对照：AxPROT[1]=1 被拒，且下游没被碰到"""
    rec = await setup(dut)

    dut.s_awprot.value = PROT_NONSEC
    dut.s_arprot.value = PROT_NONSEC
    assert await write_with_tamper_at(dut, 0x10, 1, None) == RESP_REFUSED
    resp, data = await read_with_tamper_at(dut, 0x10, None)
    assert resp == RESP_REFUSED and data == 0
    assert rec.touched() == 0, f"non-secure 到了下游：{rec.writes} {rec.reads}"

    dut._log.info("non-secure 被拒，下游零次被访问")


@cocotb.test()
async def test_tamper_one_cycle_early_write(dut):
    """对照档：tamper 早一拍 —— 旧 RTL 也拒，用来隔离变量"""
    rec = await setup(dut)
    assert await write_with_tamper_at(dut, 0x10, 0x1234, -1) == RESP_REFUSED
    assert rec.touched() == 0, f"tamper 早一拍还是到了下游：{rec.writes}"
    dut._log.info("tamper 早一拍：拒，且下游没被碰到")


@cocotb.test()
async def test_tamper_same_cycle_write(dut):
    """关键档：tamper 与 AW/W 握手**同一拍**

    旧 RTL 在这里放行 —— permit() 只看 tamper_latched，而它这一拍还是 0。
    """
    rec = await setup(dut)
    resp = await write_with_tamper_at(dut, 0x10, 0xDEADBEEF, 0)
    assert rec.touched() == 0, (
        f"tamper 同拍的写**穿过了防火墙**，下游收到 {rec.writes} —— "
        "这就是那一拍宽的 TOCTOU 窗口")
    assert resp == RESP_REFUSED, f"tamper 同拍的写回了 {resp}，应当 DECERR"
    dut._log.info("tamper 同拍的写：拒，下游零次被访问")


@cocotb.test()
async def test_tamper_same_cycle_read(dut):
    """关键档：tamper 与 AR 握手同一拍

    读比写更要紧一点：写是"改坏了什么"，读是"泄露了什么"。
    """
    rec = await setup(dut)
    resp, data = await read_with_tamper_at(dut, 0x14, 0)
    assert rec.touched() == 0, (
        f"tamper 同拍的读**穿过了防火墙**，下游被读了 {rec.reads} —— "
        "被保护的寄存器在这一拍里是可读的")
    assert resp == RESP_REFUSED, f"tamper 同拍的读回了 {resp}，应当 DECERR"
    assert data == 0, f"tamper 同拍的读还带回了数据 0x{data:08x}"
    dut._log.info("tamper 同拍的读：拒，下游零次被访问，也没带回数据")


@cocotb.test()
async def test_tamper_latches_forever(dut):
    """锁存只进不出：同拍那一下之后，后续一切访问都拒"""
    rec = await setup(dut)

    await write_with_tamper_at(dut, 0x10, 0x1, 0)
    assert int(dut.tamper.value) == 0, "tamper 应当已经放掉了"
    assert int(dut.tamper_latched.value) == 1, "tamper 没被锁存"

    for _ in range(4):
        assert await write_with_tamper_at(dut, 0x20, 0x2, None) == RESP_REFUSED
        resp, _ = await read_with_tamper_at(dut, 0x20, None)
        assert resp == RESP_REFUSED
    assert rec.touched() == 0, f"锁存之后还有事务到了下游：{rec.writes} {rec.reads}"

    dut._log.info("tamper 锁存之后，后续读写全部被拒")
