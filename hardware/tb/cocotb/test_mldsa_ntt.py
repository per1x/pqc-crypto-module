"""cocotb：ML-DSA 的 256 点 NTT 核对拍

比对 RTL == vectors/rtl/mldsa_ntt.hex == mldsa_model 现算。这三者并不互相独立
（向量由模型生成），模型本身的正确性由 hardware/model/mldsa_oracle.py 的
schoolbook 负循环卷积与 ACVP 重建两道预言机保证。

因此这里额外做两件事：
  一、往返性质 invntt(ntt(x)) ≡ x·2³²（**不是恒等**）
  二、握手语义 —— done 是电平、复位后计数器有确定值，两条都对应 accel.h 的契约

同时记录 cycle 数，作为无板阶段的性能数据。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

from tbutil import load_pairs, s32

from mldsa_model import Q, invntt_tomont, ntt  # noqa: E402


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.wr_en.value = 0
    dut.inverse.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_poly(dut, coeffs):
    for i, c in enumerate(coeffs):
        dut.wr_en.value = 1
        dut.wr_addr.value = i
        dut.wr_data.value = c & 0xFFFFFFFF
        await RisingEdge(dut.clk)
    dut.wr_en.value = 0
    await RisingEdge(dut.clk)


async def run_transform(dut, inverse: int) -> int:
    """发一次变换命令并等它做完。

    done 是电平（保持到下一次 start），所以必须先确认它被新命令清掉，
    再去等它重新拉高；否则上一次残留的 done=1 会让等待循环立刻退出。
    """
    dut.inverse.value = inverse
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"
    cycles = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        cycles += 1
        assert cycles < 20000, "NTT 超时，检查状态机"
    return cycles


async def read_poly(dut):
    """读口是**同步读**：地址给出后要等一个上升沿，rd_data 才是那个地址的内容。

    系数存储从寄存器阵列换成 BRAM 之后组合读就没了（BRAM 没有组合读口），
    这里的一拍延迟是硬件真实时序，不是测试的将就。
    """
    out = []
    for i in range(256):
        dut.rd_addr.value = i
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        out.append(s32(int(dut.rd_data.value)))
    return out


@cocotb.test()
async def test_ntt_forward(dut):
    """正变换：与向量文件和模型三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("mldsa_ntt.hex")
    assert len(pairs) == 10
    total = 0
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s32(int(x, 16)) for x in vin]
        expect = [s32(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        total += await run_transform(dut, 0)
        got = await read_poly(dut)
        model = ntt(list(coeffs))
        assert got == expect == model, (
            f"第 {idx} 组不匹配\n  RTL  前4={got[:4]}\n  向量 前4={expect[:4]}")
    dut._log.info(f"mldsa 正变换 {len(pairs)} 组三方一致，平均 {total // len(pairs)} cycles/次")


@cocotb.test()
async def test_ntt_inverse(dut):
    """逆变换：同样三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("mldsa_invntt.hex")
    assert len(pairs) == 10
    total = 0
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s32(int(x, 16)) for x in vin]
        expect = [s32(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        total += await run_transform(dut, 1)
        got = await read_poly(dut)
        model = invntt_tomont(list(coeffs))
        assert got == expect == model, f"第 {idx} 组逆变换不匹配"
    dut._log.info(f"mldsa 逆变换 {len(pairs)} 组三方一致，平均 {total // len(pairs)} cycles/次")


@cocotb.test()
async def test_roundtrip(dut):
    """性质断言：invntt(ntt(x)) ≡ x·2³² (mod q)，**不是恒等**"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    mont = (1 << 32) % Q
    src = [((i * 65537) % Q) - Q // 2 for i in range(256)]
    await load_poly(dut, src)
    c1 = await run_transform(dut, 0)
    fwd = await read_poly(dut)
    await load_poly(dut, fwd)
    c2 = await run_transform(dut, 1)
    back = await read_poly(dut)
    for a, b in zip(src, back):
        assert (a * mont - b) % Q == 0, f"往返不符 x·2³²：{a} -> {b}"
    assert fwd != src, "正变换没改变系数"
    dut._log.info(f"mldsa 往返性质成立；正变换 {c1} cycles，逆变换 {c2} cycles")


@cocotb.test()
async def test_done_is_level(dut):
    """done 必须是电平：置位后保持到下一次 start

    这条直接对应 accel.h 的契约 —— 软件轮询 STATUS.DONE，
    如果 done 只高 1 拍，真实寄存器/AXI 在任意时刻采样就会漏掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.done.value) == 0, "复位后 done 必须为 0"

    src = [((i * 12345) % Q) - Q // 2 for i in range(256)]
    await load_poly(dut, src)
    await run_transform(dut, 0)
    assert int(dut.done.value) == 1

    for _ in range(50):
        await RisingEdge(dut.clk)
        assert int(dut.done.value) == 1, "done 掉了 —— 它应当保持到下一次 start"

    await load_poly(dut, src)
    assert int(dut.done.value) == 1, "写系数不该清 done"
    dut._log.info("mldsa_ntt_core: done 为电平语义")


@cocotb.test()
async def test_reset_is_clean(dut):
    """复位后计数器有确定值：Icarus 是 4-state，没复位的话会看到 X

    判据不看内部信号，而是看复位后立刻发一次变换能不能算对 ——
    如果 len/grp/j/k 复位后是 X，第一次变换就会错。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("mldsa_ntt.hex")
    vin, vexp = pairs[0]
    coeffs = [s32(int(x, 16)) for x in vin]
    expect = [s32(int(x, 16)) for x in vexp]

    await load_poly(dut, coeffs)
    await run_transform(dut, 0)
    assert await read_poly(dut) == expect, "复位后的第一次变换就算错了"

    await reset(dut)
    assert int(dut.done.value) == 0, "复位必须清 done"
    await load_poly(dut, coeffs)
    await run_transform(dut, 0)
    assert await read_poly(dut) == expect, "复位后重跑结果不一致"
    dut._log.info("mldsa_ntt_core: 复位干净，复位后第一次变换即正确")
