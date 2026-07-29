"""cocotb：噪声源连续健康检测对拍（NIST SP 800-90B §4.4）

判据分三层：

一、**阈值本身**。RTL 的参数取值必须与按 SP 800-90B 定义现算出来的阈值相等。
    抄表抄错一格是这类模块最常见的问题，所以这里不比对"某个记住的数字"，
    而是比对现算结果。

二、**逐样本行为**。把同一条样本流同时喂给 RTL 与参考模型，每一步都比对告警位
    与两个计数器 —— 不只看最终结论。

三、**判定的边界与有效性**。卡死源必须**恰好**在第 C 个相同样本上告警（第 C−1
    个上不能告警），偏置源必须被 APT 抓到，理想均匀源不能误报；同时验证告警是
    锁存的、且只由 clear 清除。最后一条是反证：把阈值调大到不可能触发，
    同样的卡死流必须不再告警 —— 否则说明告警不是这两项检测判出来的。
"""
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

import tbutil  # noqa: F401  —— 导入即把 hardware/model 放进 sys.path

from trng_health_model import (  # noqa: E402
    HealthTests, apt_cutoff, apt_window, default_parameters, rct_cutoff,
)

RCT_C, APT_W, APT_C = default_parameters()


async def reset(dut):
    dut.rst_n.value = 0
    dut.clear.value = 0
    dut.sample_valid.value = 0
    dut.sample.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def feed(dut, bit):
    dut.sample.value = bit
    dut.sample_valid.value = 1
    await RisingEdge(dut.clk)
    dut.sample_valid.value = 0
    await Timer(1, unit="ns")


@cocotb.test()
async def test_parameters_match_definition(dut):
    """RTL 的阈值参数必须与按定义现算的结果一致"""
    assert RCT_C == rct_cutoff(0.5), "RCT 阈值与定义式不符"
    assert APT_W == apt_window(1), "APT 窗口与标准规定不符"
    assert APT_C == apt_cutoff(0.5, APT_W), "APT 阈值与定义式不符"
    assert int(dut.u_main.RCT_CUTOFF.value) == RCT_C, "RTL 的 RCT_CUTOFF 参数取值不对"
    assert int(dut.u_main.APT_WINDOW.value) == APT_W, "RTL 的 APT_WINDOW 参数取值不对"
    assert int(dut.u_main.APT_CUTOFF.value) == APT_C, "RTL 的 APT_CUTOFF 参数取值不对"
    dut._log.info(f"阈值与 SP 800-90B 定义式一致：RCT C={RCT_C}，"
                  f"APT W={APT_W} C={APT_C}")


@cocotb.test()
async def test_rct_boundary(dut):
    """卡死的噪声源：第 C−1 个相同样本上不能告警，第 C 个上必须告警"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for i in range(RCT_C - 1):
        await feed(dut, 1)
        assert int(dut.rct_alarm.value) == 0, f"第 {i + 1} 个相同样本就告警了"
        assert int(dut.rct_run.value) == i + 1, "重复计数不对"
    await feed(dut, 1)
    assert int(dut.rct_alarm.value) == 1, f"连续 {RCT_C} 个相同样本没有触发 RCT"
    dut._log.info(f"RCT 恰好在第 {RCT_C} 个相同样本上告警")


@cocotb.test()
async def test_rct_resets_on_change(dut):
    """样本一变，重复计数必须归一"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for _ in range(RCT_C - 1):
        await feed(dut, 0)
    assert int(dut.rct_alarm.value) == 0
    await feed(dut, 1)
    assert int(dut.rct_run.value) == 1, "样本变化后重复计数未归一"
    # 归一之后要再攒满 C 个才触发：这里先补到 C−1
    for _ in range(RCT_C - 2):
        await feed(dut, 1)
    assert int(dut.rct_run.value) == RCT_C - 1
    assert int(dut.rct_alarm.value) == 0, "计数归一后不应立刻再次触发"
    await feed(dut, 1)
    assert int(dut.rct_alarm.value) == 1
    dut._log.info("重复计数在样本变化处正确归一")


@cocotb.test()
async def test_apt_biased_source(dut):
    """严重偏置的源必须被 APT 在一个窗口内抓到，且 RCT 未必先告警"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(20260730)
    # 偏置到 90%，但每隔几个插一个相反值，避免先撞上 RCT
    for i in range(APT_W):
        bit = 1 if (i % 8 == 3) else (0 if rng.random() < 0.93 else 1)
        await feed(dut, bit)
    assert int(dut.apt_alarm.value) == 1, "偏置的源没有触发 APT"
    assert int(dut.rct_alarm.value) == 0, "这条流不该触发 RCT，测试设置有误"
    dut._log.info("APT 在一个窗口内抓到偏置源，且未与 RCT 混淆")


@cocotb.test()
async def test_uniform_source_no_false_alarm(dut):
    """理想均匀源上不应误报，且与参考模型逐样本一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(20260731)
    model = HealthTests(RCT_C, APT_W, APT_C)
    for i in range(4000):
        bit = rng.getrandbits(1)
        await feed(dut, bit)
        model.feed(bit)
        assert int(dut.rct_run.value) == model.rct_run, f"第 {i} 个样本：重复计数不一致"
        assert int(dut.apt_count.value) == model.apt_count, f"第 {i} 个样本：窗口计数不一致"
        assert int(dut.apt_index.value) == model.apt_index, f"第 {i} 个样本：窗口下标不一致"
        assert int(dut.rct_alarm.value) == int(model.rct_alarm)
        assert int(dut.apt_alarm.value) == int(model.apt_alarm)
    assert int(dut.alarm.value) == 0, "均匀随机源上出现了误报"
    dut._log.info("4000 个均匀随机样本：与参考模型逐样本一致，无误报")


@cocotb.test()
async def test_alarm_is_latched(dut):
    """告警锁存：置位后保持，只有 clear 能清

    软件在任意时刻读健康状态都必须能看到告警，与 STATUS.DONE 用电平同理。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for _ in range(RCT_C):
        await feed(dut, 1)
    assert int(dut.rct_alarm.value) == 1
    assert int(dut.alarm.value) == 1

    # 换成正常的样本流，告警必须保持
    rng = random.Random(1)
    for _ in range(200):
        await feed(dut, rng.getrandbits(1))
    assert int(dut.rct_alarm.value) == 1, "告警在正常样本上被自行清除了"

    dut.clear.value = 1
    await RisingEdge(dut.clk)
    dut.clear.value = 0
    await Timer(1, unit="ns")
    assert int(dut.rct_alarm.value) == 0, "clear 未能清除告警"
    assert int(dut.alarm.value) == 0
    dut._log.info("告警锁存，且只由 clear 清除")


@cocotb.test()
async def test_window_boundary(dut):
    """窗口边界：第 W+1 个样本开始新窗口，参考值与计数一起重置"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    model = HealthTests(RCT_C, APT_W, APT_C)
    rng = random.Random(9)
    for i in range(APT_W + 5):
        bit = rng.getrandbits(1)
        await feed(dut, bit)
        model.feed(bit)
        if i == APT_W - 1:
            assert int(dut.apt_index.value) == APT_W, "窗口内下标未走到 W"
        if i == APT_W:
            assert int(dut.apt_index.value) == 1, "第 W+1 个样本应当开启新窗口"
            assert int(dut.apt_count.value) == 1, "新窗口的计数应当从 1 开始"
        assert int(dut.apt_index.value) == model.apt_index
        assert int(dut.apt_count.value) == model.apt_count
    dut._log.info("窗口边界处参考值与计数正确重置")


@cocotb.test()
async def test_negative_control(dut):
    """反证：阈值调高到不可能触发时，同一条卡死流必须不再告警

    没有这一条，一个恒为 1 的告警位也能让上面所有正向断言通过。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for _ in range(RCT_C + 50):
        await feed(dut, 1)
    assert int(dut.rct_alarm.value) == 1, "正常阈值下卡死流应当告警"
    assert int(dut.ctl_rct_alarm.value) == 0, \
        "阈值调高到不可能触发时仍然告警 —— 告警不是这项检测判出来的"
    assert int(dut.ctl_apt_alarm.value) == 0, "空对照的 APT 也不该告警"
    dut._log.info("空对照成立：告警确实由阈值判定产生")
