"""cocotb：SM4 分组核（GB/T 32907-2016）

  ① **S 盒逐值**：256 个输入与模型里的表比对。SM4 的 S 盒标准正文只给表
     （代数参数要反推），所以这一层只保证"RTL 的表 == 模型的表"；
     真正把表钉住的是下面的 GB/T 向量 —— 表错一格，密文立刻不对。
  ② **GB/T 32907 附录 A.1**：官方单次加密向量，加解密两个方向。
  ③ **迭代加密**：同一个块反复加密一万次，与模型比对。GB/T 附录 A.2 给的是
     一百万次，那条由 `sym_oracle.py --million` 在模型侧跑满（见该用例注释）。
  ④ **随机往返** + 与 Python 参考实现逐字节比对。

另外单验 CK 常量的算式（拿模型里的 CK 表比对轮密钥）、拍数与数据无关、zeroize。
"""
import random
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from sym_oracle import (  # noqa: E402
    SM4_CT, SM4_KEY, SM4_PT, SM4_SBOX, sm4_crypt_block,
    sm4_round_keys,
)


async def reset(dut):
    dut.rst_n.value = 0
    dut.key_start.value = 0
    dut.key_in.value = 0
    dut.blk_start.value = 0
    dut.decrypt.value = 0
    dut.block_in.value = 0
    dut.zeroize.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_key(dut, key: bytes):
    dut.key_in.value = int.from_bytes(key, "big")
    dut.key_start.value = 1
    await RisingEdge(dut.clk)
    dut.key_start.value = 0
    for t in range(200):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        if int(dut.key_ready.value):
            return t + 1
    raise AssertionError("密钥扩展没完成")


async def run_block(dut, blk: bytes, decrypt: bool = False):
    dut.block_in.value = int.from_bytes(blk, "big")
    dut.decrypt.value = 1 if decrypt else 0
    dut.blk_start.value = 1
    await RisingEdge(dut.clk)
    dut.blk_start.value = 0
    for t in range(200):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        if int(dut.blk_done.value):
            return int(dut.block_out.value).to_bytes(16, "big"), t + 1
    raise AssertionError("分组处理没完成")


@cocotb.test()
async def test_sbox_exhaustive(dut):
    """SM4 S 盒 256 个输入逐值比对"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    sb = dut.u_s0
    for x in range(256):
        sb.x.value = x
        await Timer(1, unit="ns")
        got = int(sb.y.value)
        assert got == SM4_SBOX[x], \
            f"S[{x:02x}] = {got:02x}，应当是 {SM4_SBOX[x]:02x}"

    dut._log.info("SM4 S 盒：256 个输入逐值一致")


@cocotb.test()
async def test_round_keys(dut):
    """轮密钥与模型一致 —— 这一条盯的是 CK 那个算式

    CK 在 RTL 里是算出来的（28i、28i+7、28i+14、28i+21），在模型里是照
    定义生成的表。算式写错一位，这里立刻分道扬镳，而不必等到密文对不上
    才回头猜是哪一步。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await load_key(dut, SM4_KEY)
    want = sm4_round_keys(SM4_KEY)
    for i in range(32):
        got = int(dut.rk[i].value)
        assert got == want[i], \
            f"rk[{i}] = {got:08x}，应当是 {want[i]:08x}（CK 的算式可能写错了）"

    dut._log.info("32 个轮密钥逐个与模型一致，CK 的移位和算对了")


@cocotb.test()
async def test_gbt_vectors(dut):
    """GB/T 32907-2016 附录 A.1：官方向量，加解密两个方向"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = await load_key(dut, SM4_KEY)
    ct, b = await run_block(dut, SM4_PT)
    assert ct == SM4_CT, f"加密：{ct.hex()}，应当是 {SM4_CT.hex()}"

    pt, _ = await run_block(dut, SM4_CT, decrypt=True)
    assert pt == SM4_PT, f"解密：{pt.hex()}，应当是 {SM4_PT.hex()}"

    assert n == 32 and b == 33, f"拍数：密钥扩展 {n}、分组 {b}"
    dut._log.info(f"GB/T 附录 A.1：加解密都对（密钥扩展 {n} 拍、分组 {b} 拍）")


@cocotb.test()
async def test_iterated(dut):
    """迭代加密一万次，与模型逐字节一致

    GB/T 32907 附录 A.2 给的是**一百万次**迭代。那条在 cocotb 里要跑约一小时
    （一百万 × 35 拍），所以这里分两段接力：

      · RTL 跑一万次，与 Python 模型的一万次结果比对；
      · 模型自己跑满一百万次、对上 GB/T 的 A.2 密文
        —— `python3 hardware/model/sym_oracle.py --million`。

    两段接力和直接跑一百万次的区别，只是"RTL 与模型在第 10000 次仍然一致"
    而不是第 1000000 次。任何一轮、任何一个常数上的偏差在第一次迭代就会
    发散，一万次早就足够放大 —— 这不是妥协掉了什么，是把一小时的机器时间
    换成了等价的两段。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = 10_000
    await load_key(dut, SM4_KEY)
    x = SM4_PT
    for i in range(n):
        x, _ = await run_block(dut, x)
        if i == 0:
            assert x == SM4_CT, "第一次迭代就与附录 A.1 不符"

    want = SM4_PT
    for _ in range(n):
        want = sm4_crypt_block(SM4_KEY, want)

    assert x == want, f"一万次之后 {x.hex()}，模型给的是 {want.hex()}"
    dut._log.info(f"迭代 {n} 次：与模型逐字节一致"
                  "（一百万次那条由 sym_oracle.py --million 覆盖）")


@cocotb.test()
async def test_random_roundtrip(dut):
    """随机密钥与明文：与参考实现一致，且往返回到原文"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(20260812)
    for _ in range(8):
        key = bytes(rng.randrange(256) for _ in range(16))
        pt = bytes(rng.randrange(256) for _ in range(16))
        await load_key(dut, key)
        ct, _ = await run_block(dut, pt)
        want = sm4_crypt_block(key, pt)
        assert ct == want, (f"加密不一致\n  key={key.hex()}\n  pt ={pt.hex()}\n"
                            f"  得 {ct.hex()}\n  期 {want.hex()}")
        back, _ = await run_block(dut, ct, decrypt=True)
        assert back == pt, f"往返回不到原文：{back.hex()} != {pt.hex()}"

    dut._log.info("随机 8 组：与参考实现逐字节一致，往返回到原文")


@cocotb.test()
async def test_cycles_and_zeroize(dut):
    """拍数与数据无关；zeroize 一拍清掉轮密钥"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(3)
    seen_k, seen_b = set(), set()
    for _ in range(6):
        key = bytes(rng.randrange(256) for _ in range(16))
        pt = bytes(rng.randrange(256) for _ in range(16))
        seen_k.add(await load_key(dut, key))
        _, b = await run_block(dut, pt)
        seen_b.add(b)
        _, b = await run_block(dut, pt, decrypt=True)
        seen_b.add(b)
    assert seen_k == {32}, f"密钥扩展拍数随数据变了：{seen_k}"
    assert seen_b == {33}, f"分组拍数随数据变了：{seen_b}"

    await load_key(dut, SM4_KEY)
    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    assert int(dut.key_ready.value) == 0, "zeroize 之后 key_ready 该掉下来"
    for i in range(32):
        assert int(dut.rk[i].value) == 0, f"zeroize 之后 rk[{i}] 还有残留"

    await load_key(dut, SM4_KEY)
    ct, _ = await run_block(dut, SM4_PT)
    assert ct == SM4_CT, "重新装载后结果不对"

    dut._log.info("拍数恒为 32+33 与数据无关；zeroize 把 32 个轮密钥一拍清空")
