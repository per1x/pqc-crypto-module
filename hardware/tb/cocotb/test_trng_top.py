"""cocotb：TRNG 顶层（trng_top）—— 策略层

单元测试已经分别管住了噪声源、健康检测、调理器。这个文件测的是**只有在顶层
才存在的东西**，也就是 SP 800-90B 里那些"必须怎么做"的策略：

  · **启动健康检测（§4.3）**：上电后没跑够 STARTUP_SAMPLES 个干净样本之前，
    一个随机字都不许出来。这一条是最容易被实现成"先出数据再补检测"的，
    所以测试直接盯着"启动完成前 rd_valid 恒为 0"。
  · **熵账**：第一个字必须在吸收满一个 rate 块（1088 比特）之后才出现，
    不能提前。提前就意味着调理比例没达到设计假设。
  · **zeroize**：FIFO 真的被逐地址覆零（不是只挪指针）、海绵状态被清、
    启动检测重跑。这是密码边界 zeroize-on-tamper 的挂钩点。

告警之后的处置（清池、清 FIFO、重跑启动检测）在 test_trng_top_alarm.py 里测，
那个文件要把 RCT 阈值调到很小才能在合理仿真时长里触发，所以单独一个构建。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

DECIM = 8
RATE_BITS = 17 * 64          # 一个 rate 块 = 1088 比特
STARTUP_SAMPLES = 1024       # trng_top 的默认参数


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


async def tick(dut):
    """推进一拍，返回本拍读出的字（rd_en 已置位且 rd_valid 时）或 None"""
    await Timer(1, unit="ns")
    got = int(dut.rd_data.value) if (int(dut.rd_valid.value)
                                     and int(dut.rd_en.value)) else None
    await RisingEdge(dut.clk)
    return got


async def run_until_words(dut, n, limit):
    """一直跑到收够 n 个随机字，返回 (字列表, 用掉的时钟数)"""
    words = []
    dut.rd_en.value = 1
    for cyc in range(limit):
        w = await tick(dut)
        if w is not None:
            words.append(w)
            if len(words) == n:
                dut.rd_en.value = 0
                return words, cyc + 1
    dut.rd_en.value = 0
    raise AssertionError(f"{limit} 个时钟内只收到 {len(words)}/{n} 个字")


@cocotb.test()
async def test_no_output_before_startup_test(dut):
    """启动健康检测跑完之前，一个随机字都不许出来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert int(dut.STARTUP_SAMPLES.value) == STARTUP_SAMPLES, \
        "测试里的 STARTUP_SAMPLES 与 RTL 参数不一致"

    dut.rd_en.value = 1
    leaked = 0
    # 启动检测要吃 STARTUP_SAMPLES 个样本、每个间隔 DECIM 个时钟，
    # 外加复位释放到第一个样本之间的对齐余量。
    budget = STARTUP_SAMPLES * DECIM + 4 * DECIM
    for _ in range(budget):
        await Timer(1, unit="ns")
        if int(dut.rd_valid.value):
            leaked += 1
        if int(dut.startup_done.value):
            break
        await RisingEdge(dut.clk)
    dut.rd_en.value = 0

    assert leaked == 0, f"启动检测完成前 rd_valid 拉高了 {leaked} 次 —— 数据泄出去了"
    assert int(dut.startup_done.value) == 1, "启动检测没有在预期的样本数内完成"
    assert int(dut.startup_count.value) == STARTUP_SAMPLES - 1, \
        f"启动计数 {int(dut.startup_count.value)}，应为 {STARTUP_SAMPLES - 1}"
    assert int(dut.blocks_absorbed.value) == 0, \
        "启动期间调理器就已经在吸收了 —— 那些样本本该全部丢弃"
    dut._log.info(f"启动检测吃满 {STARTUP_SAMPLES} 个样本才放行，期间无输出")


@cocotb.test()
async def test_first_word_after_one_rate_block(dut):
    """第一个字必须在吸收满 1088 比特之后才出现，不能提前"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 启动检测 + 一个 rate 块，都按 DECIM 个时钟一个样本算
    ideal = (STARTUP_SAMPLES + RATE_BITS) * DECIM
    words, cycles = await run_until_words(dut, 1, ideal * 3)

    assert cycles >= ideal, (
        f"第一个字在第 {cycles} 拍就出来了，理论最早 {ideal} 拍 —— "
        f"调理比例不足，熵账不成立")
    # 上界放宽到 1.1 倍：置换/挤出期间丢掉的样本会拖慢一点，但不该多太多
    assert cycles < ideal * 1.1, \
        f"第一个字用了 {cycles} 拍（理论 {ideal}），丢样本比预期多得多"

    assert int(dut.ready.value) == 1, "出了字但 ready 没拉高"
    assert int(dut.alarm.value) == 0, "正常运行却有告警"
    dut._log.info(f"第一个字在第 {cycles} 拍出现（理论下界 {ideal}），"
                  f"首字 0x{words[0]:08x}")


@cocotb.test()
async def test_words_are_not_degenerate(dut):
    """连续取多个字：互不相同、比特分布不偏

    与 test_trng_source 里那条一样，这是通路自检不是熵评估 —— 环振跑的是
    行为模型。它抓的是"FIFO 把同一个字反复吐出来""调理器没在推进"这类错误。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = 16   # 两轮挤出
    ideal = (STARTUP_SAMPLES + RATE_BITS * 2) * DECIM
    words, _ = await run_until_words(dut, n, ideal * 3)
    # run_until_words 返回时最后一拍的非阻塞赋值还没落地，读 words_out 会少 1。
    # 等一小段时间让它稳定下来再读。
    await Timer(1, unit="ns")

    assert len(set(words)) == n, f"{n} 个字里有重复：{[hex(w) for w in words]}"
    ones = sum(bin(w).count("1") for w in words)
    frac = ones / (n * 32)
    assert 0.35 < frac < 0.65, f"输出比特偏置异常：{frac:.3f}"
    assert int(dut.words_out.value) == n, \
        f"words_out={int(dut.words_out.value)}，应为 {n}"
    dut._log.info(f"{n} 个字互不相同，1 的比例 {frac:.3f}")


@cocotb.test()
async def test_zeroize(dut):
    """zeroize：FIFO 逐地址覆零、海绵清空、启动检测重跑"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    ideal = (STARTUP_SAMPLES + RATE_BITS) * DECIM
    await run_until_words(dut, 1, ideal * 3)
    assert int(dut.blocks_absorbed.value) >= 1
    assert int(dut.startup_done.value) == 1

    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0

    # 擦除扫描要走 FIFO_DEPTH 拍，期间 ready 必须为低
    await Timer(1, unit="ns")
    assert int(dut.fifo_wiping.value) == 1, "zeroize 没有启动 FIFO 擦除扫描"
    assert int(dut.ready.value) == 0, "擦除期间 ready 仍为高"
    await RisingEdge(dut.clk)

    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.fifo_wiping.value) == 0:
            break
        await RisingEdge(dut.clk)
    else:
        raise AssertionError("FIFO 擦除扫描没有结束")

    assert int(dut.startup_done.value) == 0, "zeroize 之后启动检测没有重跑"
    assert int(dut.rd_valid.value) == 0, "zeroize 之后 FIFO 里还有可读数据"
    assert int(dut.words_out.value) == 0, "zeroize 没有清计数"
    assert int(dut.blocks_absorbed.value) == 0, "zeroize 没有清空海绵状态"

    # 清完之后必须能重新暖机产出
    words, _ = await run_until_words(dut, 1, ideal * 3)
    dut._log.info(f"zeroize 后 FIFO 已擦除、海绵已清、启动检测重跑，"
                  f"重新产出首字 0x{words[0]:08x}")
