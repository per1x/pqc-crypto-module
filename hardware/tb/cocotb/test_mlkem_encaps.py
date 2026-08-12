"""cocotb：整个 ML-KEM.Encaps 核的对拍

黄金模型是 hardware/model/mlkem_oracle.py 的 mlkem_encaps —— 那份实现拿
NIST ACVP 的 encapDecap 向量验过（预言机 E），所以这里不是"自己和自己比"。

分两层查，理由同 KeyGen：
  ① 中间量：r̂、e₁、e₂、以及从 ek 解出来的 t̂，逐系数比对（DEBUG_BANK=1）。
  ② 最终字节：K‖c 逐字节比对。

ek 走的是**字节流输入**（384k+32 字节），所以顺带把"上游断续给字节"
和"下游断续收字节"两种背压都压上去。
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
    G, H, PARAMS, mlkem_encaps, mlkem_keygen, ntt, sample_poly_cbd,
)

PARAM_SET = {"ML-KEM-512": 0, "ML-KEM-768": 1, "ML-KEM-1024": 2}
SL_RHAT, SL_E1, SL_THAT, SL_E2 = 0, 4, 8, 12


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
    dut.m_in.value = 0
    dut.ek_valid.value = 0
    dut.ek_data.value = 0
    dut.out_ready.value = 0
    dut.dbg_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def run_encaps(dut, name, ek: bytes, m: bytes, feed=None, ready=None):
    """跑一次 Encaps，返回输出字节

    feed / ready 给 None 就是全程拉高；给 tick -> bool 就是断续背压。
    两种情况共用这一段采样代码 —— 否则"背压下也一致"验证的只是两段
    测试代码碰巧写法不同。
    """
    dut.param_set.value = PARAM_SET[name]
    dut.m_in.value = int.from_bytes(m, "little")
    dut.out_ready.value = 1

    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"

    got = bytearray()
    pos = 0
    saw_last = False
    for tick in range(6_000_000):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")

        push = pos < len(ek) and (feed is None or feed(tick))
        dut.ek_valid.value = 1 if push else 0
        dut.ek_data.value = ek[pos] if push else 0

        rdy = 1 if ready is None else (1 if ready(tick) else 0)
        dut.out_ready.value = rdy
        await Timer(1, unit="ns")

        if push and int(dut.ek_ready.value) == 1:
            pos += 1
        if int(dut.out_valid.value) == 1 and rdy == 1:
            got.append(int(dut.out_data.value))
            if int(dut.out_last.value) == 1:
                saw_last = True
        if int(dut.done.value) == 1:
            break

    assert pos == len(ek), f"ek 只吃进了 {pos} 字节，应当是 {len(ek)}"
    assert saw_last, "跑完了却没见到 out_last"
    assert int(dut.done.value) == 1, "超时：核没有完成"
    # 拍数直接从这里报出来，进展文档里的数字就是这一行打的，不是估的
    dut._log.info(f"{name}：{tick + 1} 拍（@100 MHz ≈ {(tick + 1) / 100:.0f} µs）")
    return bytes(got)


@cocotb.test()
async def test_encaps_768(dut):
    """ML-KEM-768：中间量逐系数比对，K‖c 逐字节比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-768"
    k, eta1 = PARAMS[name]
    ek, _ = mlkem_keygen(bytes(range(32)), bytes(range(32, 64)), name)
    m = bytes(range(64, 96))

    got = await run_encaps(dut, name, ek, m)

    # ---- 第一层：中间量 ----
    _, rand = G(m + H(ek))
    for i in range(k):
        want = ntt(sample_poly_cbd(rand, i, eta1))
        assert await bank_poly(dut, SL_RHAT + i) == want, f"r̂[{i}] 不一致"
    for i in range(k):
        want = sample_poly_cbd(rand, k + i, 2)
        assert await bank_poly(dut, SL_E1 + i) == want, f"e₁[{i}] 不一致"
    assert await bank_poly(dut, SL_E2) == sample_poly_cbd(rand, 2 * k, 2), "e₂ 不一致"

    # 从 ek 解出来的 t̂：存储里存的就是 ByteDecode12 的原值 [0, 2¹²)
    for i in range(k):
        blk = ek[384 * i:384 * (i + 1)]
        want = []
        for n in range(128):
            b0, b1, b2 = blk[3 * n], blk[3 * n + 1], blk[3 * n + 2]
            want.append(b0 | ((b1 & 0xF) << 8))
            want.append((b1 >> 4) | (b2 << 4))
        assert await bank_poly(dut, SL_THAT + i) == want, f"t̂[{i}] 不一致"

    # ---- 第二层：最终字节 ----
    shared, c = mlkem_encaps(ek, m, name)
    assert len(got) == 32 + len(c), f"输出 {len(got)} 字节，应当是 {32 + len(c)}"
    assert got[:32] == shared, "共享密钥 K 与黄金模型不一致"
    assert got[32:] == c, "密文 c 与黄金模型不一致"

    dut._log.info(
        f"mlkem_encaps ML-KEM-768：r̂/e₁/e₂/t̂ 逐系数一致，K 32 字节、c {len(c)} 字节逐字节一致")


@cocotb.test()
async def test_encaps_512_1024(dut):
    """另外两个参数集：k、η1、du、dv 全都不同，走同一份数据通路"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for name in ("ML-KEM-512", "ML-KEM-1024"):
        ek, _ = mlkem_keygen(bytes([PARAM_SET[name]] * 32), bytes([0x5A] * 32), name)
        m = bytes([PARAM_SET[name] * 17 & 0xFF] * 32)
        shared, c = mlkem_encaps(ek, m, name)
        got = await run_encaps(dut, name, ek, m)
        assert got == shared + c, f"{name}：K‖c 与黄金模型不一致"
        dut._log.info(f"{name}：K 32 + c {len(c)} 字节全对（du/dv 也对上了）")


@cocotb.test()
async def test_encaps_backpressure(dut):
    """ek 上游断续给、密文下游断续收，结果必须与全程满速时逐字节相同

    ek 那一路同时喂给 H 的海绵，海绵每 136 字节要停 24 拍置换；
    输出那一路的字节来自打包器，它自己也会因为比特不够而停。
    三种停顿叠在一起才是真通路上的样子。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    ek, _ = mlkem_keygen(bytes([3] * 32), bytes([4] * 32), name)
    m = bytes([5] * 32)
    shared, c = mlkem_encaps(ek, m, name)

    got = await run_encaps(dut, name, ek, m,
                           feed=lambda t: (t % 3) != 0,
                           ready=lambda t: (t % 5) < 2)
    assert got == shared + c, "背压下输出与黄金模型不一致"
    dut._log.info("背压：ek 三拍给两拍、下游五拍收两拍，输出仍逐字节一致")
