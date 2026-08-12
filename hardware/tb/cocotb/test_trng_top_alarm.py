"""cocotb：TRNG 告警之后的处置（SP 800-90B §4.4）

标准只要求"检测到失效就停止输出并上报"，具体动作留给使用方。本设计选了最
保守的一种，这个文件就是在钉住那几个动作确实发生了：

  · ready 立刻拉低，此后一个字都不许出来；
  · FIFO 被清并逐地址覆零；
  · **调理器连同海绵状态一起复位** —— 告警意味着噪声源可能已经失效一段
    时间，池子里可能混进了低熵输入，留着比丢掉风险大；
  · 启动健康检测重新来过，不能"清个告警就接着用"。

【怎么让告警确定性地发生】
环振跑的是行为模型，等它自然出现 41 个连续相同样本是不现实的，把阈值调到
"大概会触发"又会让测试变成概率性的、偶发红。所以这里用 `-P` 把 RCT 阈值
压到 2：只要出现两个连续相同的样本就告警，几个样本之内必定发生。
测的是**告警之后的处置逻辑**，阈值本身正不正确由 test_trng_health.py 负责。

"FIFO 里有数据时被擦除"这条路径由 test_trng_top.py 的 zeroize 用例覆盖 ——
两者走的是同一个 fifo_flush 信号、同一个 sync_fifo 擦除状态机。

运行：
  make -f Makefile.trng MODULE=test_trng_top_alarm TOPLEVEL=trng_top \\
       PARAMS=-Ptrng_top.RCT_CUTOFF=2
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

DECIM = 8


async def reset(dut):
    dut.rst_n.value = 0
    dut.enable.value = 0
    dut.zeroize.value = 0
    dut.clear_alarm.value = 0
    dut.rd_en.value = 0
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    dut.enable.value = 1
    await RisingEdge(dut.clk)


async def wait_alarm(dut, limit=200):
    """跑到告警出现，顺便盯着这段时间里有没有数据漏出去"""
    dut.rd_en.value = 1
    leaked = 0
    for cyc in range(limit):
        await Timer(1, unit="ns")
        leaked += int(dut.rd_valid.value)
        if int(dut.alarm.value):
            dut.rd_en.value = 0
            return cyc, leaked
        await RisingEdge(dut.clk)
    dut.rd_en.value = 0
    raise AssertionError(f"{limit} 个时钟内没有触发告警（RCT 阈值是不是没被覆盖成 2？）")


@cocotb.test()
async def test_parameter_override_took_effect(dut):
    """先确认 -P 真的把阈值改了 —— 否则后面几条全是假通过"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    got = int(dut.RCT_CUTOFF.value)
    assert got == 2, (
        f"RCT_CUTOFF={got}，参数覆盖没生效。"
        f"这个文件必须带 PARAMS=-Ptrng_top.RCT_CUTOFF=2 运行")
    dut._log.info("参数覆盖生效：RCT_CUTOFF=2")


@cocotb.test()
async def test_alarm_stops_output_and_wipes(dut):
    """告警 → ready 拉低、FIFO 擦除、海绵复位、启动检测归零"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    cyc, leaked = await wait_alarm(dut)
    assert leaked == 0, f"告警前就漏出了 {leaked} 拍数据（启动检测都还没过）"
    assert int(dut.rct_alarm.value) == 1, "告警了但 rct_alarm 没置位"

    # 告警的那一拍 ready 就必须已经是低的
    await Timer(1, unit="ns")
    assert int(dut.ready.value) == 0, "告警了 ready 还是高的"

    # FIFO 擦除扫描要被启动（fifo_flush 用的是 alarm 的上升沿）
    seen_wiping = False
    for _ in range(64):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        seen_wiping |= bool(int(dut.fifo_wiping.value))
    assert seen_wiping, "告警没有触发 FIFO 擦除扫描"

    assert int(dut.startup_done.value) == 0, "告警后启动检测标志没有清"
    assert int(dut.startup_count.value) == 0, "告警后启动计数没有归零"
    assert int(dut.blocks_absorbed.value) == 0, "告警后海绵状态没有复位"
    dut._log.info(f"第 {cyc} 拍告警：ready 拉低、FIFO 擦除、海绵复位、启动检测归零")


@cocotb.test()
async def test_alarm_blocks_output_indefinitely(dut):
    """不清告警就一直不出数：跑够一个 rate 块的时长也不能有字出来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    await wait_alarm(dut)

    dut.rd_en.value = 1
    leaked = 0
    for _ in range(4000):
        await Timer(1, unit="ns")
        leaked += int(dut.rd_valid.value)
        await RisingEdge(dut.clk)
    dut.rd_en.value = 0

    assert leaked == 0, f"告警未清期间漏出了 {leaked} 拍数据"
    assert int(dut.alarm.value) == 1, "告警自己消失了 —— 告警必须是锁存的"
    assert int(dut.words_out.value) == 0, "告警期间 words_out 变了"
    dut._log.info("告警未清期间 4000 拍无任何输出，且告警保持锁存")


@cocotb.test()
async def test_clear_alarm_restarts_startup_test(dut):
    """clear_alarm 清掉告警，但必须重跑启动检测，不能直接接着用"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    await wait_alarm(dut)

    dut.clear_alarm.value = 1
    await RisingEdge(dut.clk)
    dut.clear_alarm.value = 0
    await Timer(1, unit="ns")

    assert int(dut.alarm.value) == 0, "clear_alarm 没有清掉告警"
    assert int(dut.startup_done.value) == 0, \
        "清告警之后 startup_done 直接是高的 —— 启动检测被跳过了"
    assert int(dut.startup_count.value) == 0, "清告警之后启动计数没有从头开始"
    dut._log.info("clear_alarm 清掉告警，启动检测从零重跑")
