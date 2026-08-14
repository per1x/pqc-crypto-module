"""cocotb：TRNG 的 AXI4-Lite 从机 + AxPROT 安全门控

这个文件测两类东西，第二类才是重点。

一、**总线契约**：寄存器读回、STATUS 各位的含义、RDATA 读时弹出、
    参数回读口与 RTL 实际参数一致（驱动启动自测就靠这个口）。

二、**AxPROT 门控 —— "只让安全 master 访问"的硬件落点**：
    · non-secure（AxPROT[1]=1）的读写一律被拒 —— **读回 0、写不落笔**
      （RAZ/WI；为什么不是 DECERR，见 trng_axi.v 文件头）；
    · **被挡掉的读绝不能弹 FIFO**。这一条单列出来测，是因为它是最容易写错、
      后果又最实际的一个：如果被拒的读仍然弹出，普通世界虽然拿不到数，
      却获得了一个"反复读就能把熵池抽干"的手段 —— 拿不到随机数和让别人也
      拿不到随机数，是两个不同的攻击，后者一样致命。
      **RAZ/WI 之后这一条比以前更要紧**：响应码不再区分放行与拒绝，
      "没弹 FIFO" 成了唯一能证明"事务真的没往下走"的观测点。
    · secure（AxPROT[1]=0）的访问必须正常。少了这一条，一个恒为拒绝的
      实现也能通过前两条 —— 而 RAZ/WI 之后"恒为拒绝"和"工作正常但值是 0"
      在响应码上长得一模一样，所以这一条现在是必需的，不是补充。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

# 寄存器偏移，与 trng_axi.v 的表一致
CTRL, STATUS, RDATA, HEALTH = 0x00, 0x04, 0x08, 0x0C
VIOL = 0x38          # A_VIOL：{读违规[31:16], 写违规[15:0]}
APTIDX, STARTUP, BLOCKS, WORDS = 0x10, 0x14, 0x18, 0x1C
VERSION, PARAM0, PARAM1, PARAM2 = 0x20, 0x24, 0x28, 0x2C

ST_READY, ST_DATA_VALID, ST_ALARM = 1 << 0, 1 << 1, 1 << 2
ST_STARTUP_DONE, ST_ENABLED, ST_UNDERRUN = 1 << 5, 1 << 7, 1 << 8

PROT_SECURE = 0b000     # AxPROT[1]=0
PROT_NONSEC = 0b010     # AxPROT[1]=1

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

DECIM = 8
STARTUP_SAMPLES = 1024
RATE_BITS = 17 * 64


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


async def axi_read(dut, addr, prot=PROT_SECURE):
    """一次 AXI4-Lite 读，返回 (rdata, rresp)"""
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
            data = int(dut.s_axi_rdata.value)
            resp = int(dut.s_axi_rresp.value)
            await RisingEdge(dut.clk)
            return data, resp
        await RisingEdge(dut.clk)
    raise AssertionError(f"读 0x{addr:02x}：rvalid 一直不来")


async def axi_write(dut, addr, data, prot=PROT_SECURE):
    """一次 AXI4-Lite 写，返回 bresp"""
    dut.s_axi_awaddr.value = addr
    dut.s_axi_awprot.value = prot
    dut.s_axi_awvalid.value = 1
    dut.s_axi_wdata.value = data
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 1

    aw_done = w_done = False
    for _ in range(64):
        await Timer(1, unit="ns")
        take_aw = int(dut.s_axi_awready.value) and not aw_done
        take_w = int(dut.s_axi_wready.value) and not w_done
        await RisingEdge(dut.clk)
        if take_aw:
            aw_done = True
            dut.s_axi_awvalid.value = 0
        if take_w:
            w_done = True
            dut.s_axi_wvalid.value = 0
        if aw_done and w_done:
            break
    else:
        raise AssertionError(f"写 0x{addr:02x}：地址/数据通道没握上手")

    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_bvalid.value):
            resp = int(dut.s_axi_bresp.value)
            await RisingEdge(dut.clk)
            return resp
        await RisingEdge(dut.clk)
    raise AssertionError(f"写 0x{addr:02x}：bvalid 一直不来")


async def wait_data_ready(dut, limit):
    """等到 STATUS.DATA_VALID 拉高"""
    for _ in range(limit):
        st, resp = await axi_read(dut, STATUS)
        assert resp == RESP_OKAY
        if st & ST_DATA_VALID:
            return st
        for _ in range(200):
            await RisingEdge(dut.clk)
    raise AssertionError("等不到 DATA_VALID")


@cocotb.test()
async def test_readonly_registers(dut):
    """版本号与参数回读口：驱动启动自测靠它核对硬件里跑的到底是什么阈值"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    ver, resp = await axi_read(dut, VERSION)
    assert resp == RESP_OKAY
    assert ver == int(dut.VERSION.value), f"VERSION 读回 0x{ver:08x} 不对"

    p0, _ = await axi_read(dut, PARAM0)
    assert (p0 >> 24) & 0xFF == int(dut.DECIM.value)
    assert (p0 >> 16) & 0xFF == int(dut.NUM_RO.value)
    assert (p0 >> 8) & 0xFF == int(dut.RATE_LANES.value)
    assert p0 & 0xFF == int(dut.OUT_LANES.value)

    p1, _ = await axi_read(dut, PARAM1)
    assert p1 & 0xFFFF == int(dut.RCT_CUTOFF.value), "RCT 阈值回读不对"
    assert (p1 >> 16) & 0xFFFF == int(dut.APT_CUTOFF.value), "APT 阈值回读不对"

    p2, _ = await axi_read(dut, PARAM2)
    assert p2 & 0xFFFF == int(dut.APT_WINDOW.value)
    assert (p2 >> 16) & 0xFFFF == int(dut.STARTUP_SAMPLES.value)

    dut._log.info(f"参数回读一致：RCT={p1 & 0xFFFF} APT_W={p2 & 0xFFFF} "
                  f"APT_C={(p1 >> 16) & 0xFFFF} DECIM={(p0 >> 24) & 0xFF}")


@cocotb.test()
async def test_status_and_data_path(dut):
    """暖机 → DATA_VALID → 读 RDATA 拿到互不相同的随机字"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    st, _ = await axi_read(dut, STATUS)
    assert st & ST_ENABLED, "上电后 ENABLE 应为高（要靠它暖机）"
    assert not (st & ST_DATA_VALID), "启动检测都没过就已经有数据可读了"
    assert not (st & ST_ALARM)

    limit = (STARTUP_SAMPLES + RATE_BITS) * DECIM // 200 + 20
    st = await wait_data_ready(dut, limit)
    assert st & ST_STARTUP_DONE and st & ST_READY

    words = []
    for _ in range(8):
        st, _ = await axi_read(dut, STATUS)
        if not (st & ST_DATA_VALID):
            break
        w, resp = await axi_read(dut, RDATA)
        assert resp == RESP_OKAY
        words.append(w)

    assert len(words) >= 4, f"只读到 {len(words)} 个字"
    assert len(set(words)) == len(words), "读到了重复的字 —— RDATA 没有弹出"

    n, _ = await axi_read(dut, WORDS)
    assert n == len(words), f"WORDS 计数 {n} 与实际读出的 {len(words)} 不符"
    dut._log.info(f"读到 {len(words)} 个互不相同的随机字，首字 0x{words[0]:08x}")


@cocotb.test()
async def test_underrun_is_latched(dut):
    """FIFO 空时读 RDATA：返回 0 且锁存 UNDERRUN

    返回 0 是危险的 —— 不看状态就读的驱动会把 0 当随机数用。UNDERRUN 就是
    用来事后抓这种驱动 bug 的，所以它必须是锁存的、必须能被显式清掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    st, _ = await axi_read(dut, STATUS)
    assert not (st & ST_UNDERRUN), "复位后 UNDERRUN 就是高的"

    w, resp = await axi_read(dut, RDATA)      # 此刻 FIFO 必空（还在暖机）
    assert resp == RESP_OKAY and w == 0, f"空读返回了 0x{w:08x}"

    st, _ = await axi_read(dut, STATUS)
    assert st & ST_UNDERRUN, "空读之后 UNDERRUN 没有置位"

    # 隔几拍再看，必须还在（锁存）
    for _ in range(50):
        await RisingEdge(dut.clk)
    st, _ = await axi_read(dut, STATUS)
    assert st & ST_UNDERRUN, "UNDERRUN 自己消失了 —— 它必须是锁存的"

    assert await axi_write(dut, CTRL, 0b101) == RESP_OKAY   # ENABLE + CLEAR_ALARM
    st, _ = await axi_read(dut, STATUS)
    assert not (st & ST_UNDERRUN), "CLEAR_ALARM 没有清掉 UNDERRUN"
    dut._log.info("空读返回 0、UNDERRUN 锁存、CLEAR_ALARM 可清")


@cocotb.test()
async def test_nonsecure_access_is_refused(dut):
    """AxPROT[1]=1 的读写一律 DECERR"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.SECURE_ONLY.value) == 1, "这个用例要求 SECURE_ONLY=1"

    for addr, name in ((VERSION, "VERSION"), (STATUS, "STATUS"),
                       (RDATA, "RDATA"), (PARAM1, "PARAM1")):
        data, resp = await axi_read(dut, addr, prot=PROT_NONSEC)
        assert resp == RESP_REFUSED, f"non-secure 读 {name} 返回了 resp={resp}"
        assert data == 0, f"non-secure 读 {name} 返回了数据 0x{data:08x}"

    resp = await axi_write(dut, CTRL, 0x0, prot=PROT_NONSEC)
    assert resp == RESP_REFUSED, "non-secure 写 CTRL 没有被拒"

    # 而且写没有落笔：ENABLE 仍应为高
    st, _ = await axi_read(dut, STATUS)
    assert st & ST_ENABLED, "non-secure 的写虽然报了 DECERR，但把 ENABLE 关掉了"
    # 违规计数：RAZ/WI 之后总线上不留痕，这个计数器是唯一的证据，
    # 而且它自己也只有 secure 读得到 —— 用 secure 读它。
    viol, resp = await axi_read(dut, VIOL)
    assert resp == RESP_OKAY, "secure 读 VIOL 应当正常"
    v_rd, v_wr = viol >> 16, viol & 0xFFFF
    assert v_rd == 4, f"4 笔被拒的读应当记 4，记了 {v_rd}"
    assert v_wr == 1, f"1 笔被拒的写应当记 1，记了 {v_wr}"
    dut._log.info(f"non-secure 的读写全部被拒（读回 0、写无副作用），"
                  f"违规计数 读{v_rd}/写{v_wr}")


@cocotb.test()
async def test_refused_read_does_not_pop_fifo(dut):
    """**被 DECERR 挡掉的读绝不能弹 FIFO**

    这是整个门控里最要紧的一条。拿不到随机数只是拒绝服务的一半；如果被拒的
    读仍然弹出，普通世界就拿到了一个"反复读把熵池抽干"的手段 —— 安全世界
    要随机数时拿不到，同样是致命的。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    limit = (STARTUP_SAMPLES + RATE_BITS) * DECIM // 200 + 20
    await wait_data_ready(dut, limit)

    before, _ = await axi_read(dut, WORDS)

    for _ in range(16):
        data, resp = await axi_read(dut, RDATA, prot=PROT_NONSEC)
        assert resp == RESP_REFUSED
        assert data == 0, f"被拒的读回了随机数 0x{data:08x}"

    after, _ = await axi_read(dut, WORDS)
    assert after == before, (
        f"16 次 non-secure 读把 WORDS 从 {before} 推到了 {after} "
        f"—— 被拒的读弹了 FIFO，普通世界可以借此抽干熵池")

    # secure 的读必须仍然正常 —— 否则一个恒为 DECERR 的实现也能通过上面所有条
    st, _ = await axi_read(dut, STATUS)
    assert st & ST_DATA_VALID, "16 次被拒的读之后数据没了"
    w, resp = await axi_read(dut, RDATA)
    assert resp == RESP_OKAY, "secure 的读也被拒了"
    after2, _ = await axi_read(dut, WORDS)
    assert after2 == before + 1, "secure 的读没有正确弹出"
    dut._log.info(f"16 次 non-secure 读没有消耗任何字（WORDS 保持 {before}），"
                  f"secure 读正常弹出 0x{w:08x}")


@cocotb.test()
async def test_tamper_zeroizes(dut):
    """tamper 硬件引脚：不经软件直接擦除全部中间态"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    limit = (STARTUP_SAMPLES + RATE_BITS) * DECIM // 200 + 20
    await wait_data_ready(dut, limit)
    st, _ = await axi_read(dut, STATUS)
    assert st & ST_STARTUP_DONE

    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0
    for _ in range(40):
        await RisingEdge(dut.clk)

    st, _ = await axi_read(dut, STATUS)
    assert not (st & ST_STARTUP_DONE), "tamper 之后启动检测没有重跑"
    assert not (st & ST_DATA_VALID), "tamper 之后 FIFO 里还有数据"
    assert not (st & ST_READY), "tamper 之后 ready 还是高的"

    blocks, _ = await axi_read(dut, BLOCKS)
    assert blocks == 0, "tamper 之后海绵状态没有清"
    words, _ = await axi_read(dut, WORDS)
    assert words == 0, "tamper 之后计数没有清"
    dut._log.info("tamper 引脚擦除了 FIFO、海绵状态与全部计数，启动检测重跑")
