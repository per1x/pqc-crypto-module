"""cocotb：整个 ML-KEM.KeyGen 核的对拍

黄金模型是 hardware/model/mlkem_oracle.py 的 mlkem_keygen —— 那个实现是拿
NIST ACVP 向量验过的（oracle_d），所以这里的比对不是"自己和自己比"。

分两层查：
  ① 中间量：ŝ、ê、t̂ 逐系数比对。RTL 里这三个量都在多项式存储里，
     DEBUG_BANK=1 时把存储的读口引出来看。放在这一层是因为
     ek/dk 一旦对不上，光看几千个字节完全定位不到是哪一步错了。
  ② 最终字节：ek、dk 逐字节比对。这一层才是"这个核对不对"的判据。

⚠️ DEBUG_BANK 是**仿真专用**。综合时必须是 0 —— 那个口直连私钥系数，
   引出来等于在密码边界上开个洞。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

import tbutil  # noqa: F401  —— 导入即把 hardware/model 放进 sys.path
from tbutil import s16

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mlkem_oracle import (  # noqa: E402
    G, PARAMS, mlkem_keygen, ntt, sample_poly_cbd,
)

# 参数集编码要与 RTL 的 param_set 对上
PARAM_SET = {"ML-KEM-512": 0, "ML-KEM-768": 1, "ML-KEM-1024": 2}

SL_SHAT, SL_EHAT, SL_THAT = 0, 4, 8


async def bank_poly(dut, slot: int, idx: int) -> list[int]:
    """读出多项式存储里第 (slot+idx) 个槽的 256 个系数"""
    out = []
    for n in range(256):
        dut.dbg_addr.value = ((slot + idx) << 8) | n
        await Timer(1, unit="ns")
        out.append(s16(int(dut.dbg_data.value)))
    return out


async def run_keygen(dut, name: str, d: bytes, z: bytes, ready=None):
    """跑一次完整 KeyGen，返回输出的全部字节

    ready 给 None 就是全程拉高；给一个 tick -> bool 的函数就是断续背压。
    两种情况必须走同一段采样代码 —— 否则"背压下也一致"这句话就没被真正验证，
    验证的只是两段测试代码碰巧写法不同。
    """
    dut.param_set.value = PARAM_SET[name]
    dut.d_in.value = int.from_bytes(d, "little")
    dut.z_in.value = int.from_bytes(z, "little")
    dut.out_ready.value = 1

    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"

    got = bytearray()
    saw_last = False
    for tick in range(4_000_000):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        # ready 要在读 valid 之前定下来，并且这一拍定的值就是下一个上升沿
        # 生效的值 —— 先读 valid 再改 ready 会把两拍的信号凑在一起看
        rdy = 1 if ready is None else (1 if ready(tick) else 0)
        dut.out_ready.value = rdy
        await Timer(1, unit="ns")
        if int(dut.out_valid.value) == 1 and rdy == 1:
            got.append(int(dut.out_data.value))
            if int(dut.out_last.value) == 1:
                saw_last = True
        if int(dut.done.value) == 1:
            break
    assert saw_last, "跑完了却没见到 out_last"
    assert int(dut.done.value) == 1, "超时：核没有完成"
    # 拍数直接从这里报出来，进展文档里的数字就是这一行打的，不是估的
    dut._log.info(f"{name}：{tick + 1} 拍（@100 MHz ≈ {(tick + 1) / 100:.0f} µs）")
    return bytes(got)


async def reset(dut):
    dut.rst_n.value = 0
    dut.start.value = 0
    dut.param_set.value = 1
    dut.d_in.value = 0
    dut.z_in.value = 0
    dut.out_ready.value = 0
    dut.dbg_addr.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


@cocotb.test()
async def test_keygen_768(dut):
    """ML-KEM-768：中间量 ŝ/ê/t̂ 逐系数比对，ek/dk 逐字节比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-768"
    k, eta1 = PARAMS[name]
    d = bytes(range(32))
    z = bytes(range(100, 132))

    got = await run_keygen(dut, name, d, z)

    # ---- 第一层：中间量 ----
    rho, sigma = G(d + bytes([k]))
    s_hat = [ntt(sample_poly_cbd(sigma, n, eta1)) for n in range(k)]
    e_hat = [ntt(sample_poly_cbd(sigma, k + n, eta1)) for n in range(k)]

    for i in range(k):
        assert await bank_poly(dut, SL_SHAT, i) == s_hat[i], f"ŝ[{i}] 不一致"
        assert await bank_poly(dut, SL_EHAT, i) == e_hat[i], f"ê[{i}] 不一致"

    # t̂ 从黄金模型的 ek 反解（ek 的前 384k 字节就是 ByteEncode12(t̂)）
    ek_ref, dk_ref = mlkem_keygen(d, z, name)
    for i in range(k):
        blk = ek_ref[384 * i:384 * (i + 1)]
        want = []
        for m in range(128):
            b0, b1, b2 = blk[3 * m], blk[3 * m + 1], blk[3 * m + 2]
            want.append(b0 | ((b1 & 0xF) << 8))
            want.append((b1 >> 4) | (b2 << 4))
        # 存储里存的是有符号代表元，编码时才折回 [0, q)
        assert [c % 3329 for c in await bank_poly(dut, SL_THAT, i)] == want, \
            f"t̂[{i}] 不一致"

    # ---- 第二层：最终字节 ----
    assert len(got) == len(ek_ref) + len(dk_ref), \
        f"输出 {len(got)} 字节，应当是 {len(ek_ref) + len(dk_ref)}"
    assert got[:len(ek_ref)] == ek_ref, "ek 与黄金模型不一致"
    assert got[len(ek_ref):] == dk_ref, "dk 与黄金模型不一致"

    dut._log.info(
        "mlkem_keygen ML-KEM-768：ŝ/ê/t̂ 逐系数一致，"
        f"ek {len(ek_ref)} 字节、dk {len(dk_ref)} 字节逐字节一致")


@cocotb.test()
async def test_keygen_512_1024(dut):
    """另外两个参数集：k 和 η1 都不同，走的是同一份数据通路"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for name in ("ML-KEM-512", "ML-KEM-1024"):
        d = bytes([PARAM_SET[name]] * 32)
        z = bytes([0xA5] * 32)
        ek_ref, dk_ref = mlkem_keygen(d, z, name)
        got = await run_keygen(dut, name, d, z)
        assert got == ek_ref + dk_ref, f"{name}：ek‖dk 与黄金模型不一致"
        dut._log.info(f"{name}：ek {len(ek_ref)} + dk {len(dk_ref)} 字节全对")


@cocotb.test()
async def test_keygen_backpressure(dut):
    """下游断续拉 ready：输出字节流不能因为背压而丢字节或错位

    真通路上游是 AXI4-Stream，ready 随时会掉。而且 ek 那两段还要同时喂给
    H 的海绵 —— 海绵每 136 字节要停下来置换一次，out_valid 那时必须落下去。
    这两种停顿叠在一起，结果仍必须与全程 ready=1 时逐字节相同。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    name = "ML-KEM-512"
    d = bytes([7] * 32)
    z = bytes([9] * 32)
    ek_ref, dk_ref = mlkem_keygen(d, z, name)

    got = await run_keygen(dut, name, d, z, ready=lambda t: (t % 5) < 2)
    assert got == ek_ref + dk_ref, "断续背压下输出与黄金模型不一致"
    dut._log.info("背压：ready 五拍拉高两拍，输出仍逐字节一致")
