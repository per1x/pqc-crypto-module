"""cocotb：AES-128 / AES-256 分组核

三层，一层比一层往外钉：

  ① **S 盒逐值**：256 个输入全过一遍正向表与逆向表，与 sym_oracle.py 里
     **从 GF(2^8) 求逆 + 仿射变换算出来的**表比对。RTL 里是表、模型里是代数，
     所以"两边抄错同一格"这件事不可能发生。顺带验 S(S⁻¹(x)) == x。
  ② **FIPS 197 附录 C**：AES-128 与 AES-256 的官方向量，加解密两个方向。
  ③ **随机往返**：随机密钥、随机明文，加密再解密必须回到原文；
     并与 Python 参考实现逐字节比对。这一层是为了盖住"只有官方那一组恰好对"。

另外单验拍数与数据无关（常量时间那一条在 AES 上的形态）与 zeroize。
"""
import random
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from sym_oracle import (  # noqa: E402
    AES128_CT, AES128_KEY, AES256_CT, AES256_KEY, AES_PT, INV_SBOX, SBOX,
    aes_decrypt_block, aes_encrypt_block,
)


async def reset(dut):
    dut.rst_n.value = 0
    dut.key_start.value = 0
    dut.key_in.value = 0
    dut.key_256.value = 0
    dut.blk_start.value = 0
    dut.decrypt.value = 0
    dut.block_in.value = 0
    dut.zeroize.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_key(dut, key: bytes):
    """返回密钥扩展用了多少拍"""
    assert len(key) in (16, 32)
    dut.key_256.value = 1 if len(key) == 32 else 0
    dut.key_in.value = int.from_bytes(key + b"\x00" * (32 - len(key)), "big")
    dut.key_start.value = 1
    await RisingEdge(dut.clk)
    dut.key_start.value = 0
    for t in range(200):
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        if int(dut.key_ready.value):
            return t + 1
    raise AssertionError("密钥扩展没完成")


async def run_block(dut, blk: bytes, decrypt: bool):
    """返回 (输出分组, 拍数)"""
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
    """S 盒 256 个输入逐值比对：RTL 的表 vs 模型的代数定义"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # S 盒是纯组合的，直接从 aes_core 内部的实例上取
    fwd = dut.g_sbox[0].u_s
    inv = dut.g_sbox[0].u_is
    for x in range(256):
        fwd.x.value = x
        inv.x.value = x
        await Timer(1, unit="ns")
        got_f = int(fwd.y.value)
        got_i = int(inv.y.value)
        assert got_f == SBOX[x], f"S[{x:02x}] = {got_f:02x}，应当是 {SBOX[x]:02x}"
        assert got_i == INV_SBOX[x], \
            f"S⁻¹[{x:02x}] = {got_i:02x}，应当是 {INV_SBOX[x]:02x}"

    # 两张表互逆 —— 只比对"各自与模型一致"的话，模型自己错了还是过
    for x in range(256):
        assert INV_SBOX[SBOX[x]] == x, f"S⁻¹(S({x:02x})) 不是 {x:02x}"

    dut._log.info("AES S 盒：256 个输入正向/逆向逐值与代数定义一致，且互逆")


@cocotb.test()
async def test_fips197_vectors(dut):
    """FIPS 197 附录 C.1 / C.3：官方向量，加解密两个方向"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for name, key, ct, kcyc, bcyc in (
            ("AES-128", AES128_KEY, AES128_CT, 41, 11),
            ("AES-256", AES256_KEY, AES256_CT, 53, 15)):
        n = await load_key(dut, key)
        got, b = await run_block(dut, AES_PT, decrypt=False)
        assert got == ct, f"{name} 加密：{got.hex()}，应当是 {ct.hex()}"

        back, _ = await run_block(dut, ct, decrypt=True)
        assert back == AES_PT, f"{name} 解密：{back.hex()}，应当是 {AES_PT.hex()}"

        dut._log.info(f"{name}：FIPS 197 向量加解密都对（密钥扩展 {n} 拍、分组 {b} 拍）")
        assert n == kcyc, f"{name} 密钥扩展 {n} 拍，预期 {kcyc}"
        assert b == bcyc, f"{name} 分组 {b} 拍，预期 {bcyc}"


@cocotb.test()
async def test_random_roundtrip(dut):
    """随机密钥与明文：与 Python 参考实现逐字节一致，且加解密往返回到原文

    只测官方那一组向量是不够的 —— 一个把 ShiftRows 写成"恰好在那组输入上
    等价"的实现照样能过。随机往返把这类巧合盖掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(20260812)
    for i in range(8):
        klen = 16 if i % 2 == 0 else 32
        key = bytes(rng.randrange(256) for _ in range(klen))
        pt = bytes(rng.randrange(256) for _ in range(16))

        await load_key(dut, key)
        ct, _ = await run_block(dut, pt, decrypt=False)
        want = aes_encrypt_block(key, pt)
        assert ct == want, (f"AES-{klen*8} 加密不一致\n  key={key.hex()}\n"
                            f"  pt ={pt.hex()}\n  得 {ct.hex()}\n  期 {want.hex()}")

        back, _ = await run_block(dut, ct, decrypt=True)
        assert back == pt, f"AES-{klen*8} 往返回不到原文：{back.hex()} != {pt.hex()}"
        assert aes_decrypt_block(key, ct) == pt, "模型自己的往返就不对（自检）"

    dut._log.info("随机 8 组（128/256 各半）：与参考实现逐字节一致，往返回到原文")


@cocotb.test()
async def test_cycles_data_independent(dut):
    """拍数只由密钥长度决定，与密钥和明文的取值无关

    硬件查表本来就没有 cache 时序问题，但"拍数与数据无关"这一条要真的量一遍：
    数据通路上哪怕有一处写成了"某个值就多等一拍"，这里就会露出来。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rng = random.Random(7)
    for klen, kexp, bexp in ((16, 41, 11), (32, 53, 15)):
        seen_k, seen_b = set(), set()
        for _ in range(6):
            key = bytes(rng.randrange(256) for _ in range(klen))
            pt = bytes(rng.randrange(256) for _ in range(16))
            seen_k.add(await load_key(dut, key))
            _, b = await run_block(dut, pt, decrypt=False)
            seen_b.add(b)
            _, b = await run_block(dut, pt, decrypt=True)
            seen_b.add(b)
        assert seen_k == {kexp}, f"AES-{klen*8} 密钥扩展拍数随数据变了：{seen_k}"
        assert seen_b == {bexp}, f"AES-{klen*8} 分组拍数随数据变了：{seen_b}"

    dut._log.info("拍数：AES-128 = 41+11、AES-256 = 53+15，与密钥/明文取值无关")


@cocotb.test()
async def test_zeroize(dut):
    """zeroize 一拍清掉轮密钥：清完之后必须重新装密钥才能干活"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await load_key(dut, AES128_KEY)
    ct, _ = await run_block(dut, AES_PT, decrypt=False)
    assert ct == AES128_CT

    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    dut.zeroize.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    assert int(dut.key_ready.value) == 0, "zeroize 之后 key_ready 该掉下来"
    assert int(dut.block_out.value) == 0, "zeroize 之后输出寄存器该清零"

    # 没有密钥时 blk_start 不该做任何事
    dut.block_in.value = int.from_bytes(AES_PT, "big")
    dut.blk_start.value = 1
    await RisingEdge(dut.clk)
    dut.blk_start.value = 0
    for _ in range(30):
        await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    assert int(dut.blk_done.value) == 0, "没装密钥却处理了一个分组"

    # 重新装载后照常工作
    await load_key(dut, AES128_KEY)
    ct, _ = await run_block(dut, AES_PT, decrypt=False)
    assert ct == AES128_CT, "重新装载后结果不对"

    dut._log.info("zeroize：轮密钥与输出一拍清空，清完不装密钥就不干活")
