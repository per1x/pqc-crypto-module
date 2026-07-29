"""cocotb：256 点 NTT 核对拍 —— L2 层（路线图 §5.1.3 的 ntt_vectors）

比对：RTL == vectors/rtl/ntt.hex == model/ref_model.py 现算。

⚠️ **这三者并不互相独立**：向量是 export_vectors.py 从 ref_model 生成的，
所以上面这条实际只等价于 `RTL == ref_model`。一张"自洽但错误"的旋转因子表
照样能全绿。ref_model 本身的正确性由 **model/ntt_oracle.py** 的两道
独立预言机保证（schoolbook 负循环卷积 + 重建 ML-KEM KeyGen 比对 ACVP 向量），
那里才是"对得上真实 ML-KEM"的依据。

同时记录 cycle 数 —— 这就是无板阶段能拿到的性能数据（§5.3.3 那段注释）。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "model"))

from ref_model import invntt, ntt  # noqa: E402

VEC = ROOT / "vectors" / "rtl"


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def load_pairs(name: str):
    """文件里一行输入、一行输出，交替"""
    path = VEC / name
    if not path.exists():
        raise FileNotFoundError(f"{path} 不存在 —— 先跑 model/export_vectors.py")
    rows = [l.split() for l in path.read_text().splitlines()
            if l.strip() and not l.startswith("#")]
    return [(rows[i], rows[i + 1]) for i in range(0, len(rows) - 1, 2)]


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
        dut.wr_data.value = c & 0xFFFF
        await RisingEdge(dut.clk)
    dut.wr_en.value = 0
    await RisingEdge(dut.clk)


async def run_transform(dut, inverse: int) -> int:
    """发一次变换命令并等它做完。

    ⚠️ done 是**电平**（保持到下一次 start），所以这里必须先确认它被新命令清掉，
    再去等它重新拉高。否则上一次残留的 done=1 会让等待循环立刻退出，
    在变换还没做完时就去读系数 —— 这正是 done 从脉冲改成电平后暴露出来的
    握手时序问题（脉冲语义下碰巧不会出错）。
    """
    dut.inverse.value = inverse
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")          # 让这一拍的非阻塞赋值落定
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"
    cycles = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        cycles += 1
        assert cycles < 20000, "NTT 超时，检查状态机"
    return cycles


async def read_poly(dut):
    out = []
    for i in range(256):
        dut.rd_addr.value = i
        await Timer(1, unit="ns")
        out.append(s16(int(dut.rd_data.value)))
    return out


@cocotb.test()
async def test_ntt_forward(dut):
    """正变换：与向量文件和 Python 模型三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("ntt.hex")
    assert len(pairs) == 20
    total_cycles = 0
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s16(int(x, 16)) for x in vin]
        expect = [s16(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        total_cycles += await run_transform(dut, 0)
        got = await read_poly(dut)
        model = ntt(list(coeffs))
        assert got == expect == model, (
            f"第 {idx} 组不匹配\n  RTL   前8={got[:8]}\n  向量  前8={expect[:8]}\n"
            f"  模型  前8={model[:8]}")
    dut._log.info(f"ntt 正变换 {len(pairs)} 组与 ref_model 一致（其正确性见 ntt_oracle.py），"
                  f"平均 {total_cycles // len(pairs)} cycles/次")


@cocotb.test()
async def test_ntt_inverse(dut):
    """逆变换：同样三方一致"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    pairs = load_pairs("invntt.hex")
    assert len(pairs) == 20        # 与正变换测试对称（原来只有正变换断言了条数）
    for idx, (vin, vexp) in enumerate(pairs):
        coeffs = [s16(int(x, 16)) for x in vin]
        expect = [s16(int(x, 16)) for x in vexp]
        await load_poly(dut, coeffs)
        await run_transform(dut, 1)
        got = await read_poly(dut)
        model = invntt(list(coeffs))
        assert got == expect == model, f"第 {idx} 组逆变换不匹配"
    dut._log.info(f"ntt 逆变换 {len(pairs)} 组与 ref_model 一致")


@cocotb.test()
async def test_reset_is_clean(dut):
    """复位后内部计数器有确定值：Icarus 是 4-state，没复位的话会看到 X

    判据不看内部信号（那样太依赖实现），而是看**复位后立刻发一次变换**能不能
    算对 —— 如果 len/grp/j/k 复位后是 X，第一次变换就会错。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.done.value) == 0

    pairs = load_pairs("ntt.hex")
    vin, vexp = pairs[0]
    coeffs = [s16(int(x, 16)) for x in vin]
    expect = [s16(int(x, 16)) for x in vexp]
    await load_poly(dut, coeffs)
    await run_transform(dut, 0)
    got = await read_poly(dut)
    assert got == expect, "复位后的第一次变换就算错了 —— 检查计数器复位"

    # 中途复位再来一次，也必须干净
    await reset(dut)
    assert int(dut.done.value) == 0, "复位必须清 done"
    await load_poly(dut, coeffs)
    await run_transform(dut, 0)
    assert await read_poly(dut) == expect, "复位后重跑结果不一致"
    dut._log.info("复位干净：复位后第一次变换即正确，中途复位可重跑")


@cocotb.test()
async def test_done_is_level(dut):
    """done 必须是**电平**：置位后保持到下一次 start，而不是 1 周期脉冲

    这条直接对应 accel.h 的契约 —— 软件"轮询 STATUS.DONE"，
    如果 done 只高 1 拍，真实寄存器/AXI 在任意时刻采样就会漏掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.done.value) == 0, "复位后 done 必须为 0"

    src = [((i * 13) % 3329) - 1664 for i in range(256)]
    await load_poly(dut, src)
    await run_transform(dut, 0)
    assert int(dut.done.value) == 1

    # 空转 50 拍，done 必须一直是 1
    for _ in range(50):
        await RisingEdge(dut.clk)
        assert int(dut.done.value) == 1, "done 掉了 —— 它应当保持到下一次 start"

    # 写系数也不该清掉 done
    await load_poly(dut, src)
    assert int(dut.done.value) == 1, "写系数不该清 done"

    # 下一次 start 才清
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当被清掉"
    # 跑完又回到 1
    cycles = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        cycles += 1
        assert cycles < 20000
    dut._log.info("done 为电平语义：保持到下一次 start 才清")


@cocotb.test()
async def test_roundtrip(dut):
    """性质断言：invntt(ntt(x)) ≡ x·2^16 (mod q)，**不是恒等**"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    Q, MONT = 3329, 2285
    src = [((i * 37) % Q) - Q // 2 for i in range(256)]
    await load_poly(dut, src)
    c1 = await run_transform(dut, 0)
    fwd = await read_poly(dut)
    await load_poly(dut, fwd)
    c2 = await run_transform(dut, 1)
    back = await read_poly(dut)
    for a, b in zip(src, back):
        assert (a * MONT - b) % Q == 0, f"往返不符 x·mont：{a} -> {b}"
    assert fwd != src, "正变换没改变系数？"
    dut._log.info(f"往返性质成立；正变换 {c1} cycles，逆变换 {c2} cycles")
