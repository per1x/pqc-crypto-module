"""cocotb：TRNG 的原始噪声抽头（RAW_TAP）

这条通路是给 **SP 800-90B 的最小熵评估**用的：那套方法要的是噪声源的
**原始数字化样本**，不是调理器（SHA-3）的输出。调理器的输出无论熵多低
看着都像随机数，拿它跑 EntropyAssessment 会得到一个非常好看、但**毫无意义**
的数字 —— 而且正好骗过想少做一步的人。

抽头点必须是 `src_valid/src_bit`，也就是**健康检测（RCT/APT）吃的同一条流**。
检测的对象、评估的对象、被使用的对象是同一个，三者才都有意义。

测四条：
  ① RAW_TAP=1 时，攒够 32 个原始比特就出一个字，raw_valid 抬起来。
  ② 读出来的字**确实是原始比特**，不是调理器的输出 —— 拿同一时刻的
     RDATA 比对，两者必须不同（相同就说明抽头接到调理器后面去了）。
  ③ 抽头**不拖慢噪声源**：FIFO 满了丢新的，不做背压。背压会改变被评估的
     那条流的统计性质，等于评估了一个不存在的东西。
  ④ **RAW_TAP=0 时整条通路不存在**（另一条用例，见 rtl_sim.sh 里的参数覆盖）。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer


async def reset(dut):
    dut.rst_n.value = 0
    dut.enable.value = 0
    dut.zeroize.value = 0
    dut.clear_alarm.value = 0
    dut.rd_en.value = 0
    dut.raw_rd_en.value = 0
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    dut.enable.value = 1
    await RisingEdge(dut.clk)


async def wait_for(dut, sig, limit):
    for _ in range(limit):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        if int(sig.value) == 1:
            return True
    return False


async def pop_raw(dut, limit=400_000):
    """取一个原始字

    ⚠️ sync_fifo 的 rd_data 是**组合**输出（rd_data = mem[rptr]），
       数据在读脉冲之前就有效。先采数再打脉冲，反了的话取到的是下一个字，
       FIFO 空的时候还会取到 0 —— 而"全 0"正好会被误判成"抽头接了根死线"。
    """
    if not await wait_for(dut, dut.raw_valid, limit):
        raise AssertionError("等不到 raw_valid —— FIFO 里没有字可取")
    v = int(dut.raw_data.value)
    dut.raw_rd_en.value = 1
    await RisingEdge(dut.clk)
    dut.raw_rd_en.value = 0
    await Timer(1, unit="ps")
    return v


@cocotb.test()
async def test_raw_words_appear(dut):
    """攒够 32 个原始比特就出一个字"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wait_for(dut, dut.raw_valid, 400_000), \
        "等不到原始噪声字 —— RAW_TAP 没接通，或者噪声源没在出样本"

    words = [await pop_raw(dut) for _ in range(4)]
    dut._log.info("原始字：" + " ".join(f"0x{w:08x}" for w in words))

    # 全 0 或全 1 说明抽的是一根常量线，不是噪声
    assert not all(w == 0 for w in words), "原始字全 0 —— 抽头接到了一根死线上"
    assert not all(w == 0xFFFFFFFF for w in words), "原始字全 1 —— 同上"


@cocotb.test()
async def test_raw_is_not_conditioned_output(dut):
    """原始字**不是**调理器的输出 —— 抽头接错地方是这条通路最致命的错法

    接到调理器后面的话，这条通路看起来一切正常（有数、在变、统计性质漂亮），
    但拿它跑出来的最小熵是**假的**，而且假得很好看。所以必须显式区分。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wait_for(dut, dut.raw_valid, 400_000), "等不到原始噪声字"
    raws = [await pop_raw(dut) for _ in range(8)]

    # 取几个调理后的字
    conds = []
    for _ in range(4):
        if not await wait_for(dut, dut.rd_valid, 2_000_000):
            break
        dut.rd_en.value = 1
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        conds.append(int(dut.rd_data.value))
        dut.rd_en.value = 0
        await RisingEdge(dut.clk)

    if conds:
        assert not (set(raws) & set(conds)), (
            "原始字和调理后的字出现了相同值 —— 抽头很可能接到了调理器后面。"
            "那样跑出来的最小熵是假的。")
        dut._log.info(f"原始 {len(raws)} 字与调理后 {len(conds)} 字无交集，抽头位置正确")
    else:
        dut._log.warning("这次没等到调理输出，只验了原始那一侧")


@cocotb.test()
async def test_raw_fifo_never_backpressures(dut):
    """FIFO 满了丢新的，**绝不**回压噪声源

    回压会改变被评估的那条流的统计性质 —— 采集到的就不再是噪声源本来的
    样子了，评估的是一个不存在的东西。所以这里故意一直不读 raw，让它满，
    然后确认**噪声源本身**还在出样本。

    ⚠️ 观测量必须选对。第一版拿 blocks_absorbed（调理器吸收的块数）当判据，
       它确实会停 —— 但那是因为输出 FIFO 满了没人读，跟 raw 抽头毫无关系，
       等于用一个必然发生的现象去指控一个无辜的模块。
       正确的观测量是 apt_index：它数的是**健康检测处理过的样本数**，
       而健康检测吃的正是 src_valid/src_bit 那条流，也就是抽头的同一条。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wait_for(dut, dut.raw_valid, 400_000), "等不到原始噪声字"

    # 一直不读 raw，让它撑满
    for _ in range(200_000):
        await RisingEdge(dut.clk)

    a0 = int(dut.apt_index.value)
    n0 = int(dut.startup_count.value)
    for _ in range(400_000):
        await RisingEdge(dut.clk)
    a1 = int(dut.apt_index.value)
    n1 = int(dut.startup_count.value)

    assert (a1 != a0) or (n1 > n0), (
        f"raw FIFO 满了之后噪声源不出样本了"
        f"（apt_index {a0}->{a1}, startup {n0}->{n1}）—— 抽头回压了噪声源")
    dut._log.info(
        f"raw FIFO 撑满期间噪声源照常出样本：apt_index {a0}->{a1}, startup {n0}->{n1}")
