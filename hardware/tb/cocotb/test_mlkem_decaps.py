"""cocotb：整个 ML-KEM.Decaps 核的对拍

黄金模型是 hardware/model/mlkem_oracle.py 的 mlkem_decaps —— 那份实现拿
NIST ACVP 的 encapDecap 向量验过（预言机 F），所以这里不是"自己和自己比"。

三件事必须分开验，因为它们会各自独立地错：

  ① **正常密文**：K 必须等于封装时的 K。这条只验"解密 + 重加密 + 比对通过"。
  ② **被改过的密文**：K 必须等于隐式拒绝值 J(z‖c) = SHAKE256(z‖c, 32)，
     而不是"某个错误的 K"、也不是全零。这条验的是拒绝那一路真的接对了 ——
     只测 ① 的话，把拒绝值写成常量也能过。
  ③ **拍数与密文内容无关**：正常与被改过的密文必须用**完全相同的拍数**。
     这是 FO 变换里最容易漏的一条时序信道，见 decaps.v 头部的说明。

另外单验 dk_hash_ok：把 dk 里存的 h 改一个比特，它必须掉下来。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

import tbutil  # noqa: F401
from tbutil import s16

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mlkem_oracle import (  # noqa: E402
    PARAMS, PARAMS_C, mlkem_decaps, mlkem_encaps, mlkem_keygen, poly_frombytes,
)

PARAM_SET = {"ML-KEM-512": 0, "ML-KEM-768": 1, "ML-KEM-1024": 2}
SL_S = 0


async def bank_poly(dut, slot: int) -> list[int]:
    out = []
    for n in range(256):
        dut.dbg_addr.value = (slot << 8) | n
        await Timer(1, unit="ns")
        out.append(s16(int(dut.dbg_data.value)))
    return out


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.param_set.value = 1
    dut.dk_valid.value = 0
    dut.dk_data.value = 0
    dut.c_valid.value = 0
    dut.c_data.value = 0
    dut.out_ready.value = 1
    dut.dbg_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def run_decaps(dut, name, dk: bytes, c: bytes, feed=None, ready=None):
    """跑一次 Decaps，返回 (K, 拍数)

    dk 与 c 是两条独立的字节流，核先吃完 dk 再吃 c；两条都挂在同一个采样
    循环里，所以"谁先谁后"由 RTL 的 ready 决定，测试不替它安排顺序。
    """
    dut.param_set.value = PARAM_SET[name]
    dut.out_ready.value = 1

    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"

    got = bytearray()
    dpos = 0
    cpos = 0
    saw_last = False
    ticks = 0
    for tick in range(6_000_000):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")

        push_d = dpos < len(dk) and (feed is None or feed(tick))
        dut.dk_valid.value = 1 if push_d else 0
        dut.dk_data.value = dk[dpos] if push_d else 0

        push_c = cpos < len(c) and (feed is None or feed(tick))
        dut.c_valid.value = 1 if push_c else 0
        dut.c_data.value = c[cpos] if push_c else 0

        rdy = 1 if ready is None else (1 if ready(tick) else 0)
        dut.out_ready.value = rdy
        await Timer(1, unit="ns")

        if push_d and int(dut.dk_ready.value) == 1:
            dpos += 1
        if push_c and int(dut.c_ready.value) == 1:
            cpos += 1
        if int(dut.out_valid.value) == 1 and rdy == 1:
            got.append(int(dut.out_data.value))
            if int(dut.out_last.value) == 1:
                saw_last = True
        ticks = tick + 1
        if int(dut.done.value) == 1:
            break

    assert dpos == len(dk), f"dk 只吃进了 {dpos} 字节，应当是 {len(dk)}"
    assert cpos == len(c), f"c 只吃进了 {cpos} 字节，应当是 {len(c)}"
    assert saw_last, "跑完了却没见到 out_last"
    assert int(dut.done.value) == 1, "超时：核没有完成"
    assert len(got) == 32, f"输出了 {len(got)} 字节，应当是 32"
    # 拍数直接从这里报出来，进展文档里的数字就是这一行打的，不是估的
    dut._log.info(f"{name}：{ticks} 拍（@100 MHz ≈ {ticks / 100:.0f} µs）")
    return bytes(got), ticks


def kat(name, seed=0):
    """造一组自洽的 (dk, c, K)"""
    d = bytes(((seed + i) & 0xFF) for i in range(32))
    z = bytes(((seed + 0x40 + i) & 0xFF) for i in range(32))
    m = bytes(((seed + 0x80 + i) & 0xFF) for i in range(32))
    ek, dk = mlkem_keygen(d, z, name)
    shared, c = mlkem_encaps(ek, m, name)
    return dk, c, shared


@cocotb.test()
async def test_decaps_768(dut):
    """ML-KEM-768：中间量 ŝ 逐系数比对，K 逐字节比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-768"
    k, _ = PARAMS[name]
    dk, c, shared = kat(name)

    got, _ = await run_decaps(dut, name, dk, c)

    # ---- 第一层：从 dk 解出来的 ŝ ----
    for i in range(k):
        want = poly_frombytes(dk[384 * i:384 * (i + 1)])
        assert await bank_poly(dut, SL_S + i) == want, f"ŝ[{i}] 不一致"

    # ---- 第二层：最终的 K ----
    assert got == shared, "共享密钥与封装时的 K 不一致"
    assert got == mlkem_decaps(dk, c, name), "与黄金模型的 Decaps 不一致"
    assert int(dut.dk_hash_ok.value) == 1, "合法 dk 的哈希自检不该失败"

    dut._log.info("mlkem_decaps ML-KEM-768：ŝ 逐系数一致，K 32 字节逐字节一致")


@cocotb.test()
async def test_decaps_512_1024(dut):
    """另外两个参数集：k、du、dv 全都不同，走同一份数据通路"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for name in ("ML-KEM-512", "ML-KEM-1024"):
        dk, c, shared = kat(name, seed=PARAM_SET[name] * 0x11)
        got, _ = await run_decaps(dut, name, dk, c)
        assert got == shared, f"{name}：K 与封装时的不一致"
        assert int(dut.dk_hash_ok.value) == 1, f"{name}：哈希自检不该失败"
        du, dv = PARAMS_C[name]
        dut._log.info(f"{name}：K 32 字节全对（du={du} dv={dv} 也对上了）")


@cocotb.test()
async def test_decaps_implicit_reject(dut):
    """密文被改过 → 必须返回 J(z‖c)，而不是别的任何东西

    只验"K 不等于正确值"是不够的：拒绝值写成全零、写成常量、或者错用了
    z‖c 之外的输入，都能让"不等于正确值"成立。所以这里直接跟黄金模型的
    J(z‖c) 逐字节比。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-768"
    dk, c, shared = kat(name, seed=7)

    # 改三个位置：第一个字节、正中间、最后一个字节。
    # 只改第一个字节的话，"比到第一个不同就停"的实现照样能过。
    for where in (0, len(c) // 2, len(c) - 1):
        bad = bytearray(c)
        bad[where] ^= 0x01
        bad = bytes(bad)

        want = mlkem_decaps(dk, bad, name)
        assert want != shared, "改过的密文不该还解出原来的 K（黄金模型自检）"

        got, _ = await run_decaps(dut, name, dk, bad)
        assert got == want, f"第 {where} 字节被改：隐式拒绝值与黄金模型不一致"

    dut._log.info("三处改动的密文都返回了 J(z‖c)，与黄金模型逐字节一致")


@cocotb.test()
async def test_decaps_constant_time(dut):
    """拍数必须与密文内容无关

    正常密文、第一个字节被改、最后一个字节被改 —— 三次的拍数必须完全相同。
    如果比对写成"一发现不同就跳出"，第一个字节被改的那次会明显更快，
    这条用例就会挂。这是 decaps.v 里唯一一条不能妥协的性质。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    dk, c, _ = kat(name, seed=11)

    first = bytearray(c); first[0] ^= 0xFF
    last = bytearray(c); last[-1] ^= 0xFF

    _, t_ok = await run_decaps(dut, name, dk, c)
    _, t_first = await run_decaps(dut, name, dk, bytes(first))
    _, t_last = await run_decaps(dut, name, dk, bytes(last))

    assert t_ok == t_first == t_last, (
        f"拍数随密文内容变了：正常 {t_ok}、首字节改 {t_first}、末字节改 {t_last}"
        " —— 密文比对提前退出了")
    dut._log.info(f"三种密文都是 {t_ok} 拍，比对没有提前退出")


@cocotb.test()
async def test_decaps_dk_hash_check(dut):
    """dk 里存的 h 与 H(ek) 不符 → dk_hash_ok 必须掉下来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    k, _ = PARAMS[name]
    dk, c, _ = kat(name, seed=23)

    bad = bytearray(dk)
    bad[768 * k + 32] ^= 0x80          # h 的第一个字节
    await run_decaps(dut, name, bytes(bad), c)
    assert int(dut.dk_hash_ok.value) == 0, "h 被改了，自检却说没问题"

    await run_decaps(dut, name, dk, c)
    assert int(dut.dk_hash_ok.value) == 1, "合法 dk 的自检不该失败"

    dut._log.info("dk 的哈希自检（FIPS 203 §7.3）会咬人")


@cocotb.test()
async def test_decaps_backpressure(dut):
    """两条输入流断续给、输出断续收，K 必须逐字节不变"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    dk, c, shared = kat(name, seed=31)

    got, _ = await run_decaps(dut, name, dk, c,
                              feed=lambda t: (t % 3) != 0,
                              ready=lambda t: (t % 5) < 2)
    assert got == shared, "背压下 K 与黄金模型不一致"
    dut._log.info("背压：输入三拍给两拍、输出五拍收两拍，K 仍逐字节一致")
