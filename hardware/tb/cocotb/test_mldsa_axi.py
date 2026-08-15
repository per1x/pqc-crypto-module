"""cocotb：ML-DSA 共享引擎的 AXI4-Lite 从机（mldsa_axi）

⚠️⚠️ **这一套用的是行为级替身 engine（stub_mldsa_engine.v），不是真的 ML-DSA。**
所以它证明的是「软件视角下这块外设能不能用、私钥能不能出得来」，
**不证明任何算法正确性** —— 算法在 test_mldsa_keygen / _sign / _verify 里
对着 ACVP 官方向量逐字节验过，两件事不要混着说。

替身按固定延迟出一份**可以在 Python 里算出来**的假输出：把整个输入缓冲
逐字节吸收进 SHAKE256，挤出一个字节 h，输出全按 h 铺开（见 stub 的文件头）。
关键在于**用哈希而不是字节和**：哈希与顺序有关，于是"金库供的 sk 落在
engine 的哪个偏移""rnd/ctx/msg 有没有接在该接的位置"这些事都会体现在输出上。

输入字节流的排布（与 engine 那条线共用的契约）：
    KeyGen : ξ(32)
    Sign   : [sk，仅当 SK_FROM_SLOT=0] ‖ rnd(32) ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)
    Verify : pk ‖ sig ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)

覆盖：
  ① 三种 OP × 三个 PSET 的寄存器时序与长度；
  ② START 前的长度校验：喂不够就 PARAM_ERR|LEN_ERR 且**不启动**
     （走金库/不走金库 × ctx 空/非空 四种组合都覆盖）；
  ③ 金库：SK_TO_SLOT 时 sk 不出现在 OUT_DATA；SK_FROM_SLOT 时软件不送 sk
     也能签，且两条路签出来的字节完全相同；
  ④ 一次性闩锁：置上后强制走金库，且**没有任何写法能把它清掉**（反证）；
  ⑤ 非法 OP / PSET / SLOT / ctx_len 被拒；
  ⑥ 陈旧状态：被拒的 START 不留上一次的 DONE/OUT_LEN；CLEAR 清得干净；
  ⑦ 同一个 OP 连跑两次（残留 done 那个上板才暴露的坑）；
  ⑧ 防火墙：non-secure 被拦且无副作用；
  ⑨ ZEROIZE / tamper 真的把 64 KB 金库逐字节擦掉；
  ⑩ 排布相关的两条防呆：写 MODE 清写指针、运行途中改参数回 SLVERR。
"""
import hashlib

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

# ---- 寄存器映射（槽内偏移）----
VERSION, CTRL, MODE, STATUS = 0x00, 0x04, 0x08, 0x0C
IN_DATA, IN_PTR, OUT_DATA, OUT_PTR = 0x10, 0x14, 0x18, 0x1C
OUT_LEN, MSG_LEN, CTX_LEN, KEYSTAT, VIOL = 0x20, 0x24, 0x28, 0x2C, 0x30

C_START, C_CLEAR, C_ZEROIZE, C_SK_LOCK = 1 << 0, 1 << 1, 1 << 2, 1 << 4

ST_BUSY, ST_DONE, ST_VOK = 1 << 0, 1 << 1, 1 << 2
ST_PARAMERR, ST_LENERR = 1 << 3, 1 << 4
ST_TAMPER, ST_WIPING = 1 << 5, 1 << 6

OP_KEYGEN, OP_SIGN, OP_VERIFY = 0, 1, 2
M_SK_TO_SLOT, M_SK_FROM_SLOT = 1 << 4, 1 << 5

KS_LOCK = 1 << 8            # KEYSTAT：[7:0] 槽有效位，[8] 闩锁，[31:16] 每槽 pset

# FIPS 204 表 2
PK = {0: 1312, 1: 1952, 2: 2592}
SK = {0: 2560, 1: 4032, 2: 4896}
SIG = {0: 2420, 1: 3309, 2: 4627}
PSNAME = {0: "ML-DSA-44", 1: "ML-DSA-65", 2: "ML-DSA-87"}

RND0 = bytes(32)            # 确定性签名：rnd = 0³²（ACVP 的确定性条目就是它）

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_SLVERR = 0, 2
# 被拒长什么样是 RTL 的策略（RAZ/WI：读回 0、写丢弃、响应 OKAY），
# 用例里一律写 RESP_REFUSED，理由见 test_mlkem_axi.py 里那段。
RESP_REFUSED = RESP_OKAY


# ============================================================================
# 替身 engine 的模型（与 stub_mldsa_engine.v 一一对应）
# ============================================================================
def eng_input(op, *, xi=b"", rnd=b"", ctx=b"", msg=b"", sk=b"", pk=b"", sig=b""):
    """engine 输入缓冲里应当是什么 —— **engine 看到的永远是完整的一份**，
    sk 是软件送的还是金库供的，对它没有区别。"""
    if op == OP_KEYGEN:
        return xi
    if op == OP_SIGN:
        return sk + rnd + ctx + msg
    return pk + sig + ctx + msg


def stub_h(eng_in: bytes) -> int:
    return hashlib.shake_256(eng_in).digest(1)[0]


def stub_keygen(pset, xi):
    """替身的 KeyGen 输出：pk‖sk"""
    h = stub_h(eng_input(OP_KEYGEN, xi=xi))
    pk = bytes((h + i) & 0xFF for i in range(PK[pset]))
    sk = bytes(((h ^ 0xA5) + j) & 0xFF for j in range(SK[pset]))
    return pk, sk


def stub_sign(pset, sk, rnd, ctx, msg):
    h = stub_h(eng_input(OP_SIGN, sk=sk, rnd=rnd, ctx=ctx, msg=msg))
    return bytes((h + i) & 0xFF for i in range(SIG[pset]))


def stub_verify_ok(pk, sig, ctx, msg):
    return stub_h(eng_input(OP_VERIFY, pk=pk, sig=sig, ctx=ctx, msg=msg)) == 0


def make_verify_pass(pk, sig, ctx, msg):
    """调最后两个 sig 字节，凑出一份让替身判"通过"的输入

    替身的假判定是 h == 0，所以造一个通过的例子要搜一下。搜的是确定性的：
    同一组输入每次都得到同一个答案。
    """
    s = bytearray(sig)
    for v in range(4096):
        s[-1] = v & 0xFF
        s[-2] = (v >> 8) & 0xFF
        if stub_verify_ok(pk, bytes(s), ctx, msg):
            return bytes(s)
    raise AssertionError("没搜到能让替身判通过的 sig（4096 次都没中）")


def sign_payload(sk, rnd, ctx, msg, *, from_slot=False):
    """软件该往 IN_DATA 里灌什么 —— 走金库时不送 sk"""
    return (b"" if from_slot else sk) + rnd + ctx + msg


# ============================================================================
# AXI4-Lite 搬运（与 test_mlkem_axi.py 同一份写法）
# ============================================================================
async def reset(dut):
    dut.rst_n.value = 0
    dut.tamper.value = 0
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


async def rd(dut, addr, prot=PROT_SECURE):
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
            d = int(dut.s_axi_rdata.value)
            r = int(dut.s_axi_rresp.value)
            await RisingEdge(dut.clk)
            return d, r
        await RisingEdge(dut.clk)
    raise AssertionError(f"读 0x{addr:02x}：rvalid 一直不来")


async def wr(dut, addr, data, prot=PROT_SECURE):
    dut.s_axi_awaddr.value = addr
    dut.s_axi_awprot.value = prot
    dut.s_axi_awvalid.value = 1
    dut.s_axi_wdata.value = data
    dut.s_axi_wstrb.value = 0xF
    dut.s_axi_wvalid.value = 1
    aw = w = False
    for _ in range(64):
        await Timer(1, unit="ns")
        ta = int(dut.s_axi_awready.value) and not aw
        tw = int(dut.s_axi_wready.value) and not w
        await RisingEdge(dut.clk)
        if ta:
            aw = True
            dut.s_axi_awvalid.value = 0
        if tw:
            w = True
            dut.s_axi_wvalid.value = 0
        if aw and w:
            break
    else:
        raise AssertionError(f"写 0x{addr:02x}：没握上手")
    for _ in range(64):
        await Timer(1, unit="ns")
        if int(dut.s_axi_bvalid.value):
            r = int(dut.s_axi_bresp.value)
            await RisingEdge(dut.clk)
            return r
        await RisingEdge(dut.clk)
    raise AssertionError(f"写 0x{addr:02x}：bvalid 一直不来")


def mode_word(op, pset, *, to_slot=False, from_slot=False, slot=0):
    return (op | (pset << 2)
            | (M_SK_TO_SLOT if to_slot else 0)
            | (M_SK_FROM_SLOT if from_slot else 0)
            | (slot << 6))


async def fill(dut, payload: bytes):
    """把字节流灌进 IN_DATA（写指针自增）"""
    for b in payload:
        assert await wr(dut, IN_DATA, b) == RESP_OKAY


async def start_and_wait(dut, limit=40_000):
    """写 START 然后等 —— 返回 True=跑完，False=被拒（PARAM_ERR）"""
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            return True
        if st & ST_PARAMERR:
            return False
    raise AssertionError("BUSY 一直不落，也没报错")


async def out_bytes(dut, first, count):
    """从 OUT_DATA 取 count 个字节（先把读游标 seek 到 first）

    OUT_PTR 可写这一点在这里很值钱：几千字节的输出不必整个读回来，
    抽样几段就够 —— 读一个字节是两笔 AXI 事务，全读一遍是几万笔。
    """
    assert await wr(dut, OUT_PTR, first) == RESP_OKAY
    out = bytearray()
    for _ in range(count):
        d, _ = await rd(dut, OUT_DATA)
        out.append(d & 0xFF)
    return bytes(out)


async def run_op(dut, op, pset, payload, *, to_slot=False, from_slot=False,
                 slot=0, msg_len=0, ctx_len=0, limit=40_000):
    """一次完整调用：设参数 → 清 → 灌字节 → 启动 → 等完成。返回 OUT_LEN

    ⚠️ 顺序是 MODE 在最前 —— 写 MODE 会清 IN_PTR（排布依赖 MODE，见 RTL 文件头）。
    """
    assert await wr(dut, MODE, mode_word(op, pset, to_slot=to_slot,
                                         from_slot=from_slot, slot=slot)) == RESP_OKAY
    assert await wr(dut, MSG_LEN, msg_len) == RESP_OKAY
    assert await wr(dut, CTX_LEN, ctx_len) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, payload)
    p, _ = await rd(dut, IN_PTR)
    assert p == len(payload), f"IN_PTR = {p}，应当是 {len(payload)}"
    ok = await start_and_wait(dut, limit)
    if not ok:
        return None
    n, _ = await rd(dut, OUT_LEN)
    return n


# ============================================================================
# ① 三种 OP × 三个 PSET
# ============================================================================
@cocotb.test()
async def test_three_ops_three_psets(dut):
    """九个组合各跑一遍：长度、DONE、输出字节都对得上

    Sign 那一路**故意走金库**（先 KeyGen 存槽，再按槽签）：既省掉几千笔
    AXI 写，又顺带把"金库供的 sk 与软件送的 sk 在 engine 眼里是同一份排布"
    这条钉在每个参数集上。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    v, r = await rd(dut, VERSION)
    assert r == RESP_OKAY and v == 0x0001_0000, f"VERSION=0x{v:08x}"

    for pset in (0, 1, 2):
        name = PSNAME[pset]

        # ---- KeyGen（不存槽：ACVP 核对 sk 走的就是这条路）----
        xi = bytes((0x10 + pset + i) & 0xFF for i in range(32))
        pk_ref, sk_ref = stub_keygen(pset, xi)
        n = await run_op(dut, OP_KEYGEN, pset, xi)
        assert n == PK[pset] + SK[pset], \
            f"{name} KeyGen OUT_LEN={n}，应当是 {PK[pset]}+{SK[pset]}"
        assert await out_bytes(dut, 0, 8) == pk_ref[:8], f"{name} pk 头 8 字节不对"
        assert await out_bytes(dut, PK[pset], 8) == sk_ref[:8], \
            f"{name} sk 段头 8 字节不对（不存槽时 sk 本来就该出得来）"

        # ---- KeyGen 存槽 → Sign 用槽 ----
        n = await run_op(dut, OP_KEYGEN, pset, xi, to_slot=True, slot=pset)
        assert n == PK[pset], f"{name} 存槽时 OUT_LEN={n}，应当恰好是 pk 的 {PK[pset]}"

        ctx, msg = b"\xC1\xC2", b"hello-mldsa"
        sig_ref = stub_sign(pset, sk_ref, RND0, ctx, msg)
        n = await run_op(dut, OP_SIGN, pset,
                         sign_payload(sk_ref, RND0, ctx, msg, from_slot=True),
                         from_slot=True, slot=pset,
                         msg_len=len(msg), ctx_len=len(ctx))
        assert n == SIG[pset], f"{name} Sign OUT_LEN={n}，应当是 {SIG[pset]}"
        assert await out_bytes(dut, 0, 8) == sig_ref[:8], \
            f"{name} 按槽签出来的签名不对 —— 金库里的 sk 没有正确进到 engine"

        # ---- Verify ----
        n = await run_op(dut, OP_VERIFY, pset,
                         pk_ref + sig_ref + ctx + msg,
                         msg_len=len(msg), ctx_len=len(ctx))
        assert n == 0, f"{name} Verify 不该有输出字节，OUT_LEN={n}"
        st, _ = await rd(dut, STATUS)
        want = stub_verify_ok(pk_ref, sig_ref, ctx, msg)
        assert bool(st & ST_VOK) == want, \
            f"{name} verify_ok={bool(st & ST_VOK)}，替身模型说应当是 {want}"

        dut._log.info(f"{name}：KeyGen/Sign/Verify 三条都对上（Sign 走的是金库）")


@cocotb.test()
async def test_verify_ok_both_ways(dut):
    """verify_ok 两种结果都能确定性地造出来

    只验"通过"是不够的：一个把 verify_ok 恒接成 1 的实现同样能过。
    替身的假判定挂在整份输入的摘要上，Python 调两个字节就能翻转它。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    ctx, msg = b"", b"\x01\x02\x03\x04"
    pk = bytes((i * 7 + 1) & 0xFF for i in range(PK[pset]))
    sig0 = bytes((i * 3 + 5) & 0xFF for i in range(SIG[pset]))
    good = make_verify_pass(pk, sig0, ctx, msg)

    n = await run_op(dut, OP_VERIFY, pset, pk + good + ctx + msg,
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == 0
    st, _ = await rd(dut, STATUS)
    assert st & ST_VOK, "该判通过的这一份没有置 verify_ok"

    bad = bytearray(good)
    bad[0] ^= 0x01
    assert not stub_verify_ok(pk, bytes(bad), ctx, msg)
    await run_op(dut, OP_VERIFY, pset, pk + bytes(bad) + ctx + msg,
                 msg_len=len(msg), ctx_len=len(ctx))
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_VOK), \
        "改了一个字节还判通过 —— verify_ok 多半是接死的"

    dut._log.info("verify_ok 通过/不通过两种结果都由输入确定性地造出来了")


# ============================================================================
# ② START 前的长度校验
# ============================================================================
@cocotb.test()
async def test_underfill_refused_and_not_started(dut):
    """喂不够就 PARAM_ERR|LEN_ERR 且**不启动 engine**

    判据是四条一起：PARAM_ERR 置位、LEN_ERR 置位、BUSY 从未拉起、OUT_LEN 仍是 0。
    只看错误位是不够的 —— 先启动再报错同样能置位，而那时 engine 已经把
    输入缓冲里的**残留**当私钥算过一轮了（见 mldsa_axi.v 文件头那段）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    # KeyGen 要 32 字节 ξ，只给 31
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, pset)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, bytes(31))
    assert await wr(dut, CTRL, C_START) == RESP_OKAY

    busy_seen = False
    for _ in range(200):
        st, _ = await rd(dut, STATUS)
        if st & ST_BUSY:
            busy_seen = True
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, f"欠填没被拒（STATUS={st:#x}）"
    assert st & ST_LENERR, f"欠填应当同时置 LEN_ERR（STATUS={st:#x}）"
    assert not busy_seen, "欠填却启动了 engine —— 残留已经进了运算"
    assert not (st & ST_DONE), "欠填竟然报了 DONE"
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"欠填之后 OUT_LEN = {n}"

    # 喂满就照常
    assert await run_op(dut, OP_KEYGEN, pset, bytes(32)) == PK[pset] + SK[pset]
    st, _ = await rd(dut, STATUS)
    assert not (st & (ST_PARAMERR | ST_LENERR)), "喂满之后错误位还挂着"

    dut._log.info("KeyGen 喂 31/32 被拒且未启动 engine；喂满照常")


@cocotb.test()
async def test_length_check_covers_all_stream_shapes(dut):
    """长度校验：走金库/不走金库 × ctx 空/非空，**少一个字节就必须被拒**

    这四种组合的欠填门槛各不相同：
        不走金库 : sk + 32(rnd) + ctx + msg
        走金库   : 32(rnd) + ctx + msg          ← sk 不由软件送，不能算进去
    算错任何一边都会出安静的错误：门槛算高了，一个完全正确的调用被判参数错；
    算低了，engine 拿残留当 rnd 或 sk 去签 —— 签出来的东西**照样能验过**
    （用的是同一份坏材料对应的 pk），没有任何痕迹。

    每种组合都做两次：少一个字节必须被拒，正好喂满必须跑完且签名对得上。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    xi = bytes([0x33] * 32)
    _, sk = stub_keygen(pset, xi)
    rnd = bytes(range(32))
    msg = b"length-check"

    # 金库那条路要先有 sk 在槽里
    await run_op(dut, OP_KEYGEN, pset, xi, to_slot=True, slot=0)

    for from_slot in (False, True):
        for ctx in (b"", b"\xAA\xBB\xCC"):
            label = ("走金库" if from_slot else "自送 sk") + \
                    ("、ctx 非空" if ctx else "、ctx 空")
            full = sign_payload(sk, rnd, ctx, msg, from_slot=from_slot)
            want = (32 + len(ctx) + len(msg)
                    + (0 if from_slot else SK[pset]))
            assert len(full) == want, f"{label}：用例自己的长度就不对"

            # 少一个字节
            r = await run_op(dut, OP_SIGN, pset, full[:-1], from_slot=from_slot,
                             slot=0, msg_len=len(msg), ctx_len=len(ctx),
                             limit=3000)
            assert r is None, f"{label}：少一个字节居然跑起来了"
            st, _ = await rd(dut, STATUS)
            assert st & ST_LENERR, f"{label}：少一个字节应当置 LEN_ERR（{st:#x}）"

            # 正好喂满
            n = await run_op(dut, OP_SIGN, pset, full, from_slot=from_slot,
                             slot=0, msg_len=len(msg), ctx_len=len(ctx))
            assert n == SIG[pset], f"{label}：喂满之后 OUT_LEN={n}"
            assert await out_bytes(dut, 0, 8) == \
                stub_sign(pset, sk, rnd, ctx, msg)[:8], \
                f"{label}：签名字节不对 —— 排布或长度算错了"

    dut._log.info("四种流形态（走金库/自送 × ctx 空/非空）：少一字节全被拒，"
                  "喂满全对上")


@cocotb.test()
async def test_msg_len_counts_toward_need(dut):
    """MSG_LEN 报大了而字节没跟上 —— 也是欠填

    这一条单列，是因为它是软件最容易犯的错：sk 与 rnd 送全了，msg 只送了一半。
    长度校验必须把 MSG_LEN/CTX_LEN 算进去，否则 engine 会拿残留当消息签名，
    签出来的东西**一样能通过验证**（验的是同一份残留），错得毫无痕迹。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    _, sk = stub_keygen(pset, bytes([0x11] * 32))
    msg = b"0123456789"

    # 报 10 字节 msg，只送 4 个
    assert await run_op(dut, OP_SIGN, pset,
                        sign_payload(sk, RND0, b"", msg[:4]),
                        msg_len=len(msg), limit=3000) is None, "msg 欠填却跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_LENERR, f"msg 欠填应当置 LEN_ERR（STATUS={st:#x}）"

    # 送齐就过
    n = await run_op(dut, OP_SIGN, pset, sign_payload(sk, RND0, b"", msg),
                     msg_len=len(msg))
    assert n == SIG[pset]
    assert await out_bytes(dut, 0, 8) == stub_sign(pset, sk, RND0, b"", msg)[:8]

    dut._log.info("MSG_LEN 报了而字节没送齐：被判欠填；送齐之后签名逐字节对上")


@cocotb.test()
async def test_rnd_reaches_engine(dut):
    """rnd 那 32 个字节确实进了 engine，而且位置没错

    没有 rnd 入口就没法对 ACVP 的确定性 siggen 条目（rnd=0³²）验签名，
    所以这条要证明的不只是"长度算上了它"，而是**它真的被送进去了**：
    只改 rnd、别的一律不动，签出来的字节必须变。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    _, sk = stub_keygen(pset, bytes([0x77] * 32))
    msg, ctx = b"same-message", b"\x01"

    n = await run_op(dut, OP_SIGN, pset, sign_payload(sk, RND0, ctx, msg),
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == SIG[pset]
    det = await out_bytes(dut, 0, 16)
    assert det == stub_sign(pset, sk, RND0, ctx, msg)[:16], "确定性那一份不对"

    rnd1 = bytes([0xA5] * 32)
    n = await run_op(dut, OP_SIGN, pset, sign_payload(sk, rnd1, ctx, msg),
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == SIG[pset]
    hedged = await out_bytes(dut, 0, 16)
    assert hedged == stub_sign(pset, sk, rnd1, ctx, msg)[:16], "换了 rnd 那一份不对"

    assert det != hedged, (
        "只改 rnd 而签名一个字节没变 —— rnd 根本没进 engine，"
        "那 ACVP 的确定性条目就无从对起")

    dut._log.info("rnd 进到了 engine：rnd=0³² 与 rnd=0xA5×32 签出来的字节不同")


# ============================================================================
# ③ 私钥金库
# ============================================================================
@cocotb.test()
async def test_sk_stays_on_chip(dut):
    """SK_TO_SLOT：sk 不出现在 OUT_DATA；SK_FROM_SLOT：不送 sk 也能签

    两半都必须成立：
      · **出不来**：OUT_LEN 恰好是 pk 的长度，一个字节不多；而且**把读游标
        往 sk 那一段 seek 过去也一个字节都拿不到**（只查 OUT_LEN 是不够的 ——
        读指针要是没被卡住，软件照样能把 sk 捞出来）。
      · **还能用**：拿槽里的 sk 去签，签出来的字节与"软件自己送 sk"那一路
        **逐字节相同**。少了这一半，一个"把 sk 直接丢掉"的实现也能过上一半。
        替身的输出挂在整份输入的**摘要**上，所以这条相等还顺带证明了
        金库供的 sk 落在了与软件自送时**完全相同的偏移**上。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset, slot = 0, 3
    xi = bytes([0x21] * 32)
    pk_ref, sk_ref = stub_keygen(pset, xi)

    n = await run_op(dut, OP_KEYGEN, pset, xi, to_slot=True, slot=slot)
    assert n == PK[pset], f"存槽时 OUT_LEN={n}，应当恰好是 pk 的 {PK[pset]}"
    assert await out_bytes(dut, 0, 16) == pk_ref[:16], "pk 不对"

    # 读游标推到 sk 那一段：一个字节都不该给
    assert await wr(dut, OUT_PTR, PK[pset]) == RESP_OKAY
    for i in range(16):
        d, r = await rd(dut, OUT_DATA)
        assert r == RESP_OKAY and d == 0, (
            f"OUT_PTR seek 到 pk 之后第 {i} 个字节读到了 0x{d:02x} —— "
            "sk 从读游标那条路漏出来了")
    p, _ = await rd(dut, OUT_PTR)
    assert p == PK[pset], f"越界读把读游标推动了（OUT_PTR={p}）"

    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << slot), f"槽 {slot} 没被标成有效：KEYSTAT=0x{ks:08x}"
    assert ((ks >> 16) >> (2 * slot)) & 3 == pset, "槽里记的参数集不对"

    # 用槽签 vs 自己送 sk：必须逐字节相同
    ctx, msg = b"\xAA", b"sign-me"
    by_slot = await run_op(dut, OP_SIGN, pset,
                           sign_payload(sk_ref, RND0, ctx, msg, from_slot=True),
                           from_slot=True, slot=slot,
                           msg_len=len(msg), ctx_len=len(ctx))
    assert by_slot == SIG[pset], f"按槽签的 OUT_LEN={by_slot}"
    sig_slot = await out_bytes(dut, 0, 32)

    by_hand = await run_op(dut, OP_SIGN, pset,
                           sign_payload(sk_ref, RND0, ctx, msg),
                           msg_len=len(msg), ctx_len=len(ctx))
    assert by_hand == SIG[pset]
    sig_hand = await out_bytes(dut, 0, 32)

    assert sig_slot == sig_hand, (
        "按槽签与自己送 sk 签出来的不一样 —— 金库里的 sk 或者它进 engine 的"
        "位置是错的")
    assert sig_slot == stub_sign(pset, sk_ref, RND0, ctx, msg)[:32], \
        "与替身模型不一致"

    dut._log.info(f"sk 全程留在片内：OUT_LEN={PK[pset]}（正好 pk），"
                  "seek 到 sk 段读回全 0，按槽签与自送 sk 逐字节相同")


@cocotb.test()
async def test_sign_from_empty_or_mismatched_slot_refused(dut):
    """空槽 / 参数集不匹配的槽：当场拒绝，不是让它跑到超时

    槽不对时 sk 的长度算错，搬进 engine 的字节数就不对 —— 签出来的是一个
    "看起来完全合法、但私钥不是那一把"的安静错误。所以必须在 START 那一刻判掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    payload = sign_payload(b"", RND0, b"", b"m", from_slot=True)
    assert await run_op(dut, OP_SIGN, 0, payload, from_slot=True, slot=5,
                        msg_len=1, limit=3000) is None, "空槽居然签起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "空槽应当报 PARAM_ERR"
    assert not (st & ST_BUSY), "空槽被拒之后不该还 BUSY"

    # 往槽 5 存一把 44 的 sk，再按 65 去用它
    await run_op(dut, OP_KEYGEN, 0, bytes([7] * 32), to_slot=True, slot=5)
    assert await run_op(dut, OP_SIGN, 1, payload, from_slot=True, slot=5,
                        msg_len=1, limit=3000) is None, "参数集不匹配居然签起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "参数集不匹配应当报 PARAM_ERR"

    # 用对参数集就正常
    assert await run_op(dut, OP_SIGN, 0, payload, from_slot=True, slot=5,
                        msg_len=1) == SIG[0]

    dut._log.info("空槽与参数集不匹配都在 START 处判掉，没有跑到超时")


# ============================================================================
# ④ 一次性闩锁
# ============================================================================
@cocotb.test()
async def test_sk_lock_is_one_way(dut):
    """SK_LOCK 置上之后强制走金库，而且**没有任何写法能把它清掉**"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pset = 0
    xi = bytes([0x41] * 32)
    pk_ref, sk_ref = stub_keygen(pset, xi)

    # 闩锁之前：不设 SK_TO_SLOT 就该拿到 pk‖sk（ACVP 核对靠这条路）
    n = await run_op(dut, OP_KEYGEN, pset, xi)
    assert n == PK[pset] + SK[pset], "闩锁之前 sk 就出不来了，那 ACVP 没法核对"

    assert await wr(dut, CTRL, C_SK_LOCK) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & KS_LOCK, f"闩锁没置上（KEYSTAT=0x{ks:08x}）"

    # 闩锁之后：**同一个 MODE 字**（SK_TO_SLOT=0），sk 不再出来
    n = await run_op(dut, OP_KEYGEN, pset, xi, slot=1)
    assert n == PK[pset], f"闩锁之后 OUT_LEN={n}，应当只剩 pk 的 {PK[pset]}"
    assert await out_bytes(dut, 0, 8) == pk_ref[:8]
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 1), "闩锁强制走金库时槽没有被标成有效"

    # 而且确实是"进了金库"而不是"被丢掉"：拿它签，与自送 sk 一致
    msg = b"z"
    n = await run_op(dut, OP_SIGN, pset,
                     sign_payload(b"", RND0, b"", msg, from_slot=True),
                     from_slot=True, slot=1, msg_len=len(msg))
    assert n == SIG[pset]
    assert await out_bytes(dut, 0, 16) == \
        stub_sign(pset, sk_ref, RND0, b"", msg)[:16]

    # ---- 反证：没有任何写法能把它清掉 ----
    for label, addr, val in (
            ("CTRL 写全 1", CTRL, 0xFFFF_FFFF),
            ("CTRL 写 0", CTRL, 0x0000_0000),
            ("CTRL 写 ~SK_LOCK", CTRL, 0xFFFF_FFFF & ~C_SK_LOCK),
            ("CTRL 写 CLEAR", CTRL, C_CLEAR),
            ("MODE 写全 1", MODE, 0xFFFF_FFFF),
            ("MODE 写 0", MODE, 0x0000_0000),
            ("KEYSTAT 写 0（只读寄存器）", KEYSTAT, 0x0000_0000),
            ("VERSION 写 0（只读寄存器）", VERSION, 0x0000_0000),
            ("IN_PTR 写 0", IN_PTR, 0),
            ("OUT_PTR 写 0", OUT_PTR, 0),
            ("MSG_LEN 写 0", MSG_LEN, 0),
            ("CTX_LEN 写 0", CTX_LEN, 0)):
        await wr(dut, addr, val)
        # "CTRL 写全 1"顺带把 ZEROIZE（[2]）也置上了 —— 那正是这条反证要试的
        # 东西之一（"连擦带清一起来能不能把闩锁带走"）。擦除要 65536 拍，
        # 期间写会回 SLVERR，所以每一步之后都等它落下来再继续。
        await _settle_wipe(dut)
        ks, _ = await rd(dut, KEYSTAT)
        assert ks & KS_LOCK, f"{label} 之后闩锁掉了（KEYSTAT=0x{ks:08x}）"

    # ZEROIZE 也不行 —— 擦秘密不等于撤防线
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & KS_LOCK, "ZEROIZE 把闩锁清掉了 —— 它不该有这个能力"
    assert (ks & 0xFF) == 0, "ZEROIZE 之后槽的有效位应当全清"

    # 擦完之后闩锁仍然在管事
    n = await run_op(dut, OP_KEYGEN, pset, xi, slot=2)
    assert n == PK[pset], "ZEROIZE 之后闩锁不管事了"

    dut._log.info("闩锁一次性生效：12 种写法 + ZEROIZE 都撤不回来，擦完仍然在管事")


# ============================================================================
# ⑤ 非法输入
# ============================================================================
@cocotb.test()
async def test_illegal_params_refused(dut):
    """非法 OP / PSET / SLOT / ctx_len 在 START 那一刻被拒，且不启动 engine"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    cases = [
        ("op=3", mode_word(3, 0), 0),
        ("pset=3", mode_word(0, 3), 0),
        ("op=3 且 pset=3", mode_word(3, 3), 0),
        ("slot=8（只有 8 个槽）", mode_word(0, 0, to_slot=True, slot=8), 0),
        ("slot=15", mode_word(0, 0, to_slot=True, slot=15), 0),
        ("ctx_len=256（FIPS 204 上限 255）", mode_word(1, 0), 256),
    ]
    for why, mword, ctx in cases:
        assert await wr(dut, MODE, mword) == RESP_OKAY
        m, _ = await rd(dut, MODE)
        assert m == mword, f"{why}：MODE 回读 {m:#x}，非法值没进到寄存器里"
        assert await wr(dut, CTX_LEN, ctx) == RESP_OKAY
        assert await wr(dut, MSG_LEN, 0) == RESP_OKAY
        assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
        await fill(dut, bytes(64))     # 比任何一种合法组合的最小量还多

        assert await wr(dut, CTRL, C_START) == RESP_OKAY
        busy_seen = False
        for _ in range(150):
            st, _ = await rd(dut, STATUS)
            if st & ST_BUSY:
                busy_seen = True
        st, _ = await rd(dut, STATUS)
        assert st & ST_PARAMERR, f"{why}：PARAM_ERR 没置位（{st:#x}）"
        assert not busy_seen, f"{why}：engine 竟然被启动了"
        assert not (st & ST_DONE), f"{why}：竟然报了 DONE"
        n, _ = await rd(dut, OUT_LEN)
        assert n == 0, f"{why}：OUT_LEN={n}"

    # 换回合法参数：错误位清掉，照常能跑
    assert await wr(dut, CTX_LEN, 0) == RESP_OKAY
    assert await run_op(dut, OP_KEYGEN, 0, bytes(32)) == PK[0] + SK[0]
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_PARAMERR), "合法参数跑完之后 PARAM_ERR 还挂着"

    dut._log.info("op=3 / pset=3 / slot≥8 / ctx_len>255 全部被拒且未启动 engine")


@cocotb.test()
async def test_in_ptr_only_accepts_zero(dut):
    """IN_PTR 只认写 0 —— 任意设置写指针等于给了一条绕过喂够校验的路

    把指针推到"够了"的位置而字节其实是残留，正是长度校验要挡的东西。
    所以非零的写必须**明确回 SLVERR**，不是静默忽略（静默忽略的话软件会以为
    seek 成功了，接着按错误的排布灌字节）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wr(dut, MODE, mode_word(OP_KEYGEN, 0)) == RESP_OKAY
    await fill(dut, bytes(10))
    p, _ = await rd(dut, IN_PTR)
    assert p == 10

    assert await wr(dut, IN_PTR, 32) == RESP_SLVERR, "非零写 IN_PTR 没有回 SLVERR"
    p, _ = await rd(dut, IN_PTR)
    assert p == 10, f"非零写居然改动了写指针（IN_PTR={p}）"

    # 拿这个"被拒的 seek"去启动 KeyGen：仍然是欠填
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_LENERR, "seek 被拒之后仍然按 10 字节判 —— 这一条才是重点"

    assert await wr(dut, IN_PTR, 0) == RESP_OKAY
    p, _ = await rd(dut, IN_PTR)
    assert p == 0, "写 0 没有把指针复位"

    dut._log.info("IN_PTR 非零写回 SLVERR 且指针不动；写 0 正常复位")


@cocotb.test()
async def test_mode_write_resets_in_ptr(dut):
    """写 MODE 就清写指针 —— 挡的是"先灌字节再改 MODE"

    软件字节落在 engine 的哪个偏移**取决于 MODE**（SK_FROM_SLOT 那趟要给
    金库的 sk 让开前面 skLen 个字节）。所以"灌完再改 MODE"必须变成一个
    吵闹的错误，而不是按新排布去解读旧字节 —— 后者没有任何痕迹。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wr(dut, MODE, mode_word(OP_KEYGEN, 0)) == RESP_OKAY
    await fill(dut, bytes(32))
    p, _ = await rd(dut, IN_PTR)
    assert p == 32

    # 改 MODE（哪怕只是换个 pset）→ 指针归零
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, 1)) == RESP_OKAY
    p, _ = await rd(dut, IN_PTR)
    assert p == 0, f"写 MODE 之后 IN_PTR={p}，应当被清零"

    # 于是这时候 START 会报欠填，而不是拿旧字节按新排布跑
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_LENERR, f"改完 MODE 直接 START 应当报欠填（{st:#x}）"
    assert not (st & ST_BUSY)

    dut._log.info("写 MODE 清 IN_PTR：先灌后改 MODE 变成 LEN_ERR，不会安静跑错")


@cocotb.test()
async def test_params_are_read_only_while_busy(dut):
    """运行途中写 MODE / MSG_LEN / CTX_LEN / IN_DATA：回 SLVERR 且不生效

    中途改参数等于**换前提**：长度、槽号、sk 的去向全跟着变（搬 sk 搬到一半
    跳去另一个槽）。静默丢弃最危险 —— 软件会以为改成功了。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 起一次长活（ML-DSA-87 KeyGen，替身要铺 7488 字节输出）
    pset = 2
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, pset)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, bytes(32))
    assert await wr(dut, CTRL, C_START) == RESP_OKAY

    st, _ = await rd(dut, STATUS)
    assert st & ST_BUSY, "先决条件不成立：START 之后没 BUSY"

    assert await wr(dut, MODE, mode_word(OP_SIGN, 0)) == RESP_SLVERR, \
        "运行途中写 MODE 没有回 SLVERR"
    assert await wr(dut, MSG_LEN, 999) == RESP_SLVERR
    assert await wr(dut, CTX_LEN, 7) == RESP_SLVERR
    assert await wr(dut, IN_DATA, 0x5A) == RESP_SLVERR, \
        "运行途中写 IN_DATA 没有回 SLVERR（静默丢字节最危险）"

    # 一个都没生效
    m, _ = await rd(dut, MODE)
    assert m == mode_word(OP_KEYGEN, pset), f"运行途中的 MODE 写生效了（{m:#x}）"
    v, _ = await rd(dut, MSG_LEN)
    assert v == 0, f"运行途中的 MSG_LEN 写生效了（{v}）"
    v, _ = await rd(dut, CTX_LEN)
    assert v == 0

    for _ in range(40_000):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
    else:
        raise AssertionError("这一趟没跑完")
    n, _ = await rd(dut, OUT_LEN)
    assert n == PK[pset] + SK[pset], f"被打扰之后结果不对：OUT_LEN={n}"

    dut._log.info("运行途中改参数一律 SLVERR 且不生效，这一趟的结果不受影响")


# ============================================================================
# ⑥ 陈旧状态
# ============================================================================
@cocotb.test()
async def test_refused_start_invalidates_previous_result(dut):
    """被拒的 START 必须作废上一次的 DONE 与 OUT_LEN

    这一条是 ML-KEM 在板上抓到的形状，照搬过来防同一个坑：仿真里每条用例
    都从复位开始，OUT_LEN 本来就是 0，"拒绝之后留着上一次的结果"根本不会出现；
    板上是连着跑的 —— 软件轮询到 DONE=1、读出上一次的 OUT_LEN，
    拿着**上一次**的输出当成这一次的结果。比不报错更糟：它看起来成功了。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = await run_op(dut, OP_KEYGEN, 0, bytes([0x81] * 32))
    assert n == PK[0] + SK[0]
    st, _ = await rd(dut, STATUS)
    assert st & ST_DONE, "先决条件不成立：成功那次没报 DONE"

    # 紧接着来一次非法 START（op=3）
    assert await wr(dut, MODE, mode_word(3, 0)) == RESP_OKAY
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, f"非法 START 没置 PARAM_ERR（{st:#x}）"
    assert not (st & ST_BUSY), "非法 START 竟然启动了 engine"
    assert not (st & ST_DONE), (
        f"非法 START 之后 DONE 仍然是 1（STATUS={st:#x}）—— "
        "软件会拿上一次的输出当成这一次的结果")
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"非法 START 之后 OUT_LEN 还是 {n}"
    # 连字节也拿不到了
    assert await out_bytes(dut, 0, 4) == bytes(4), "被拒之后还能读到上一次的输出字节"

    # 欠填的 START 同样作废
    n = await run_op(dut, OP_KEYGEN, 0, bytes([0x82] * 32))
    assert n == PK[0] + SK[0]
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, bytes(4))
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_DONE) and (st & ST_LENERR)
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0

    dut._log.info("非法 START 与欠填 START 都当场作废上一次的 DONE/OUT_LEN")


@cocotb.test()
async def test_clear_leaves_nothing_behind(dut):
    """CLEAR 之后不留任何陈旧状态"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 先制造一份"脏状态"：跑完一次 Verify（DONE + verify_ok），再欠填一次（错误位）
    pset = 0
    ctx, msg = b"", b"\x01\x02"
    pk = bytes((i + 1) & 0xFF for i in range(PK[pset]))
    sig = make_verify_pass(pk, bytes((i * 5) & 0xFF for i in range(SIG[pset])),
                           ctx, msg)
    await run_op(dut, OP_VERIFY, pset, pk + sig + ctx + msg, msg_len=len(msg))
    st, _ = await rd(dut, STATUS)
    assert (st & ST_DONE) and (st & ST_VOK), f"先决条件不成立（STATUS={st:#x}）"

    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_DONE), f"CLEAR 之后 DONE 还在（{st:#x}）"
    assert not (st & ST_VOK), f"CLEAR 之后 verify_ok 还在（{st:#x}）"
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"CLEAR 之后 OUT_LEN={n}"
    p, _ = await rd(dut, IN_PTR)
    assert p == 0, f"CLEAR 之后 IN_PTR={p}"
    p, _ = await rd(dut, OUT_PTR)
    assert p == 0, f"CLEAR 之后 OUT_PTR={p}"

    # 错误位也清
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, 0)) == RESP_OKAY
    await fill(dut, bytes(4))
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_LENERR
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert not (st & (ST_PARAMERR | ST_LENERR)), f"CLEAR 没清错误位（{st:#x}）"

    dut._log.info("CLEAR 之后 DONE/verify_ok/OUT_LEN/IN_PTR/OUT_PTR/错误位全干净")


@cocotb.test()
async def test_repeat_same_op_twice(dut):
    """同一个 OP **连跑两次，中间只 CLEAR** —— 残留 done 那个坑

    engine 的 done 是电平，保持到下一次 start 才清；而 START 是非阻塞赋值，
    下一拍才真正拉高。一进 S_RUN 就看 done 的话，第二次运行会当场读到上一次
    残留的 done，立刻结束、OUT_LEN 是 0。**第一次永远对，第二次必错** ——
    ML-KEM 那边是在真硅上暴露的（每组向量 tc0 过、tc1 报 0 字节）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for i, xi in enumerate([bytes([0x41] * 32), bytes([0x51] * 32)]):
        pk_ref, _ = stub_keygen(0, xi)
        n = await run_op(dut, OP_KEYGEN, 0, xi)
        assert n == PK[0] + SK[0], (
            f"第 {i+1} 次 KeyGen OUT_LEN={n}"
            + ("  ← 第二次就是残留 done 那个 bug" if i else ""))
        assert await out_bytes(dut, 0, 8) == pk_ref[:8], f"第 {i+1} 次输出不对"

    # Sign 也连跑两次
    _, sk = stub_keygen(0, bytes([0x61] * 32))
    for i, msg in enumerate([b"aaaa", b"bbbb"]):
        n = await run_op(dut, OP_SIGN, 0, sign_payload(sk, RND0, b"", msg),
                         msg_len=len(msg))
        assert n == SIG[0], f"第 {i+1} 次 Sign OUT_LEN={n}"
        assert await out_bytes(dut, 0, 8) == \
            stub_sign(0, sk, RND0, b"", msg)[:8]

    dut._log.info("KeyGen 连跑两次、Sign 连跑两次，中间不复位 —— 全对")


# ============================================================================
# ⑦ 防火墙
# ============================================================================
@cocotb.test()
async def test_firewall_nonsecure_refused(dut):
    """non-secure 的读写被拦、无副作用；越界地址读回 0"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    n = await run_op(dut, OP_KEYGEN, 0, bytes([7] * 32))
    assert n == PK[0] + SK[0]

    v, r = await rd(dut, OUT_LEN, PROT_NONSEC)
    assert r == RESP_REFUSED and v == 0, f"non-secure 读没被拦：0x{v:08x}"
    v, r = await rd(dut, VERSION, PROT_NONSEC)
    assert r == RESP_REFUSED and v == 0, "non-secure 读到了 VERSION"

    assert await wr(dut, CTRL, C_ZEROIZE, PROT_NONSEC) == RESP_REFUSED, \
        "non-secure 写没被拦"
    assert await wr(dut, CTRL, C_SK_LOCK, PROT_NONSEC) == RESP_REFUSED

    # 被拦的那几笔没有副作用
    n, _ = await rd(dut, OUT_LEN)
    assert n == PK[0] + SK[0], "non-secure 的写产生了副作用（输出被清了）"
    ks, _ = await rd(dut, KEYSTAT)
    assert not (ks & KS_LOCK), "non-secure 居然把闩锁置上了"

    # 越界地址（窗口外）
    v, r = await rd(dut, 0x80)
    assert r == RESP_REFUSED and v == 0, f"越界地址没被拦：0x{v:08x}"

    # 违规计数确实在数
    viol, _ = await rd(dut, VIOL)
    assert (viol & 0xFFFF) >= 2 and (viol >> 16) >= 3, \
        f"违规计数不对：0x{viol:08x}（低半字是写、高半字是读）"

    dut._log.info("non-secure 与越界访问全部被拦、无副作用，违规计数在数")


# ============================================================================
# ⑧ 擦除
# ============================================================================
async def _settle_wipe(dut, limit=40_000):
    """如果正在擦，等它擦完；没在擦就立即返回（不断言擦除发生过）"""
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            return
    raise AssertionError("WIPING 一直没落下来")


async def _wait_wipe(dut, limit=40_000):
    """等 WIPING 落下来，顺便断言它确实曾经高过

    只依赖软件看得到的 STATUS 位 —— 板上程序能依赖的就是这一位。
    金库 64 KB → 擦除机要走 65536 拍，每次轮询是一笔 AXI 读、占好几拍，
    所以 40000 次轮询足够覆盖。
    """
    st, _ = await rd(dut, STATUS)
    assert st & ST_WIPING, (
        "写了 ZEROIZE 之后 STATUS.WIPING 没有拉高 —— "
        "说明根本没启动擦除，只是清了有效位")
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            return
    raise AssertionError("WIPING 一直没落下来")


def _mem_nonzero(mem):
    bad, first = 0, None
    for i in range(len(mem)):
        v = int(mem[i].value)
        if v:
            bad += 1
            if first is None:
                first = (i, v)
    return bad, first


@cocotb.test()
async def test_zeroize_really_wipes_vault(dut):
    """ZEROIZE 之后**读回 64 KB 金库的每一个字节**，必须全是 0

    判据不能是"槽的有效位清了"或"OUT_LEN 变 0" —— 那只证明目录页被撕了，
    正文还在不在它答不了。而正文就是 sk 本身：位流回读、扫描链、或者哪天
    有人给金库加个调试读口，都能把它捞出来。

    残留分两种，都要覆盖：
      · **真实残留**：真跑一次存槽 KeyGen，金库里就是真的 sk 字节；
      · **全量残留**：把 65536 个地址按固定步长撒满非零，证明擦的是整个
        地址空间，不是"用到的那一段"（只擦用过的那段是个很容易犯、
        而且看起来一样有效的错）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await run_op(dut, OP_KEYGEN, 0, bytes([0x11] * 32), to_slot=True, slot=0)
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & 1, "先决条件不成立：槽 0 没装上"

    mem = dut.u_skvault.mem
    depth = len(mem)
    assert depth == 65536, f"金库深度 {depth}，应当是 65536（8 槽 × 8192）"

    live = sum(1 for i in range(SK[0]) if int(mem[i].value))
    assert live > 1000, f"金库里只有 {live} 个非零字节，真实残留没建立起来"

    # 全量残留：按步长撒满整个地址空间（逐个写 65536 次太慢，
    # 步长取质数 97，覆盖每个槽、每个 BRAM 页）
    for i in range(0, depth, 97):
        mem[i].value = 0xAB
    await RisingEdge(dut.clk)

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    # 擦除期间：拒绝写并**明确回 SLVERR**（静默丢弃会让软件按错误长度启动）
    assert await wr(dut, IN_DATA, 0x99) == RESP_SLVERR, \
        "擦除期间的写没有被拒 —— 静默丢弃会让软件以为字节灌进去了"
    await _wait_wipe(dut)

    bad, first = _mem_nonzero(mem)
    assert bad == 0, (
        f"擦除之后金库还有 {bad}/{depth} 个字节非零，"
        f"第一个在 [{first[0]}] = 0x{first[1]:02x}")

    ks, _ = await rd(dut, KEYSTAT)
    assert (ks & 0xFF) == 0, "擦除之后槽的有效位应当全清"

    # 擦完还能照常再跑
    assert await run_op(dut, OP_KEYGEN, 0, bytes([0x11] * 32)) == PK[0] + SK[0]

    dut._log.info(f"ZEROIZE 后 {depth} 字节金库逐字节读回，全为 0；擦除期间写回 SLVERR")


@cocotb.test()
async def test_tamper_wipes_vault_and_closes_bus(dut):
    """tamper 走同一台擦除机，而且它是**锁存**的

    擦除机若用电平触发，tamper 之后会永远重启擦除、WIPING 再也不会落下来。
    所以这条用例专门等 WIPING 落地。tamper 之后防火墙整个关闭，
    只能看内部的 wiping —— 这是唯一一处没法从软件侧观测的地方，
    因为软件侧此时已经被整体拒绝了。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await run_op(dut, OP_KEYGEN, 0, bytes([0x33] * 32), to_slot=True, slot=4)
    mem = dut.u_skvault.mem
    for i in range(0, len(mem), 97):
        mem[i].value = 0x5A
    await RisingEdge(dut.clk)

    dut.tamper.value = 1
    await RisingEdge(dut.clk)
    dut.tamper.value = 0

    # tamper 之后总线整个关闭：读也读不到东西
    v, r = await rd(dut, VERSION)
    assert r == RESP_REFUSED and v == 0, \
        f"tamper 之后还能读到 VERSION=0x{v:08x}"

    # 逐时钟等擦完：上限必须大于擦除拍数本身（金库 64 KB → 65536 拍）
    for _ in range(80_000):
        await RisingEdge(dut.clk)
        if not int(dut.wiping.value):
            break
    else:
        raise AssertionError("tamper 之后 WIPING 一直没落下来 —— "
                             "多半是用电平而不是上升沿触发擦除")

    bad, first = _mem_nonzero(mem)
    assert bad == 0, (
        f"tamper 之后金库还有 {bad} 个字节非零，"
        f"第一个在 [{first[0]}] = 0x{first[1]:02x}")

    dut._log.info("tamper 触发一次完整擦除，64 KB 金库读回全 0，总线全程被拒")
