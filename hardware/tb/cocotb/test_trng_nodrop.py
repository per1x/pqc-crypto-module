"""cocotb：调理器必须吃到与健康检测**同一条**样本流

SP 800-90B 的整套论证有一条隐含前提：**被评估的序列、被检测的序列、
被使用的序列是同一个东西**。第一版的 trng_top 不满足它 ——

    健康检测（RCT/APT）：吃每一个 src_valid
    RAW_TAP 抽头：      抽每一个 src_valid
    调理器：            只在 S_ABSORB 收样，
                        置换（约 26 拍）与挤出（8 次握手）期间进来的直接丢掉

于是拿 A 序列算出来的 H = 0.871234 被用来给 B 序列记熵账，而 A 与 B 差着
一批样本 —— 少了哪些、少的那些是不是随机地少，没有人回答得了。丢的比例小
（约 0.6%）不改变这一点：**这是口径问题，不是精度问题。**

这份用例的判据就是那句前提本身：

    调理器实际吸收的比特序列，必须与噪声源在同一段时间里交出的比特序列
    **逐比特相同**（允许末尾差几个还在 FIFO 里的）。

判据不是"drops 计数为 0" —— 那个计数是我自己写的，用它验自己等于没验。
两条流直接对比才是独立的判据；drops 只作为附加断言。

⚠️ 这条用例读的是 hierarchical 信号（噪声源的输出、调理器的入口握手），
   因为要比对的两条流在模块内部。这是有意的：它们本来就应该是同一条，
   把它们分别取出来对比，正是这条用例存在的理由。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

DECIM = 8
SAMPLE_FIFO_DEPTH = 32       # 与 trng_top 的默认参数一致


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


async def collect(dut, cycles):
    """跑 cycles 拍，同时录下两条流

    源侧：src_valid 且已过启动检测、未告警 —— 也就是"本该进熵池"的样本。
    调理侧：调理器入口上真正被吃掉的比特（bit_valid && bit_ready）。
    """
    src, absorbed = [], []
    cond = dut.u_cond
    for _ in range(cycles):
        await Timer(1, unit="ns")
        if (int(dut.u_src.sample_valid.value)
                and int(dut.startup_done.value)
                and not int(dut.alarm.value)):
            src.append(int(dut.u_src.sample.value))
        if int(cond.bit_valid.value) and int(cond.bit_ready.value):
            absorbed.append(int(cond.bit_in.value))
        await RisingEdge(dut.clk)
    return src, absorbed


@cocotb.test()
async def test_conditioner_absorbs_every_sample(dut):
    """两条流逐比特一致

    先跑到启动检测过、再录，避免把 §4.3 要求丢掉的那 1024 个样本算进来 ——
    那些是**该丢的**，与这里要抓的"忙的时候丢"是两回事。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for _ in range(200_000):
        await RisingEdge(dut.clk)
        if int(dut.startup_done.value):
            break
    else:
        raise AssertionError("启动健康检测一直没过")
    # 让海绵先进入稳定的"吸收—置换—挤出"循环，这样录的窗口里一定包含
    # 至少一次完整的置换与挤出 —— 丢样本恰恰只发生在那几十拍里。
    dut.rd_en.value = 1
    src, absorbed = await collect(dut, 30_000)
    dut.rd_en.value = 0

    assert len(src) > 3000, f"窗口里只取到 {len(src)} 个样本，太少，说明不了问题"

    # 调理器可能还有几个比特压在 FIFO 里没吃 —— 允许尾部差 FIFO 深度以内
    lag = len(src) - len(absorbed)
    assert 0 <= lag <= SAMPLE_FIFO_DEPTH, (
        f"源交出 {len(src)} 个样本，调理器只吸收了 {len(absorbed)} 个，"
        f"差 {lag} 个（FIFO 只有 {SAMPLE_FIFO_DEPTH} 深）—— "
        "调理器吃的与健康检测吃的不是同一条流")

    assert absorbed == src[:len(absorbed)], (
        "两条流的内容对不上：第一个不同在第 "
        f"{next(i for i, (a, b) in enumerate(zip(absorbed, src)) if a != b)} 位")

    assert int(dut.sample_drops.value) == 0, (
        f"sample_drops = {int(dut.sample_drops.value)}，取样 FIFO 溢出过")

    dut._log.info(
        f"{len(src)} 个样本，调理器逐比特吸收了 {len(absorbed)} 个，"
        f"尾部滞后 {lag}（≤FIFO 深度），溢出计数 0")
