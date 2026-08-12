"""cocotb：风扇控制器（温度→占空比 + 迟滞 + 安全上限）

风扇这东西错了不会崩，只会**安静地让芯片变热**——所以每条安全性质都单独测：

  ① 温度→档位的映射对，且**有迟滞**（在阈值附近来回抖不换档）。
  ② 最低档**不是停转**。把风扇停死不是省电是赌运气。
  ③ 过温强制满速，而且**进得早出得晚**（80°C 进、74°C 才出）。
  ④ **温度读不到就强制满速**——温度未知时唯一安全的假设是"可能很热"。
     这一条最容易被漏掉，因为正常情况下永远走不到。
  ⑤ 手动覆盖**盖不过**过温强制。调试口不能让人把芯片烤了。
  ⑥ AA11 是低有效：占空比越高，引脚为低的时间越长。写反了的表现是
     "温度越高越安静"，而且在实验室里未必立刻看得出来。
  ⑦ **读数长时间一个比特不变也要强制满速。**这一条是上板之后补的：
     SYSMON 配错的时候 ④ 完全挡不住 —— DRP 照样应答、寄存器里照样有个
     看着合理的 32.5°C，只是 ADC 没在转换。"读不到"永远不成立，
     风扇就心安理得停在最低档。所以"完全不变"本身要当故障看。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

# 与 fan_ctrl.v 里的 localparam 一致（括号是对应摄氏度）
T1_UP, T1_DN = 41547, 40895      # 45 / 40
T2_UP, T2_DN = 42850, 42198      # 55 / 50
T3_UP, T3_DN = 44153, 43501      # 65 / 60
T4_UP, T4_DN = 45456, 44804      # 75 / 70
OT_ON, OT_OFF = 46108, 45326     # 80 / 74

DUTY = [25, 40, 60, 80, 100]
PWM_PERIOD = 3000


def c2code(t):
    """摄氏度 → ADC 码，与 RTL 里那组常数同一个公式"""
    return int(round((t + 273.8195117) * 65536.0 / 502.9098127))


async def reset(dut):
    dut.rst_n.value = 0
    dut.temp_code.value = 0
    dut.temp_valid.value = 0
    dut.ovr_en.value = 0
    dut.ovr_duty.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def feed(dut, code, n=3):
    """喂 n 次温度采样（档位一次只走一档，所以要喂够次数）"""
    for _ in range(n):
        dut.temp_code.value = code
        dut.temp_valid.value = 1
        await RisingEdge(dut.clk)
        dut.temp_valid.value = 0
        await RisingEdge(dut.clk)
    await Timer(1, unit="ns")


async def measure_duty(dut, keep_temp=None):
    """量一个完整 PWM 周期里引脚为低的比例（AA11 低=转）

    ⚠️ 量的过程中**必须继续喂温度，而且要带 ±1 LSB 的抖动**。
       两条理由，都对应一条真实的安全性质：
         · 不喂 → STALE 判定触发（测试里压到 200 拍），量出来全是 100%；
         · 喂了但纹丝不动 → **卡死判定**触发（压到 20 次），同样是 100%。
       真硬件上 SYSMON 每 1 ms 出一个数，而且读数一直在抖 —— 测试台照着
       这个样子喂才算模拟到位。±1 LSB 只有 0.008°C，不会改变档位。
    """
    low = 0
    for i in range(PWM_PERIOD):
        if keep_temp is not None and (i % 64) == 0:
            dut.temp_code.value = keep_temp + ((i // 64) & 1)   # ±1 LSB 抖动
            dut.temp_valid.value = 1
        await RisingEdge(dut.clk)
        dut.temp_valid.value = 0
        await Timer(1, unit="ns")
        if int(dut.fan_pin.value) == 0:
            low += 1
    return 100.0 * low / PWM_PERIOD


@cocotb.test()
async def test_steps_and_hysteresis(dut):
    """档位映射 + 迟滞：在阈值附近抖动不应换档"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(30), 8)
    assert int(dut.cur_step.value) == 0, "30°C 应当在 0 档"
    assert int(dut.cur_duty.value) == DUTY[0]

    for temp, want in ((47, 1), (57, 2), (67, 3), (77, 4)):
        await feed(dut, c2code(temp), 8)
        assert int(dut.cur_step.value) == want, \
            f"{temp}°C 应当在 {want} 档，实际 {int(dut.cur_step.value)}"

    # 迟滞：从 4 档降到 76°C（低于 T4_UP 但高于 T4_DN）不该降档
    await feed(dut, c2code(72), 8)
    assert int(dut.cur_step.value) == 4, "72°C 仍在 T4_DN(70) 之上，不该降档"
    await feed(dut, c2code(68), 8)
    assert int(dut.cur_step.value) < 4, "68°C 低于 T4_DN(70)，应当降档"

    # 在 45°C 边界来回抖：档位不应反复跳
    await feed(dut, c2code(30), 10)
    s0 = int(dut.cur_step.value)
    for _ in range(5):
        await feed(dut, c2code(44), 2)
        await feed(dut, c2code(41), 2)
    assert int(dut.cur_step.value) == s0, "在 T1 阈值下方抖动却换了档 —— 迟滞没生效"

    dut._log.info("档位映射与迟滞：30/47/57/67/77°C 各归其位，阈值附近抖动不换档")


@cocotb.test()
async def test_min_duty_is_not_off(dut):
    """最低档不是停转 —— 这一条是故意不做到最静的"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(25), 10)
    assert int(dut.cur_step.value) == 0
    d = await measure_duty(dut, keep_temp=c2code(25))
    assert 20 < d < 30, f"最低档占空比 {d:.1f}%，应当在 25% 附近且**不为 0**"

    dut._log.info(f"最低档实测占空比 {d:.1f}%（不是停转）")


@cocotb.test()
async def test_overtemp_latch(dut):
    """过温强制满速：80°C 进、74°C 才出"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(30), 10)
    assert int(dut.forced_full.value) == 0

    await feed(dut, c2code(82), 3)
    assert int(dut.forced_full.value) == 1, "82°C 没有强制满速"
    assert int(dut.cur_duty.value) == 100
    d = await measure_duty(dut, keep_temp=c2code(82))
    assert d > 99, f"强制满速时占空比只有 {d:.1f}%"

    # 降到 78°C —— 低于 OT_ON 但高于 OT_OFF，**不该解除**
    await feed(dut, c2code(78), 5)
    assert int(dut.forced_full.value) == 1, "78°C 就解除了过温 —— 迟滞没生效"

    await feed(dut, c2code(70), 5)
    assert int(dut.forced_full.value) == 0, "70°C 应当解除过温"

    dut._log.info("过温：82°C 强制满速，78°C 不解除，70°C 才解除")


@cocotb.test()
async def test_stale_forces_full(dut):
    """**温度读不到就强制满速** —— 正常路径永远走不到，所以必须单独测"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(30), 10)
    assert int(dut.forced_full.value) == 0, "有温度时不该强制满速"

    # 之后再也不喂温度，等 stale 计数超限
    for _ in range(300):
        await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    # STALE_LIMIT 在测试里被参数覆盖成很小的值，见 Makefile
    assert int(dut.forced_full.value) == 1, \
        "温度长时间不更新却没有强制满速 —— 传感器坏了风扇就停了"
    assert int(dut.cur_duty.value) == 100

    # 温度恢复之后要能自己降下来
    await feed(dut, c2code(30), 10)
    assert int(dut.forced_full.value) == 0, "温度恢复后仍卡在满速"

    dut._log.info("温度陈旧：强制满速；温度恢复后自己降下来")


@cocotb.test()
async def test_override_cannot_beat_overtemp(dut):
    """手动覆盖盖不过过温 —— 调试口不能让人把芯片烤了"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(30), 10)
    dut.ovr_en.value = 1
    dut.ovr_duty.value = 10
    await feed(dut, c2code(30), 2)
    assert int(dut.cur_duty.value) == 10, "低温下手动覆盖应当生效"

    await feed(dut, c2code(85), 3)
    assert int(dut.cur_duty.value) == 100, "过温时手动覆盖竟然还压得住转速"

    dut.ovr_duty.value = 200          # 越界值要被夹到 100
    await feed(dut, c2code(30), 10)
    assert int(dut.cur_duty.value) == 100, "覆盖值越界没有被夹住"

    dut._log.info("手动覆盖：低温生效、过温无效、越界被夹到 100")


@cocotb.test()
async def test_pin_polarity(dut):
    """AA11 低=转：占空比越高，引脚为低的时间越长"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    await feed(dut, c2code(25), 10)
    d_low = await measure_duty(dut, keep_temp=c2code(25))
    await feed(dut, c2code(85), 5)
    d_high = await measure_duty(dut, keep_temp=c2code(85))

    assert d_high > d_low + 50, (
        f"高温({d_high:.1f}%) 并不比低温({d_low:.1f}%) 转得更多 —— "
        "AA11 的极性写反了，表现会是「温度越高越安静」")

    dut._log.info(f"极性：低温 {d_low:.1f}% → 高温 {d_high:.1f}%，方向正确")


@cocotb.test()
async def test_stuck_sensor_forces_full(dut):
    """读数一个比特不变 → 强制满速（真机上真的这么坏过）"""
    cocotb.start_soon(Clock(dut.clk, 13, unit="ns").start())
    await reset(dut)

    # 先喂两个**不同**的温度，让卡死计数器清零、档位落到 0
    await feed(dut, c2code(30), 6)
    await feed(dut, c2code(31), 6)
    assert int(dut.forced_full.value) == 0, "温度正常抖动时不该强制满速"
    assert int(dut.sensor_stuck.value) == 0

    # 之后一直喂**完全相同**的码。STUCK_LIMIT 在测试里被压到很小，见 Makefile。
    await feed(dut, c2code(31), 40)
    assert int(dut.sensor_stuck.value) == 1, \
        "读数几十次一模一样却没报卡死 —— 传感器死了风扇会一直停在最低档"
    assert int(dut.forced_full.value) == 1
    assert int(dut.cur_duty.value) == 100

    # 温度真的动了就该自己恢复
    await feed(dut, c2code(33), 4)
    assert int(dut.sensor_stuck.value) == 0, "读数变了却还卡在卡死状态"
    assert int(dut.forced_full.value) == 0

    dut._log.info("传感器卡死：读数不变 → 强制满速；读数一变 → 自己恢复")
