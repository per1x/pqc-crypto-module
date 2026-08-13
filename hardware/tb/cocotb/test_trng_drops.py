"""cocotb：取样 FIFO 的溢出计数是活的 —— 反证用

test_trng_nodrop 断言默认构建下 `sample_drops == 0`。这条断言有个显而易见的
弱点：**一个永远读到 0 的寄存器，和一个"确实没溢出过"的寄存器，长得一模一样。**
如果那段计数逻辑压根没接上（写错了信号、条件恒假、忘了接到 AXI），
上面那条断言照样绿。

所以专门用一个把 SAMPLE_FIFO_DEPTH 压到 2 的构建跑这一条：强迫它溢出，
证明计数确实会动。跑法见 tools/rtl_sim.sh：

    make -f Makefile.trng MODULE=test_trng_drops TOPLEVEL=trng_top \\
         PARAMS=-Ptrng_top.SAMPLE_FIFO_DEPTH=2

（深度不能压到 1：sync_fifo 的指针宽度是 $clog2(DEPTH)，1 深会退化成 0 位。）
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge


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


@cocotb.test()
async def test_drops_counter_is_live(dut):
    """FIFO 压到 2 深，溢出计数必须真的动起来"""
    depth = int(dut.SAMPLE_FIFO_DEPTH.value)
    assert depth <= 2, (
        f"这条用例要求用 -Ptrng_top.SAMPLE_FIFO_DEPTH=2 的构建跑，"
        f"当前是 {depth} —— 深度够大时不会溢出，这条断言就没有意义了")

    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for _ in range(200_000):
        await RisingEdge(dut.clk)
        if int(dut.startup_done.value):
            break
    else:
        raise AssertionError("启动健康检测一直没过")

    dut.rd_en.value = 1
    for _ in range(30_000):
        await RisingEdge(dut.clk)
    dut.rd_en.value = 0

    drops = int(dut.sample_drops.value)
    assert drops > 0, (
        f"{depth} 深的 FIFO 都没溢出过 —— 那条计数逻辑没有接上，"
        "于是默认深度下的 drops==0 什么都证明不了")

    # 清零那条路也要是活的
    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0
    await RisingEdge(dut.clk)
    assert int(dut.sample_drops.value) == 0, "zeroize 没有把 drops 清零"

    dut._log.info(f"{depth} 深 FIFO 下 drops = {drops}，计数与清零都是活的")
