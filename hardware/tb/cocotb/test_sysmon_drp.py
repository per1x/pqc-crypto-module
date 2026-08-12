"""cocotb：SYSMON 的 DRP 轮询状态机

这一半特意从 fan_sysmon.v 里拆出来，就是为了能对拍 —— 厂商原语只能在
Vivado 里仿，而"什么时候拉 DEN、DRDY 不来怎么办"恰恰是最容易写错的地方。

测四条：
  ① 周期到了才发起读，DEN 只拉一拍（拉两拍 DRP 会当成两笔事务）。
  ② DRDY 回来 → value 更新 + value_valid 出一个**单拍**脉冲。
  ③ DRDY 一直不来 → 超时后回到空闲、置 timed_out，且**不拉 value_valid**
     （上层看不到 valid 才会强制风扇满速，这条链路不能被"假 valid"打断）。
  ④ **调试读的结果绝不能拉 value_valid。** 这一条最要紧：调试窗口读的是
     配置寄存器（0x40/0x41…），那些值当成温度就是几百度或者零下几百度。
     一个人在 shell 里读一下寄存器，风扇就按假温度动作 —— 必须挡死。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

PERIOD = 200          # Makefile 里把 PERIOD 压到 200 拍，见那里
TIMEOUT = 64


async def reset(dut):
    dut.rst_n.value = 0
    dut.dout.value = 0
    dut.drdy.value = 0
    dut.dbg_req.value = 0
    dut.dbg_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def wait_den(dut, limit=4 * PERIOD):
    """等 DEN 拉起来，返回当时的 daddr"""
    for _ in range(limit):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        if int(dut.den.value) == 1:
            return int(dut.daddr.value)
    raise AssertionError("等不到 DEN —— 状态机没有发起 DRP 读")


async def answer(dut, data):
    """扮演 SYSMON：DRDY 拉一拍，同时给出数据"""
    dut.dout.value = data
    dut.drdy.value = 1
    await RisingEdge(dut.clk)
    dut.drdy.value = 0
    await Timer(1, unit="ns")


@cocotb.test()
async def test_periodic_read(dut):
    """周期读：DEN 只拉一拍，地址是温度寄存器 0x00，DRDY 后出单拍 valid"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    addr = await wait_den(dut)
    assert addr == 0x00, f"周期读的地址应当是 0x00，实际 0x{addr:02x}"

    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    assert int(dut.den.value) == 0, "DEN 拉了不止一拍 —— DRP 会当成两笔事务"

    await answer(dut, 0xABCD)
    assert int(dut.value.value) == 0xABCD
    assert int(dut.value_valid.value) == 1, "DRDY 回来了却没有 value_valid"

    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    assert int(dut.value_valid.value) == 0, "value_valid 不是单拍脉冲"

    dut._log.info("周期读：地址 0x00、DEN 单拍、DRDY 后单拍 valid")


@cocotb.test()
async def test_timeout_does_not_fake_valid(dut):
    """DRDY 不来：超时后置 timed_out，**绝不**拉 value_valid"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await wait_den(dut)
    seen_valid = 0
    for _ in range(TIMEOUT + 8):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        seen_valid |= int(dut.value_valid.value)

    assert seen_valid == 0, \
        "DRDY 从没来过却拉了 value_valid —— 上层会拿到一个凭空的温度"
    assert int(dut.timed_out.value) == 1, "超时了却没有置 timed_out"

    # 不重试到死：下一个周期还会再试
    await wait_den(dut)
    dut._log.info("超时：置 timed_out、不拉 valid、下个周期继续重试")


@cocotb.test()
async def test_debug_read_does_not_touch_temperature(dut):
    """**调试读绝不能污染温度**——读一下配置寄存器不该让风扇按假温度动作"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    # 先做一次正常的周期读，拿到一个已知温度
    await wait_den(dut)
    await answer(dut, 0x9C40)
    good = int(dut.value.value)
    assert good == 0x9C40

    # 请求读 0x41（配置寄存器）。它的值当成温度是荒谬的。
    dut.dbg_addr.value = 0x41
    dut.dbg_req.value = 1
    await RisingEdge(dut.clk)
    dut.dbg_req.value = 0

    addr = await wait_den(dut)
    # 可能先轮到一次周期读，那就先应付掉再等调试读
    if addr != 0x41:
        await answer(dut, 0x9C40)
        addr = await wait_den(dut)
    assert addr == 0x41, f"调试读的地址不对：0x{addr:02x}"

    seen_valid = 0
    await answer(dut, 0x00F0)
    seen_valid |= int(dut.value_valid.value)
    for _ in range(4):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        seen_valid |= int(dut.value_valid.value)

    assert seen_valid == 0, \
        "调试读也拉了 value_valid —— 读一下配置寄存器风扇就按假温度动作了"
    assert int(dut.value.value) == good, "调试读把温度寄存器覆盖掉了"
    assert int(dut.dbg_data.value) == 0x00F0, "调试读的数据没进 dbg_data"
    assert int(dut.dbg_valid.value) == 1

    dut._log.info("调试读：数据只进 dbg_data，温度那条路一点没动")
