"""cocotb：PL 内密钥仓 + AXI 防火墙（S5 的密码边界）

这个文件测的不是"功能对不对"，而是**一条边界成立不成立**。功能只有几行：
写 8 个字、commit、用 use 口取出来。值钱的是下面这些：

一、**密钥没有任何一条通往总线的路径**。
    不是"读回来被门控成 0"，而是根本没有导线。验法是**逐字扫描整个
    256 字节地址空间**，断言写进去的密钥字一个都不出现。这条用例读的是
    硬件的实际行为，不是 RTL 的注释。

二、**tamper 一拍全清、且只进不出**。
    拉一拍 tamper：所有槽立刻失效、use_key 归零、之后一切装载被拒，
    而且软件**没有任何一条路**能把它清回来（只有 rst_n）。

三、**防火墙拦下的事务根本不到下游**。
    non-secure 的写不仅返回 DECERR，而且**不产生任何副作用** ——
    用"non-secure 反复写 CTRL.ZEROIZE，槽位必须原封不动"来验：
    如果它到了下游只是被忽略，稍有疏漏就是一次远程擦库。

四、**半把密钥不算密钥**。写 7 个字就 commit，槽位必须仍然无效。

五、地址窗口越界与违规留痕。
"""
import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

VERSION, CTRL, STATUS, SLOT_SEL = 0x00, 0x04, 0x08, 0x0C
KEY_IN, SLOT_CTRL, SLOT_STAT, VALID_MAP = 0x10, 0x14, 0x18, 0x1C
LOCK_MAP, ZERO_CNT, VIOL_CNT, VIOL_INFO, PARAM0 = 0x20, 0x24, 0x28, 0x2C, 0x30

CTRL_ZEROIZE = 1 << 0
SC_BEGIN, SC_COMMIT, SC_LOCK, SC_ERASE = 1 << 0, 1 << 1, 1 << 2, 1 << 3
ST_READY, ST_TAMPER, ST_DENY = 1 << 0, 1 << 1, 1 << 2

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_DECERR = 0, 3
# ============================================================================
# 【"被拒"在总线上长什么样：RAZ/WI，不是 DECERR】
# ============================================================================
# 防火墙拒绝一笔访问时**读回 0、写丢弃，响应仍是 OKAY** —— 不产生总线错误。
# 改动的理由在 hardware/rtl/bus/axi4lite_firewall.v 的文件头：DECERR 的
# posted 写会以 SError 回来，aarch64 的内核只能 panic，于是"写错一个地址"
# 的代价是一次断电。
#
# 用例里一律写 RESP_REFUSED，不写具体码值 —— **"被拒长什么样"是 RTL 的策略，
# 不该抄进每一条断言**。抄进去的后果这次已经见过了：策略一改，几十条断言
# 全得跟着动，而它们本来一条都不该动（它们要证的是"被拒了"，不是"回了 3"）。
#
# ⚠️ 读的断言必须**同时**查 rdata == 0。RAZ/WI 之后响应码不再区分放行与拒绝，
#    数据才是。只查 resp 的读用例现在等于什么都没查。
RESP_REFUSED = RESP_OKAY

WORDS = 8


async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
    dut.use_sel.value = 0
    dut.s_axi_awaddr.value = 0
    dut.s_axi_awprot.value = 0
    dut.s_axi_awvalid.value = 0
    dut.s_axi_wdata.value = 0
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 0
    dut.s_axi_bready.value = 1
    dut.s_axi_araddr.value = 0
    dut.s_axi_arprot.value = 0
    dut.s_axi_arvalid.value = 0
    dut.s_axi_rready.value = 1
    for _ in range(5):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def axi_read(dut, addr, prot=PROT_SECURE):
    dut.s_axi_araddr.value = addr
    dut.s_axi_arprot.value = prot
    dut.s_axi_arvalid.value = 1
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_arready.value):
            await RisingEdge(dut.clk)
            break
        await RisingEdge(dut.clk)
    else:
        raise AssertionError(f"读 0x{addr:02x}：arready 一直不来")
    dut.s_axi_arvalid.value = 0

    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_rvalid.value):
            data = int(dut.s_axi_rdata.value)
            resp = int(dut.s_axi_rresp.value)
            await RisingEdge(dut.clk)
            return data, resp
        await RisingEdge(dut.clk)
    raise AssertionError(f"读 0x{addr:02x}：rvalid 一直不来")


async def axi_write(dut, addr, data, prot=PROT_SECURE):
    dut.s_axi_awaddr.value = addr
    dut.s_axi_awprot.value = prot
    dut.s_axi_awvalid.value = 1
    dut.s_axi_wdata.value = data
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 1

    aw_done = w_done = False
    for _ in range(64):
        await Timer(1, unit="ns")
        take_aw = int(dut.s_axi_awready.value) and not aw_done
        take_w = int(dut.s_axi_wready.value) and not w_done
        await RisingEdge(dut.clk)
        if take_aw:
            aw_done = True
            dut.s_axi_awvalid.value = 0
        if take_w:
            w_done = True
            dut.s_axi_wvalid.value = 0
        if aw_done and w_done:
            break
    else:
        raise AssertionError(f"写 0x{addr:02x}：地址/数据通道没握上手")

    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_bvalid.value):
            resp = int(dut.s_axi_bresp.value)
            await RisingEdge(dut.clk)
            return resp
        await RisingEdge(dut.clk)
    raise AssertionError(f"写 0x{addr:02x}：bvalid 一直不来")


def key_words(seed):
    """一把好认的密钥：每个字都不同，且不等于 0 或全 1"""
    return [(0xA5000000 | (seed << 16) | (i * 0x1111) | (i + 1)) & 0xFFFFFFFF
            for i in range(WORDS)]


async def load_key(dut, slot, words, commit=True):
    assert await axi_write(dut, SLOT_SEL, slot) == RESP_OKAY
    assert await axi_write(dut, SLOT_CTRL, SC_BEGIN) == RESP_OKAY
    for w in words:
        assert await axi_write(dut, KEY_IN, w) == RESP_OKAY
    if commit:
        assert await axi_write(dut, SLOT_CTRL, SC_COMMIT) == RESP_OKAY


async def read_use_key(dut, slot):
    dut.use_sel.value = slot
    await Timer(1, unit="ns")
    return int(dut.use_key.value), int(dut.use_valid.value)


def as_words(key_int):
    # 先写进去的字在最高位，见 key_vault.v 里 use_mux 的注释
    return [(key_int >> (32 * (WORDS - 1 - i))) & 0xFFFFFFFF for i in range(WORDS)]


@cocotb.test()
async def test_load_and_use(dut):
    """装载 → commit → use 口取出整把密钥"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    ver, resp = await axi_read(dut, VERSION)
    assert resp == RESP_OKAY and ver == 0x0001_0000, f"VERSION = 0x{ver:08x}"
    p0, _ = await axi_read(dut, PARAM0)
    assert p0 & 0xFF == 8 and (p0 >> 8) & 0xFF == WORDS, f"PARAM0 = 0x{p0:08x}"

    words = key_words(1)
    await load_key(dut, 3, words)

    vm, _ = await axi_read(dut, VALID_MAP)
    assert vm == (1 << 3), f"VALID_MAP = 0x{vm:02x}，只有槽 3 该有效"

    key, valid = await read_use_key(dut, 3)
    assert valid == 1, "commit 之后 use_valid 该拉高"
    assert as_words(key) == words, "use 口取出来的密钥与写进去的不一致"

    key, valid = await read_use_key(dut, 4)
    assert valid == 0, "没装过的槽 use_valid 该是 0"
    assert key == 0, "没装过的槽 use_key 该是 0"

    dut._log.info("装载 → commit → use 口：256 位密钥逐字一致")


@cocotb.test()
async def test_key_never_readable(dut):
    """**核心不变量**：整个地址空间里读不出密钥的任何一个字

    逐字扫描 256 字节地址空间（包括未定义的偏移），断言写进去的 8 个密钥字
    一个都不出现。这条不是靠读代码保证的 —— 只要有人不小心把 keys 接进读
    多路选择器，它立刻就挂。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 八个槽全装上，每槽的字都不一样，一次扫描覆盖全部
    allwords = set()
    for slot in range(8):
        words = key_words(slot + 1)
        allwords.update(words)
        await load_key(dut, slot, words)

    seen = {}
    for addr in range(0, 256, 4):
        data, resp = await axi_read(dut, addr)
        if resp == RESP_OKAY:
            seen[addr] = data

    leaked = {a: d for a, d in seen.items() if d in allwords}
    assert not leaked, (
        "密钥字出现在总线读回值里："
        + ", ".join(f"0x{a:02x}→0x{d:08x}" for a, d in leaked.items()))

    # 顺便确认 KEY_IN 这个只写口读回 0（而不是"恰好不等于任何密钥字"）
    kd, kr = await axi_read(dut, KEY_IN)
    assert kr == RESP_OKAY and kd == 0, f"读 KEY_IN 得到 0x{kd:08x}，应当是 0"

    # 而 use 口确实还拿得到 —— 否则一个"把密钥全擦了"的实现也能过上面那条
    key, valid = await read_use_key(dut, 5)
    assert valid == 1 and as_words(key) == key_words(6), "use 口该照常取到密钥"

    dut._log.info(f"扫了 {len(seen)} 个可读地址，8 槽 × 8 字密钥一个都没漏出来")


@cocotb.test()
async def test_tamper_wipes_and_latches(dut):
    """tamper：一拍全清、之后拒绝一切装载、软件清不掉"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    words = key_words(2)
    await load_key(dut, 1, words)
    key, valid = await read_use_key(dut, 1)
    assert valid == 1 and as_words(key) == words

    # 拉一拍 tamper
    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")

    key, valid = await read_use_key(dut, 1)
    assert valid == 0, "tamper 之后 use_valid 该掉下来"
    assert key == 0, "tamper 之后 use_key 该是 0"
    assert int(dut.vault_tampered.value) == 1, "tamper 该被锁存"

    # tamper 之后防火墙一律拒绝 —— 连 STATUS 都读不了，这是 fail-closed
    data, resp = await axi_read(dut, STATUS)
    assert resp == RESP_REFUSED and data == 0, \
        f"tamper 之后读也该被防火墙拦下，却拿到 0x{data:08x}"

    # 软件想装新密钥：被拒
    assert await axi_write(dut, SLOT_SEL, 1) == RESP_REFUSED
    assert await axi_write(dut, KEY_IN, 0xDEADBEEF) == RESP_REFUSED
    key, valid = await read_use_key(dut, 1)
    assert valid == 0 and key == 0, "tamper 之后仍然装进去了"

    # 软件想清掉 tamper：没有这个路径。CTRL.ZEROIZE 也被拦下。
    assert await axi_write(dut, CTRL, CTRL_ZEROIZE) == RESP_REFUSED
    assert int(dut.vault_tampered.value) == 1, "tamper 被软件清掉了"

    # 只有复位能恢复
    await reset(dut)
    assert int(dut.vault_tampered.value) == 0, "复位后 tamper 该清"
    st, resp = await axi_read(dut, STATUS)
    assert resp == RESP_OKAY and (st & ST_READY), "复位后该恢复可用"

    dut._log.info("tamper：一拍清空、fail-closed、只有 rst_n 能恢复")


@cocotb.test()
async def test_nonsecure_blocked_without_side_effects(dut):
    """non-secure 访问：DECERR，而且**不产生任何副作用**

    只验"返回 DECERR"是不够的。真正危险的是"事务到了下游只是效果被忽略"——
    那种实现里，只要哪一处副作用忘了判门控信号，就是一次远程擦库。
    这里直接用最狠的那一个来验：non-secure 反复写 CTRL.ZEROIZE。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    words = key_words(9)
    await load_key(dut, 2, words)
    zc_before, _ = await axi_read(dut, ZERO_CNT)

    for _ in range(4):
        assert await axi_write(dut, CTRL, CTRL_ZEROIZE, PROT_NONSEC) == RESP_REFUSED
        assert await axi_write(dut, SLOT_CTRL, SC_ERASE, PROT_NONSEC) == RESP_REFUSED
        assert await axi_write(dut, SLOT_SEL, 7, PROT_NONSEC) == RESP_REFUSED

    key, valid = await read_use_key(dut, 2)
    assert valid == 1 and as_words(key) == words, "non-secure 的写把密钥擦了"

    zc_after, _ = await axi_read(dut, ZERO_CNT)
    assert zc_after == zc_before, f"擦除计数从 {zc_before} 变成了 {zc_after}"

    sel, _ = await axi_read(dut, SLOT_SEL)
    assert sel == 2, f"SLOT_SEL 被 non-secure 的写改成了 {sel}"

    # non-secure 的读同样 DECERR 且返回 0
    data, resp = await axi_read(dut, VALID_MAP, PROT_NONSEC)
    assert resp == RESP_REFUSED and data == 0, f"non-secure 读到了 0x{data:08x}"

    # 违规留痕：12 次写 + 1 次读
    vc, _ = await axi_read(dut, VIOL_CNT)
    assert vc & 0xFFFF == 12, f"写违规计数 {vc & 0xFFFF}，应当是 12"
    assert (vc >> 16) == 1, f"读违规计数 {vc >> 16}，应当是 1"

    vi, _ = await axi_read(dut, VIOL_INFO)
    assert vi & (1 << 10), "第一次违规没被记下来"
    assert vi & 0xFF == CTRL, f"首次违规地址记成了 0x{vi & 0xFF:02x}"
    assert vi & (1 << 8), "首次违规应当是一次写"
    assert vi & (1 << 9), "首次违规的 NS 位应当是 1"

    dut._log.info("non-secure：12 写 1 读全部 DECERR，密钥与计数原封不动，首次违规留痕")


@cocotb.test()
async def test_half_key_and_lock(dut):
    """半把密钥不算密钥；锁定的槽写不进也擦不掉"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 只写 7 个字就 commit
    words = key_words(4)
    await load_key(dut, 0, words[:7], commit=False)
    assert await axi_write(dut, SLOT_CTRL, SC_COMMIT) == RESP_OKAY

    vm, _ = await axi_read(dut, VALID_MAP)
    assert vm == 0, f"写了 7 个字就 commit，槽却有效了（VALID_MAP=0x{vm:02x}）"
    ss, _ = await axi_read(dut, SLOT_STAT)
    assert (ss >> 4) & 0xF == 7, f"已写字数记成了 {(ss >> 4) & 0xF}"
    st, _ = await axi_read(dut, STATUS)
    assert st & ST_DENY, "被拒的 commit 没有置 DENY"

    # 补上第 8 个字再 commit
    assert await axi_write(dut, KEY_IN, words[7]) == RESP_OKAY
    assert await axi_write(dut, SLOT_CTRL, SC_COMMIT) == RESP_OKAY
    key, valid = await read_use_key(dut, 0)
    assert valid == 1 and as_words(key) == words

    # 锁定之后：写不进、擦不掉、begin 也不行
    assert await axi_write(dut, SLOT_CTRL, SC_LOCK) == RESP_OKAY
    lm, _ = await axi_read(dut, LOCK_MAP)
    assert lm == 1, f"LOCK_MAP = 0x{lm:02x}"

    assert await axi_write(dut, SLOT_CTRL, SC_BEGIN) == RESP_OKAY
    assert await axi_write(dut, KEY_IN, 0xDEADBEEF) == RESP_OKAY
    assert await axi_write(dut, SLOT_CTRL, SC_ERASE) == RESP_OKAY
    key, valid = await read_use_key(dut, 0)
    assert valid == 1 and as_words(key) == words, "锁定的槽被改动了"

    # 但全局擦除清得掉锁定的槽 —— 否则一把锁死的密钥就永远占着仓位
    assert await axi_write(dut, CTRL, CTRL_ZEROIZE) == RESP_OKAY
    key, valid = await read_use_key(dut, 0)
    assert valid == 0 and key == 0, "全局擦除没清掉锁定的槽"
    lm, _ = await axi_read(dut, LOCK_MAP)
    assert lm == 0, "全局擦除后锁应当一起清"
    zc, _ = await axi_read(dut, ZERO_CNT)
    assert zc == 1, f"擦除计数 = {zc}，应当是 1"

    dut._log.info("半把密钥不算数；锁定挡住写/擦/begin；全局擦除连锁一起清")


@cocotb.test()
async def test_address_window(dut):
    """地址窗口：越界一律 DECERR 并留痕"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 窗口是 0x00~0x3F（ADDR_MASK=0xC0）
    for addr in (0x00, 0x10, 0x3C):
        _, resp = await axi_read(dut, addr)
        assert resp == RESP_OKAY, f"窗口内地址 0x{addr:02x} 被拦了"

    vc0, _ = await axi_read(dut, VIOL_CNT)
    for addr in (0x40, 0x80, 0xC0, 0xFC):
        data, resp = await axi_read(dut, addr)
        assert resp == RESP_REFUSED, f"越界地址 0x{addr:02x} 没被拦"
        assert data == 0, f"越界地址 0x{addr:02x} 回了 0x{data:08x}，应当是 0"
        assert data == 0, f"越界读返回了 0x{data:08x}"
    vc1, _ = await axi_read(dut, VIOL_CNT)
    assert (vc1 >> 16) - (vc0 >> 16) == 4, "越界读没被计数"

    dut._log.info("地址窗口 0x00~0x3F：窗口内放行，越界四个地址全部 DECERR 并计数")
