"""cocotb：密钥仓 → 对称核的整条链（S6 的落点）

这个文件测的是一句话能不能成立：

    **软件能让算法核用某把密钥，却没有任何一条路能读到那把密钥。**

三条用例，第一条是核心：

  ① `test_key_never_on_either_bus`：往密钥仓装一把 AES-128 的官方密钥，
     然后让 sym_axi 用那个槽加密 FIPS 197 的明文。密文必须与官方向量一致
     （说明密钥确实到了核里），同时把**两个从机的整个地址窗口逐字扫一遍**，
     断言密钥的 8 个字一个都不出现。
  ② 三个算法各跑一遍官方向量（AES-128/256、SM4 走密钥仓，SM3 无密钥）。
  ③ tamper：一根线同时打掉密钥仓与三个核，之后两条总线都 fail-closed。

另有"空槽装不进密钥"这一条 —— 否则一把全零密钥会被当成装好了。
"""
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from sym_oracle import (  # noqa: E402
    AES128_CT, AES128_KEY, AES256_CT, AES256_KEY, AES_PT, SM3_ABC,
    SM4_CT, SM4_KEY, SM4_PT,
)

# ---- 密钥仓 ----
V_CTRL, V_SLOT_SEL, V_KEY_IN, V_SLOT_CTRL = 0x04, 0x0C, 0x10, 0x14
V_VALID_MAP = 0x1C
SC_BEGIN, SC_COMMIT = 1 << 0, 1 << 1

# ---- 对称核 ----
S_CTRL, S_STATUS, S_ALG, S_SLOT, S_CMD, S_HASHIN = 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18
S_DIN0, S_DOUT0, S_DIGEST0 = 0x20, 0x30, 0x40
CMD_LOADKEY, CMD_BLOCK, CMD_HSTART, CMD_HFINAL = 1 << 0, 1 << 1, 1 << 2, 1 << 3
ST_BUSY, ST_DONE, ST_KEYREADY, ST_KVVALID = 1 << 0, 1 << 1, 1 << 2, 1 << 3

ALG_AES128, ALG_AES256, ALG_SM4, ALG_SM3 = 0, 1, 2, 3

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_DECERR = 0, 3


class Bus:
    """两个从机的端口名只差前缀，用一个小壳子把 BFM 复用起来"""

    def __init__(self, dut, pfx):
        self.dut = dut
        self.p = pfx

    def sig(self, name):
        return getattr(self.dut, f"{self.p}_{name}")


async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
    for p in ("vault", "sym"):
        getattr(dut, f"{p}_awaddr").value = 0
        getattr(dut, f"{p}_awprot").value = 0
        getattr(dut, f"{p}_awvalid").value = 0
        getattr(dut, f"{p}_wdata").value = 0
        getattr(dut, f"{p}_wstrb").value = 0xF
        getattr(dut, f"{p}_wvalid").value = 0
        getattr(dut, f"{p}_bready").value = 1
        getattr(dut, f"{p}_araddr").value = 0
        getattr(dut, f"{p}_arprot").value = 0
        getattr(dut, f"{p}_arvalid").value = 0
        getattr(dut, f"{p}_rready").value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def rd(bus, addr, prot=PROT_SECURE):
    bus.sig("araddr").value = addr
    bus.sig("arprot").value = prot
    bus.sig("arvalid").value = 1
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(bus.sig("arready").value):
            await RisingEdge(bus.dut.clk)
            break
        await RisingEdge(bus.dut.clk)
    else:
        raise AssertionError(f"{bus.p} 读 0x{addr:02x}：arready 一直不来")
    bus.sig("arvalid").value = 0
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(bus.sig("rvalid").value):
            d = int(bus.sig("rdata").value)
            r = int(bus.sig("rresp").value)
            await RisingEdge(bus.dut.clk)
            return d, r
        await RisingEdge(bus.dut.clk)
    raise AssertionError(f"{bus.p} 读 0x{addr:02x}：rvalid 一直不来")


async def wr(bus, addr, data, prot=PROT_SECURE):
    bus.sig("awaddr").value = addr
    bus.sig("awprot").value = prot
    bus.sig("awvalid").value = 1
    bus.sig("wdata").value = data
    bus.sig("wstrb").value = 0xF
    bus.sig("wvalid").value = 1
    aw = w = False
    for _ in range(64):
        await Timer(1, unit="ns")
        ta = int(bus.sig("awready").value) and not aw
        tw = int(bus.sig("wready").value) and not w
        await RisingEdge(bus.dut.clk)
        if ta:
            aw = True
            bus.sig("awvalid").value = 0
        if tw:
            w = True
            bus.sig("wvalid").value = 0
        if aw and w:
            break
    else:
        raise AssertionError(f"{bus.p} 写 0x{addr:02x}：没握上手")
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(bus.sig("bvalid").value):
            r = int(bus.sig("bresp").value)
            await RisingEdge(bus.dut.clk)
            return r
        await RisingEdge(bus.dut.clk)
    raise AssertionError(f"{bus.p} 写 0x{addr:02x}：bvalid 一直不来")


def key_words(key: bytes):
    """密钥按 32 位字切开，最高位在前；不足 32 字节补零"""
    k = key + b"\x00" * (32 - len(key))
    return [int.from_bytes(k[4 * i:4 * i + 4], "big") for i in range(8)]


async def vault_load(vault, slot, key: bytes):
    assert await wr(vault, V_SLOT_SEL, slot) == RESP_OKAY
    assert await wr(vault, V_SLOT_CTRL, SC_BEGIN) == RESP_OKAY
    for w_ in key_words(key):
        assert await wr(vault, V_KEY_IN, w_) == RESP_OKAY
    assert await wr(vault, V_SLOT_CTRL, SC_COMMIT) == RESP_OKAY
    vm, _ = await rd(vault, V_VALID_MAP)
    assert vm & (1 << slot), f"槽 {slot} 没装上（VALID_MAP=0x{vm:02x}）"


async def sym_wait_done(sym, limit=400):
    for _ in range(limit):
        st, _ = await rd(sym, S_STATUS)
        if st & ST_DONE:
            return
    raise AssertionError("对称核一直没完成")


async def sym_block(sym, alg, slot, blk: bytes, decrypt=False):
    assert await wr(sym, S_ALG, alg | (4 if decrypt else 0)) == RESP_OKAY
    assert await wr(sym, S_SLOT, slot) == RESP_OKAY
    st, _ = await rd(sym, S_STATUS)
    assert st & ST_KVVALID, "sym_axi 看不到密钥仓说这个槽有效"
    assert await wr(sym, S_CMD, CMD_LOADKEY) == RESP_OKAY
    for _ in range(200):
        st, _ = await rd(sym, S_STATUS)
        if st & ST_KEYREADY:
            break
    else:
        raise AssertionError("密钥没装好")

    for i in range(4):
        word = int.from_bytes(blk[4 * i:4 * i + 4], "big")
        assert await wr(sym, S_DIN0 + 4 * i, word) == RESP_OKAY
    assert await wr(sym, S_CMD, CMD_BLOCK) == RESP_OKAY
    await sym_wait_done(sym)

    out = b""
    for i in range(4):
        d, _ = await rd(sym, S_DOUT0 + 4 * i)
        out += d.to_bytes(4, "big")
    return out


@cocotb.test()
async def test_key_never_on_either_bus(dut):
    """**核心**：密钥能用，但两条总线上都读不出来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    vault, sym = Bus(dut, "vault"), Bus(dut, "sym")

    await vault_load(vault, 3, AES128_KEY)
    ct = await sym_block(sym, ALG_AES128, 3, AES_PT)
    assert ct == AES128_CT, (
        f"密文 {ct.hex()} 与 FIPS 197 不符 —— 密钥没有正确地从仓走到核里")

    # 两个从机的整个地址空间逐字扫一遍
    words = set(key_words(AES128_KEY)[:4])          # AES-128 只有前 4 个字有内容
    leaked = []
    for bus in (vault, sym):
        for addr in range(0, 256, 4):
            d, r = await rd(bus, addr)
            if r == RESP_OKAY and d in words and d != 0:
                leaked.append(f"{bus.p}:0x{addr:02x}→0x{d:08x}")
    assert not leaked, "密钥字出现在总线读回值里：" + ", ".join(leaked)

    dut._log.info("AES-128 用槽 3 的密钥算出了 FIPS 197 的密文，"
                  "而两条总线 512 字节地址空间里一个密钥字都读不到")


@cocotb.test()
async def test_three_algorithms(dut):
    """三个算法各跑一遍官方向量：AES-128/256 与 SM4 走密钥仓，SM3 无密钥"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    vault, sym = Bus(dut, "vault"), Bus(dut, "sym")

    await vault_load(vault, 0, AES128_KEY)
    assert await sym_block(sym, ALG_AES128, 0, AES_PT) == AES128_CT, "AES-128"
    assert await sym_block(sym, ALG_AES128, 0, AES128_CT, decrypt=True) == AES_PT, \
        "AES-128 解密"

    await vault_load(vault, 1, AES256_KEY)
    assert await sym_block(sym, ALG_AES256, 1, AES_PT) == AES256_CT, "AES-256"

    await vault_load(vault, 2, SM4_KEY)
    assert await sym_block(sym, ALG_SM4, 2, SM4_PT) == SM4_CT, "SM4"
    assert await sym_block(sym, ALG_SM4, 2, SM4_CT, decrypt=True) == SM4_PT, \
        "SM4 解密"

    # SM3：不需要密钥，字节一个一个写进 HASH_IN
    assert await wr(sym, S_ALG, ALG_SM3) == RESP_OKAY
    assert await wr(sym, S_CMD, CMD_HSTART) == RESP_OKAY
    for ch in b"abc":
        assert await wr(sym, S_HASHIN, ch) == RESP_OKAY
    assert await wr(sym, S_CMD, CMD_HFINAL) == RESP_OKAY
    await sym_wait_done(sym)
    dig = b""
    for i in range(8):
        d, _ = await rd(sym, S_DIGEST0 + 4 * i)
        dig += d.to_bytes(4, "big")
    assert dig == SM3_ABC, f'SM3("abc") = {dig.hex()}'

    dut._log.info("AES-128/256（含解密）、SM4（含解密）、SM3 —— 四条官方向量全对")


@cocotb.test()
async def test_empty_slot_refused(dut):
    """空槽装不进密钥 —— 否则一把全零密钥会被当成装好了"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    vault, sym = Bus(dut, "vault"), Bus(dut, "sym")

    assert await wr(sym, S_ALG, ALG_AES128) == RESP_OKAY
    assert await wr(sym, S_SLOT, 5) == RESP_OKAY          # 槽 5 没装过
    st, _ = await rd(sym, S_STATUS)
    assert not (st & ST_KVVALID), "空槽却报告 KV_VALID"

    assert await wr(sym, S_CMD, CMD_LOADKEY) == RESP_OKAY
    for _ in range(80):
        st, _ = await rd(sym, S_STATUS)
        assert not (st & ST_KEYREADY), "空槽居然把密钥装上了"

    # 装上之后就正常
    await vault_load(vault, 5, AES128_KEY)
    assert await sym_block(sym, ALG_AES128, 5, AES_PT) == AES128_CT

    dut._log.info("空槽的 LOAD_KEY 不生效；装上之后照常")


@cocotb.test()
async def test_tamper_takes_down_both(dut):
    """tamper 一根线同时打掉密钥仓与三个算法核，两条总线都 fail-closed"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    vault, sym = Bus(dut, "vault"), Bus(dut, "sym")

    await vault_load(vault, 4, AES128_KEY)
    assert await sym_block(sym, ALG_AES128, 4, AES_PT) == AES128_CT

    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0
    for _ in range(3):
        await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    assert int(dut.vault_tampered.value) == 1, "tamper 没被锁存"
    _, r = await rd(vault, V_VALID_MAP)
    assert r == RESP_DECERR, "tamper 之后密钥仓还能读"
    _, r = await rd(sym, S_STATUS)
    assert r == RESP_DECERR, "tamper 之后对称核还能读"
    assert await wr(sym, S_CMD, CMD_BLOCK) == RESP_DECERR, "tamper 之后还能下命令"

    await reset(dut)
    assert int(dut.vault_tampered.value) == 0, "复位后该恢复"
    await vault_load(vault, 4, AES128_KEY)
    assert await sym_block(sym, ALG_AES128, 4, AES_PT) == AES128_CT, "复位后不工作"

    dut._log.info("tamper：两条总线一起 fail-closed，密钥与轮密钥全清；rst_n 后恢复")


@cocotb.test()
async def test_nonsecure_refused(dut):
    """non-secure 在两条总线上都被拦下"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    vault, sym = Bus(dut, "vault"), Bus(dut, "sym")

    await vault_load(vault, 6, AES128_KEY)
    assert await wr(vault, V_CTRL, 1, PROT_NONSEC) == RESP_DECERR
    assert await wr(sym, S_CTRL, 1, PROT_NONSEC) == RESP_DECERR
    d, r = await rd(sym, S_STATUS, PROT_NONSEC)
    assert r == RESP_DECERR and d == 0

    # 被拦下的那几笔没有副作用：密钥还在，还能加密
    assert await sym_block(sym, ALG_AES128, 6, AES_PT) == AES128_CT

    dut._log.info("non-secure：两条总线都 DECERR，且没有副作用")
