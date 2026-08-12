"""cocotb：多环振噪声源（trng_source）

⚠️ **这个文件测的是数字化逻辑，不是熵。**

环振在 RTL 仿真里跑的是 ring_osc.v 里的行为模型，抖动量是编的（比真实器件大
一到两个数量级）。所以本文件里任何关于"看起来很随机"的断言，**都只是在防
低级错误**（比如采样器接反了、异或写成了或、抽取计数器差一），
**不构成任何最小熵结论**。真实的最小熵必须在硅上用 NIST EntropyAssessment
跑导出的原始比特实测 —— 见 ring_osc.v 与 docs/fpga-进展.md。

能在仿真里真正测准的是这些：
  · 抽取比：sample_valid 必须恰好每 DECIM 个时钟出现一次；
  · enable 门控：拉低后不再产样本（而且不是产出一串常量 —— 常量会喂爆 RCT）；
  · 复位行为；
  · 数据通路没接反：8 条环的异或不会退化成常量或周期极短的序列。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

DECIM = 8       # 与 trng_source 的默认参数一致
NUM_RO = 8


async def reset(dut):
    dut.rst_n.value = 0
    dut.enable.value = 0
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    dut.enable.value = 1
    await RisingEdge(dut.clk)


async def collect(dut, n):
    """收 n 个样本，同时记录每个样本之间隔了多少个时钟"""
    bits, gaps = [], []
    since = 0
    guard = 0
    while len(bits) < n:
        await Timer(1, unit="ns")
        v = int(dut.sample_valid.value)
        b = int(dut.sample.value)
        await RisingEdge(dut.clk)
        since += 1
        if v:
            bits.append(b)
            gaps.append(since)
            since = 0
        guard += 1
        assert guard < n * DECIM * 4, "样本产出得太慢，抽取计数器可能不对"
    return bits, gaps


@cocotb.test()
async def test_decimation_rate(dut):
    """sample_valid 必须严格每 DECIM 个时钟出现一次"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert int(dut.DECIM.value) == DECIM, "测试里的 DECIM 与 RTL 参数不一致"
    _, gaps = await collect(dut, 64)

    # 第一个间隔受复位对齐影响，从第二个起必须严格等于 DECIM
    bad = [g for g in gaps[1:] if g != DECIM]
    assert not bad, f"样本间隔不是恒定的 {DECIM}：出现了 {sorted(set(bad))}"
    dut._log.info(f"抽取比正确：64 个样本的间隔恒为 {DECIM} 个时钟")


@cocotb.test()
async def test_enable_gates_sampling(dut):
    """enable 拉低后不再产样本

    重点在"不产"而不是"产常量"：环振停振时采到的是恒定电平，若还继续往外
    发样本，下游 RCT 会在 41 个样本内报一个与噪声源无关的假告警，把真正的
    故障掩盖掉。trng_source 因此在 enable 低时直接不置 sample_valid。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    await collect(dut, 8)

    dut.enable.value = 0
    await RisingEdge(dut.clk)
    seen = 0
    for _ in range(DECIM * 20):
        await Timer(1, unit="ns")
        seen += int(dut.sample_valid.value)
        await RisingEdge(dut.clk)
    assert seen == 0, f"enable 拉低后仍产出了 {seen} 个样本"

    dut.enable.value = 1
    bits, _ = await collect(dut, 16)
    assert len(bits) == 16, "重新使能后没有恢复产样本"
    dut._log.info("enable 门控正确：拉低即停产样本，拉高后恢复")


@cocotb.test()
async def test_stream_is_not_degenerate(dut):
    """采样通路没接坏：输出既不是常量，也不是周期极短的序列

    这不是熵评估（见文件头）。它抓的是接线级别的错误 —— 异或写成了与、
    只有一条环真的在动、同步器把两级接成了一级之类。这类错误会让输出
    退化成常量或短周期序列，在这里必然暴露。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = 2048
    bits, _ = await collect(dut, n)

    ones = sum(bits)
    frac = ones / n
    assert 0 < ones < n, "采样输出是常量 —— 采样通路坏了"
    assert 0.30 < frac < 0.70, f"偏置大到不像话：1 的比例 {frac:.3f}"

    # 最长游程。RCT 阈值是 41，仿真里出现接近它的游程就说明源有问题。
    run = best = 1
    for i in range(1, n):
        run = run + 1 if bits[i] == bits[i - 1] else 1
        best = max(best, run)
    assert best < 20, f"最长游程 {best}，接近 RCT 阈值 41，采样通路可疑"

    # 相邻比特的转移计数：四种组合都该出现，且不严重失衡。
    # 只有一条环在动、或抽取比错成 1 时，这里会明显偏。
    trans = {(0, 0): 0, (0, 1): 0, (1, 0): 0, (1, 1): 0}
    for i in range(1, n):
        trans[(bits[i - 1], bits[i])] += 1
    assert all(v > n * 0.10 for v in trans.values()), \
        f"相邻比特转移分布失衡：{trans}"

    dut._log.info(f"{n} 个样本：1 的比例 {frac:.3f}，最长游程 {best}，"
                  f"转移分布 {trans}（仅为通路自检，非熵评估）")
