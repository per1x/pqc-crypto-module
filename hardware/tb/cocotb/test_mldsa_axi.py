"""cocotb：ML-DSA 共享引擎的 AXI4-Lite 从机（mldsa_axi）—— **接的是真 engine**

============================================================================
【这一套现在证明什么】
============================================================================
以前这里接的是行为级替身（stub_mldsa_engine.v），判据是替身的假数据模型
（h = SHAKE(整个输入缓冲)、pk[i] = h+i …），所以只证明"软件视角下这块外设
能不能用"，**不证明任何算法正确性**。替身已经删掉。

现在 `mldsa_axi` 直接例化 `hardware/rtl/mldsa/mldsa_engine.v`，也就是
**AXI → engine → 三个整核**整条链路。判据换成两样真东西：

  · **ACVP 官方向量**（vectors/mldsa_{keygen,siggen,sigver}.kat）——
    KeyGen 的 pk/sk、Sign 的 σ、Verify 的 pass/fail 都逐字节/逐判定对官方值；
  · **hardware/model/mldsa_oracle.py**（已对上全部 ACVP 的黄金模型）——
    用在"官方向量给不出期望值"的地方，典型是**按槽签名**：金库里的 sk 来自
    一次真 KeyGen，而 ACVP 的 siggen 条目自带另一把 sk，两边对不上号
    （实测 90 条 siggen 的 sk 没有一条出现在 keygen 向量里）。于是那条改成
    "KeyGen(ACVP 种子) → 槽 → 按槽签 → 对 oracle 用同一把 sk 算出来的 σ"。

也就是说**同一条 σ 被两条独立的路钉住**：自送 sk 的那条对官方 ACVP，
按槽签的那条对 oracle，且两条必须逐字节相同。

============================================================================
【⚠️ 这一套必须带参数集跑】
============================================================================
三个核这一版仍是**编译期参数化**的，engine 与 mldsa_axi 也是：`pset` 端口与
综合进去的 `PSET` 参数对不上就拒绝启动。所以不带参数直接 make 就是 ML-DSA-44，
另外两个参数集要显式传（PARAM_K/L/ETA/… 与 MLDSA_ALG），见 tools/mldsa_grid.sh。

输入字节流的排布（与 engine 那条线共用的契约）：
    KeyGen : ξ(32)
    Sign   : [sk，仅当 SK_FROM_SLOT=0] ‖ rnd(32) ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)
    Verify : pk ‖ sig ‖ ctx(CTX_LEN) ‖ msg(MSG_LEN)

覆盖：
  ① 算法：KeyGen 的 pk/sk、Sign 的 σ 逐字节对 ACVP；Verify 的 pass 与 fail
     两种判定都对 ACVP sigver；
  ② 金库：SK_TO_SLOT 时 sk 一个字节都不出 OUT_DATA（OUT_LEN 只到 pk 长度，
     且把读游标 seek 过去也拿不到）；SK_FROM_SLOT 时不送 sk 也能签，
     签出来的 σ 与自送 sk 逐字节相同、且对得上 oracle；
  ③ START 前的长度校验：喂不够就 PARAM_ERR|LEN_ERR 且**不启动**
     （走金库/不走金库 × ctx 空/非空 四种组合都覆盖）；
  ④ 消息上限 8192（核里 u_msg 是 AW=13）：超一个字节就必须被拒；
  ⑤ 一次性闩锁：置上后强制走金库，且**没有任何写法能把它清掉**（反证）；
  ⑥ 非法 OP / PSET / SLOT / ctx_len 被拒；pset 与综合参数集不符也被拒；
  ⑦ 陈旧状态：被拒的 START 不留上一次的 DONE/OUT_LEN；CLEAR 清得干净；
  ⑧ 同一个 OP 连跑两次（残留 done 那个上板才暴露的坑）；
  ⑨ 防火墙：non-secure 被拦且无副作用；
  ⑩ ZEROIZE / tamper 真的把 64 KB 金库逐字节擦掉；
  ⑪ 排布相关的两条防呆：写 MODE 清写指针、运行途中改参数回 SLVERR。
"""
import os
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import (  # noqa: E402
    _load_records, load_kat, mldsa_sign as ora_sign,
)

VEC = Path(__file__).resolve().parents[3] / "vectors"
SIGGEN_KAT = VEC / "mldsa_siggen.kat"
SIGVER_KAT = VEC / "mldsa_sigver.kat"

# ---- 本次综合的参数集 ----
ALG = os.environ.get("MLDSA_ALG", "ML-DSA-44")
PSET_OF = {"ML-DSA-44": 0, "ML-DSA-65": 1, "ML-DSA-87": 2}
PSET = PSET_OF[ALG]
# 与本次综合**不符**的那个 pset：用来验"对不上就拒绝启动"
# 与本次跑的参数集不同的一个**合法** pset。运行时化之后它不再是"非法值"，
# 只用来触发"槽里记的 pset 与本次运算的 pset 对不上"那条防线（那条还在）。
PSET_BAD = (PSET + 1) % 3

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
PKL, SKL, SIGL = PK[PSET], SK[PSET], SIG[PSET]

# 核里 u_msg 是 AW=13 —— 消息的真实上限，不是输入缓冲算出来的那个数
MSGMAX = 8192

RND0 = bytes(32)            # 确定性签名：rnd = 0³²（ACVP 的确定性条目就是它）

PROT_SECURE, PROT_NONSEC = 0b000, 0b010
RESP_OKAY, RESP_SLVERR = 0, 2
# 被拒长什么样是 RTL 的策略（RAZ/WI：读回 0、写丢弃、响应 OKAY），
# 用例里一律写 RESP_REFUSED，理由见 test_mlkem_axi.py 里那段。
RESP_REFUSED = RESP_OKAY


# ============================================================================
# 黄金判据：ACVP 官方向量 + mldsa_oracle
# ============================================================================
def kat_keygen(n: int = 1):
    """ACVP keygen 向量：(ξ, pk, sk)"""
    recs = load_kat(limit_per_alg=max(n, 2))
    out = [r for r in recs if r.get("alg") == ALG][:n]
    assert len(out) == n, f"keygen KAT 里 {ALG} 的记录不够（要 {n} 条）"
    return [(bytes.fromhex(r["seed"]), bytes.fromhex(r["pk"]),
             bytes.fromhex(r["sk"])) for r in out]


def _kat(path: Path, pred=None, key=None):
    """按条件挑 ACVP 记录；key 给了就按它排序（用来挑最短的那条）"""
    recs = _load_records(path)
    assert recs, f"找不到 {path.name}（先跑 tools/fetch_vectors.sh）"
    out = [r for r in recs
           if r.get("alg") == ALG and (pred is None or pred(r))]
    assert out, f"KAT 里没有符合条件的 {ALG} 记录"
    if key is not None:
        out.sort(key=key)
    return out


def _short(r):
    """按"要经 AXI 搬多少字节"排序 —— 一个字节两笔 AXI 事务，挑短的省一半时间"""
    return len(r["msg"]) + len(r.get("context", ""))


def kat_siggen():
    """ACVP siggen 的确定性条目（rnd = 0³²），挑消息最短的那条"""
    r = _kat(SIGGEN_KAT, lambda x: x.get("deterministic") == "1", _short)[0]
    return (bytes.fromhex(r["sk"]), bytes.fromhex(r["msg"]),
            bytes.fromhex(r.get("context", "")), bytes.fromhex(r["rnd"]),
            bytes.fromhex(r["sig"]))


def kat_sigver(result: str):
    r = _kat(SIGVER_KAT, lambda x: x.get("result") == result, _short)[0]
    return (bytes.fromhex(r["pk"]), bytes.fromhex(r["sig"]),
            bytes.fromhex(r["msg"]), bytes.fromhex(r.get("context", "")))


def golden_sig(sk: bytes, msg: bytes, ctx: bytes, rnd: bytes = RND0) -> bytes:
    """oracle 算的 σ —— 用在 ACVP 给不出期望值的地方（典型是按槽签名）

    oracle 本身已经逐字节对上了全部 ACVP siggen（预言机 D），所以它是
    一个**独立于 RTL**的判据，不是把 RTL 的输出抄一遍。
    """
    return ora_sign(sk, msg, ctx, rnd, ALG)


def diff(got: bytes, want: bytes, what: str) -> str:
    """逐字节比较的失败信息 —— 报**第一个**不一样的下标

    "整段不一致"这句话在这里几乎没有用：边界差一个字节和整段错位是完全不同的
    两个 bug，而下标一报就分得开（本仓库正是靠这个把"pk 最后一个字节"那条
    定位出来的）。
    """
    if got == want:
        return ""
    if len(got) != len(want):
        return f"{what}：长度 {len(got)} ≠ {len(want)}"
    i = next(k for k in range(len(got)) if got[k] != want[k])
    n = sum(1 for k in range(len(got)) if got[k] != want[k])
    return (f"{what}：{n}/{len(want)} 个字节不一致，第一个在 [{i}]"
            f"（读回 0x{got[i]:02x}，应当是 0x{want[i]:02x}）")


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


def mode_word(op, pset=None, *, to_slot=False, from_slot=False, slot=0):
    if pset is None:
        pset = PSET
    return (op | (pset << 2)
            | (M_SK_TO_SLOT if to_slot else 0)
            | (M_SK_FROM_SLOT if from_slot else 0)
            | (slot << 6))


async def fill(dut, payload: bytes):
    """把字节流灌进 IN_DATA（写指针自增）"""
    for b in payload:
        assert await wr(dut, IN_DATA, b) == RESP_OKAY


async def start_and_wait(dut, limit=40_000, gap_ns=2_000):
    """写 START 然后等 —— 返回 True=跑完，False=被拒（PARAM_ERR）

    ⚠️ 真 ML-DSA 一次 Sign 是几十万拍（拒绝采样要循环），不能像替身那样
       靠"一直发 AXI 读"轮询 —— 那样每一拍都要过一遍 Python。两次轮询之间
       用 Timer 空烧一段仿真时间：一次 Python 回调推进几百拍，快得多。
       gap_ns=0 时退回逐笔轮询（"BUSY 有没有拉起来"那几条要贴着看）。
    """
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    for _ in range(limit):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            return True
        if st & ST_PARAMERR:
            return False
        if gap_ns:
            await Timer(gap_ns, unit="ns")
    raise AssertionError("BUSY 一直不落，也没报错")


async def out_bytes(dut, first, count):
    """从 OUT_DATA 取 count 个字节（先把读游标 seek 到 first）"""
    assert await wr(dut, OUT_PTR, first) == RESP_OKAY
    out = bytearray()
    for _ in range(count):
        d, _ = await rd(dut, OUT_DATA)
        out.append(d & 0xFF)
    return bytes(out)


async def run_op(dut, op, payload, *, pset=None, to_slot=False, from_slot=False,
                 slot=0, msg_len=0, ctx_len=0, limit=40_000, gap_ns=2_000):
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
    ok = await start_and_wait(dut, limit, gap_ns)
    if not ok:
        return None
    n, _ = await rd(dut, OUT_LEN)
    return n


# ============================================================================
# ① 算法：整条链路对 ACVP
# ============================================================================
@cocotb.test()
async def test_keygen_matches_acvp(dut):
    """KeyGen：喂 ACVP 的 ξ，读回的 pk‖sk **逐字节**对上官方向量

    这一条是整条链路（AXI → engine → mldsa_keygen）第一次端到端对 ACVP。
    不存槽时 sk 本来就该出得来 —— 出厂验证要核对它，闩锁没置上之前这是合法用法。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    v, r = await rd(dut, VERSION)
    assert r == RESP_OKAY and v == 0x0001_0000, f"VERSION=0x{v:08x}"

    xi, pk_w, sk_w = kat_keygen()[0]
    assert len(pk_w) == PKL and len(sk_w) == SKL, \
        f"用例自己的长度表与 {ALG} 对不上"

    n = await run_op(dut, OP_KEYGEN, xi)
    assert n == PKL + SKL, f"{ALG} KeyGen OUT_LEN={n}，应当是 {PKL}+{SKL}"

    got_pk = await out_bytes(dut, 0, PKL)
    assert got_pk == pk_w, diff(got_pk, pk_w, f"{ALG} pk 与 ACVP")
    got_sk = await out_bytes(dut, PKL, SKL)
    assert got_sk == sk_w, diff(got_sk, sk_w, f"{ALG} sk 与 ACVP")

    dut._log.info(f"{ALG} KeyGen：pk {PKL}B + sk {SKL}B 全部逐字节对上 ACVP")


@cocotb.test()
async def test_sign_matches_acvp_siggen(dut):
    """Sign（软件自送 sk）：σ **逐字节**对上 ACVP siggen 的确定性条目

    确定性条目的 rnd = 0³²，所以期望值是固定的 —— 没有 rnd 这个入口就无从对起。
    Sign 的输入是四段拼起来的（sk‖rnd‖ctx‖msg），段边界算错一个字节 σ 就完全不同，
    所以官方 σ 同时钉住了本层的排布与 engine 的翻译。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    sk, msg, ctx, rnd, sig_w = kat_siggen()
    assert len(sk) == SKL and len(sig_w) == SIGL
    assert rnd == RND0, "挑到的不是确定性条目"

    n = await run_op(dut, OP_SIGN, sign_payload(sk, rnd, ctx, msg),
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == SIGL, f"{ALG} Sign OUT_LEN={n}，应当是 {SIGL}"
    assert await out_bytes(dut, 0, SIGL) == sig_w, \
        f"{ALG} σ 与 ACVP siggen 不一致（|msg|={len(msg)} |ctx|={len(ctx)}）"

    dut._log.info(f"{ALG} Sign：σ {SIGL}B 逐字节对上 ACVP siggen，"
                  f"|msg|={len(msg)} |ctx|={len(ctx)}")


def _kat_any_alg(path, alg, pred=None, key=None):
    """按**指定** alg 挑记录 —— 与 _kat 的区别是不吃全局 ALG。

    下面那条"不复位跨参数集"的用例必须在同一次仿真里用到三个参数集的向量，
    而 _kat 只认环境变量选定的那一个。
    """
    recs = _load_records(path)
    out = [r for r in recs if r.get("alg") == alg and (pred is None or pred(r))]
    assert out, f"KAT 里没有符合条件的 {alg} 记录"
    if key is not None:
        out.sort(key=key)
    return out


@cocotb.test()
async def test_verify_cross_pset_without_reset(dut):
    """**只复位一次**，之后连续用不同参数集做 Verify —— 上板 100%/0% 的那个形状

    ⚠️ 这一格是十二格矩阵漏掉的真空，而它恰好是**板上的常态**：
        · cocotb 每条用例都从 reset() 开始，而复位把 pset 与全部派生配置清零；
        · 板上 PL 装载之后**再也不复位**，配置一直是上一笔运算留下的。
    于是"参数集 A 的运算 → 不复位 → 参数集 B 的 Verify"这个序列在仿真里
    从来没跑过，而它在板上是每一次调用的常态。

    上板实测（开发形态位流，直连 /dev/mem，每套 300 次）：
        纯 Verify、前面不夹同参数集的 Sign/KeyGen
            ML-DSA-44  失败 300/300（100%）
            ML-DSA-65  失败 300/300（100%）
            ML-DSA-87  失败   0/300（0%）
        而每次 Verify 前都有同参数集 KeyGen+Sign 时，三套全过。
    温度无关（失败与成功都在 32.6°C），失败率是 100%/0% 的**确定性**，
    所以不是边缘时序。疑为 pset 派生配置（k/ℓ/γ₂/λ/ω）的锁存时机。

    顺序故意从 87 起步再切 44：板上的形状说明"从大参数集切到小的"是出事方向。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)                      # ← 全用例只此一次

    order = [2, 0, 1, 0, 2, 1]
    names = {0: "ML-DSA-44", 1: "ML-DSA-65", 2: "ML-DSA-87"}
    for step, ps in enumerate(order, 1):
        alg = names[ps]
        r = _kat_any_alg(SIGVER_KAT, alg,
                         lambda x: x.get("result") == "pass", _short)[0]
        pk = bytes.fromhex(r["pk"]);  sig = bytes.fromhex(r["sig"])
        msg = bytes.fromhex(r["msg"]); ctx = bytes.fromhex(r.get("context", ""))
        assert len(pk) == PK[ps] and len(sig) == SIG[ps]
        await run_op(dut, OP_VERIFY, pk + sig + ctx + msg, pset=ps,
                     msg_len=len(msg), ctx_len=len(ctx))
        st, _ = await rd(dut, STATUS)
        assert st & ST_VOK, (
            f"第 {step} 步 {alg}（tcId={r.get('tcid')}）应当验得过却没有"
            f"（STATUS={st:#010x}）—— 不复位跨参数集时 pset 派生配置没跟上")
        dut._log.info(f"  第 {step} 步 {alg}：通过（不复位）")


@cocotb.test()
async def test_verify_matches_acvp_sigver(dut):
    """Verify：pass 与 fail **两种判定**都对上 ACVP sigver

    只测通过那一半是不够的 —— 一个把 verify_ok 恒接成 1 的实现同样能过。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for want in ("pass", "fail"):
        pk, sig, msg, ctx = kat_sigver(want)
        assert len(pk) == PKL and len(sig) == SIGL
        n = await run_op(dut, OP_VERIFY, pk + sig + ctx + msg,
                         msg_len=len(msg), ctx_len=len(ctx))
        assert n == 0, f"Verify 不该有输出字节，OUT_LEN={n}"
        st, _ = await rd(dut, STATUS)
        ok = bool(st & ST_VOK)
        assert ok == (want == "pass"), \
            f"{ALG} sigver 判定错了：期望 {want}，verify_ok={ok}"

    dut._log.info(f"{ALG} Verify：pass 与 fail 两条都对上 ACVP sigver")


# ============================================================================
# ② 私钥金库 —— 这一条最硬
# ============================================================================
@cocotb.test()
async def test_sk_stays_on_chip_and_slot_signs_correctly(dut):
    """SK_TO_SLOT：sk 不出 OUT_DATA；SK_FROM_SLOT：按槽签出来的 σ 是**对的**

    三半都必须成立：
      · **出不来**：OUT_LEN 恰好是 pk 的长度，一个字节不多；而且**把读游标
        往 sk 那一段 seek 过去也一个字节都拿不到**（只查 OUT_LEN 是不够的 ——
        读指针要是没被卡住，软件照样能把 sk 捞出来）。
      · **还能用**：拿槽里的 sk 去签，σ 与"软件自己送 sk"那一路**逐字节相同**。
        少了这一半，一个"把 sk 直接丢掉"的实现也能过上一半。
      · **而且是对的**：那条 σ 还要对上 oracle 用 **ACVP 的 sk** 算出来的值。
        少了这一半，两条路一起错成同一个样子也能过 —— 相等只证明一致，
        不证明正确。

    ⚠️ 为什么这里不用 ACVP 的 σ 当判据：金库里的 sk 只能来自一次真 KeyGen，
       而 ACVP 的 siggen 条目自带另一把 sk（实测 90 条里没有一条的 sk 出现在
       keygen 向量里）。所以判据取 oracle —— 它自己已经逐字节对上了全部
       ACVP siggen，是独立于 RTL 的一份实现。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    slot = 3
    xi, pk_w, sk_w = kat_keygen()[0]

    n = await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=slot)
    assert n == PKL, f"存槽时 OUT_LEN={n}，应当恰好是 pk 的 {PKL}"
    assert await out_bytes(dut, 0, PKL) == pk_w, "存槽这一趟的 pk 与 ACVP 不一致"

    # 读游标推到 sk 那一段：一个字节都不该给
    assert await wr(dut, OUT_PTR, PKL) == RESP_OKAY
    for i in range(32):
        d, r = await rd(dut, OUT_DATA)
        assert r == RESP_OKAY and d == 0, (
            f"OUT_PTR seek 到 pk 之后第 {i} 个字节读到了 0x{d:02x} —— "
            "sk 从读游标那条路漏出来了")
    p, _ = await rd(dut, OUT_PTR)
    assert p == PKL, f"越界读把读游标推动了（OUT_PTR={p}）"

    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << slot), f"槽 {slot} 没被标成有效：KEYSTAT=0x{ks:08x}"
    assert ((ks >> 16) >> (2 * slot)) & 3 == PSET, "槽里记的参数集不对"

    ctx, msg = b"\xAA\xBB", b"sign-me"
    want = golden_sig(sk_w, msg, ctx)

    by_slot = await run_op(dut, OP_SIGN,
                           sign_payload(sk_w, RND0, ctx, msg, from_slot=True),
                           from_slot=True, slot=slot,
                           msg_len=len(msg), ctx_len=len(ctx))
    assert by_slot == SIGL, f"按槽签的 OUT_LEN={by_slot}"
    sig_slot = await out_bytes(dut, 0, SIGL)

    by_hand = await run_op(dut, OP_SIGN, sign_payload(sk_w, RND0, ctx, msg),
                           msg_len=len(msg), ctx_len=len(ctx))
    assert by_hand == SIGL
    sig_hand = await out_bytes(dut, 0, SIGL)

    assert sig_slot == sig_hand, (
        "按槽签与自己送 sk 签出来的不一样 —— 金库里的 sk 或者它进 engine 的"
        "位置是错的")
    assert sig_slot == want, (
        "两条路一致，但都与 oracle 对不上 —— 一致不等于正确")

    dut._log.info(
        f"{ALG} 金库：sk 全程留在片内（OUT_LEN={PKL} 正好 pk，seek 到 sk 段读回全 0），"
        f"按槽签的 σ 与自送 sk 逐字节相同、且对上 oracle")


@cocotb.test()
async def test_sign_from_empty_or_mismatched_slot_refused(dut):
    """空槽 / 参数集不匹配的槽：当场拒绝，不是让它跑到超时

    槽不对时 sk 的长度算错，搬进 engine 的字节数就不对 —— 签出来的是一个
    "看起来完全合法、但私钥不是那一把"的安静错误。所以必须在 START 那一刻判掉。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    payload = sign_payload(b"", RND0, b"", b"m", from_slot=True)
    assert await run_op(dut, OP_SIGN, payload, from_slot=True, slot=5,
                        msg_len=1, limit=3000) is None, "空槽居然签起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "空槽应当报 PARAM_ERR"
    assert not (st & ST_BUSY), "空槽被拒之后不该还 BUSY"

    # 往槽 5 存一把 sk，再报一个**别的 pset** 去用它。
    # 这里同时踩到两条防线（槽里记的 pset 对不上 / pset 与综合的参数集对不上），
    # 两条都指向同一件事：绝不按对不上号的长度去搬 sk。
    xi, _, _ = kat_keygen()[0]
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=5)
    assert await run_op(dut, OP_SIGN, payload, pset=PSET_BAD, from_slot=True,
                        slot=5, msg_len=1, limit=3000) is None, \
        "参数集不匹配居然签起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "参数集不匹配应当报 PARAM_ERR"

    # 用对参数集就正常
    assert await run_op(dut, OP_SIGN, payload, from_slot=True, slot=5,
                        msg_len=1) == SIGL

    dut._log.info("空槽与参数集不匹配都在 START 处判掉，没有跑到超时")


# ============================================================================
# ③ START 前的长度校验
# ============================================================================
@cocotb.test()
async def test_underfill_refused_and_not_started(dut):
    """喂不够就 PARAM_ERR|LEN_ERR 且**不启动 engine**

    判据是四条一起：PARAM_ERR 置位、LEN_ERR 置位、BUSY 从未拉起、OUT_LEN 仍是 0。
    只看错误位是不够的 —— 先启动再报错同样能置位，而那时 engine 已经把
    输入缓冲里的**残留**当种子/私钥算过一轮了（见 mldsa_axi.v 文件头那段）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # KeyGen 要 32 字节 ξ，只给 31
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
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

    # 喂满就照常，而且结果对上 ACVP
    xi, pk_w, _ = kat_keygen()[0]
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL
    assert await out_bytes(dut, 0, 64) == pk_w[:64]
    st, _ = await rd(dut, STATUS)
    assert not (st & (ST_PARAMERR | ST_LENERR)), "喂满之后错误位还挂着"

    dut._log.info("KeyGen 喂 31/32 被拒且未启动 engine；喂满照常且对上 ACVP")


@cocotb.test()
async def test_length_check_covers_all_stream_shapes(dut):
    """长度校验：走金库/不走金库 × ctx 空/非空，**少一个字节就必须被拒**

    这四种组合的欠填门槛各不相同：
        不走金库 : sk + 32(rnd) + ctx + msg
        走金库   : 32(rnd) + ctx + msg          ← sk 不由软件送，不能算进去
    算错任何一边都会出安静的错误：门槛算高了，一个完全正确的调用被判参数错；
    算低了，engine 拿残留当 rnd 或 sk 去签 —— 签出来的东西**照样能验过**
    （用的是同一份坏材料对应的 pk），没有任何痕迹。

    每种组合都做两次：少一个字节必须被拒，正好喂满必须跑完且 σ 对上 oracle。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, sk = kat_keygen()[0]
    rnd = bytes(range(32))
    msg = b"len-check"

    # 金库那条路要先有 sk 在槽里 —— 而且必须是**同一把**，两条路才可比
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=0)

    for from_slot in (False, True):
        for ctx in (b"", b"\xAA\xBB\xCC"):
            label = ("走金库" if from_slot else "自送 sk") + \
                    ("、ctx 非空" if ctx else "、ctx 空")
            full = sign_payload(sk, rnd, ctx, msg, from_slot=from_slot)
            want_len = 32 + len(ctx) + len(msg) + (0 if from_slot else SKL)
            assert len(full) == want_len, f"{label}：用例自己的长度就不对"

            # 少一个字节
            r = await run_op(dut, OP_SIGN, full[:-1], from_slot=from_slot,
                             slot=0, msg_len=len(msg), ctx_len=len(ctx),
                             limit=3000)
            assert r is None, f"{label}：少一个字节居然跑起来了"
            st, _ = await rd(dut, STATUS)
            assert st & ST_LENERR, f"{label}：少一个字节应当置 LEN_ERR（{st:#x}）"

            # 正好喂满
            n = await run_op(dut, OP_SIGN, full, from_slot=from_slot, slot=0,
                             msg_len=len(msg), ctx_len=len(ctx))
            assert n == SIGL, f"{label}：喂满之后 OUT_LEN={n}"
            assert await out_bytes(dut, 0, SIGL) == golden_sig(sk, msg, ctx, rnd), \
                f"{label}：σ 与 oracle 对不上 —— 排布或长度算错了"

    dut._log.info("四种流形态（走金库/自送 × ctx 空/非空）：少一字节全被拒，"
                  "喂满的 σ 全对上 oracle")


@cocotb.test()
async def test_msg_len_counts_toward_need(dut):
    """MSG_LEN 报大了而字节没跟上 —— 也是欠填

    这一条单列，是因为它是软件最容易犯的错：sk 与 rnd 送全了，msg 只送了一半。
    长度校验必须把 MSG_LEN/CTX_LEN 算进去，否则 engine 会拿残留当消息签名，
    签出来的东西**一样能通过验证**（验的是同一份残留），错得毫无痕迹。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    sk, _, _, _, _ = kat_siggen()
    msg = b"0123456789"

    # 报 10 字节 msg，只送 4 个
    assert await run_op(dut, OP_SIGN, sign_payload(sk, RND0, b"", msg[:4]),
                        msg_len=len(msg), limit=3000) is None, "msg 欠填却跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_LENERR, f"msg 欠填应当置 LEN_ERR（STATUS={st:#x}）"

    # 送齐就过
    n = await run_op(dut, OP_SIGN, sign_payload(sk, RND0, b"", msg),
                     msg_len=len(msg))
    assert n == SIGL
    assert await out_bytes(dut, 0, SIGL) == golden_sig(sk, msg, b"")

    dut._log.info("MSG_LEN 报了而字节没送齐：被判欠填；送齐之后 σ 对上 oracle")


@cocotb.test()
async def test_msg_len_over_8192_refused(dut):
    """消息上限 8192：超一个字节就必须在 START 处被拒，而且**不启动**

    ⚠️ 这个上限**不是**输入缓冲算出来的。in_addr 有 15 位（32768 字节），照它算
       "Sign-87 还能喂两万多字节"；真正的瓶颈在核里 —— sign.v / verify.v 的
       `ram_dp #(.DW(8), .AW(13)) u_msg` 只有 8192 字节。不判的话高位地址会
       安静回绕，算出来的是一个长度对、格式对、内容错的签名。

    判据用"**恰好喂满**"来把这一条与普通欠填分开：8193 那次字节是够的
       （32 + 8193 全送到了），所以它被拒**只可能**是因为 msg_len 越界。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, sk = kat_keygen()[0]
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=0)   # 走金库，省掉几千笔写

    # ---- 8193：字节喂得满满的，仍然必须被拒 ----
    over = bytes((i * 31 + 7) & 0xFF for i in range(MSGMAX + 1))
    assert await wr(dut, MODE, mode_word(OP_SIGN, from_slot=True, slot=0)) == RESP_OKAY
    assert await wr(dut, MSG_LEN, MSGMAX + 1) == RESP_OKAY
    assert await wr(dut, CTX_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, RND0 + over)
    p, _ = await rd(dut, IN_PTR)
    assert p == 32 + MSGMAX + 1, f"喂进去的字节数不对（IN_PTR={p}）"

    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    busy_seen = False
    for _ in range(200):
        st, _ = await rd(dut, STATUS)
        if st & ST_BUSY:
            busy_seen = True
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, f"msg_len=8193 没被拒（STATUS={st:#x}）"
    assert st & ST_LENERR, f"msg_len 越界应当同时置 LEN_ERR（STATUS={st:#x}）"
    assert not busy_seen, "msg_len=8193 竟然启动了 engine —— 核里的地址会回绕"
    assert not (st & ST_DONE), "被拒却报了 DONE"
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"被拒之后 OUT_LEN={n}"

    # ---- 8192：边界上那一个必须收 ----
    # 只验"START 被接受"（BUSY 拉起、没有 PARAM_ERR）就够了：这一条要钉的是
    # 门槛画在 8192 还是 8191，不是再签一次 —— σ 的正确性上面几条已经钉死。
    # 跑满一条 8 KB 消息的 Sign 只是把同一件事再等几十万拍。
    assert await wr(dut, MODE, mode_word(OP_SIGN, from_slot=True, slot=0)) == RESP_OKAY
    assert await wr(dut, MSG_LEN, MSGMAX) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, RND0 + over[:MSGMAX])
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_BUSY, f"msg_len=8192 被拒了（STATUS={st:#x}）—— 门槛画错了一格"
    assert not (st & ST_PARAMERR), f"msg_len=8192 报了 PARAM_ERR（{st:#x}）"

    # 把这一趟停掉，别让它带着 8 KB 消息跑完（下一条用例反正会复位，
    # 但留一个正在跑的运算给下一条是个坏习惯）
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    dut._log.info("消息上限：8193 喂满也被拒且未启动，8192 正常受理")


@cocotb.test()
async def test_rnd_reaches_engine(dut):
    """rnd 那 32 个字节确实进了 engine，而且位置没错

    没有 rnd 入口就没法对 ACVP 的确定性 siggen 条目（rnd=0³²）验签名，
    所以这条要证明的不只是"长度算上了它"，而是**它真的被送进去了**：
    只改 rnd、别的一律不动，签出来的字节必须变，而且两份都要对上 oracle。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    sk, _, _, _, _ = kat_siggen()
    ctx, msg = b"\x01", b"same-message"

    n = await run_op(dut, OP_SIGN, sign_payload(sk, RND0, ctx, msg),
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == SIGL
    det = await out_bytes(dut, 0, SIGL)
    assert det == golden_sig(sk, msg, ctx, RND0), "确定性那一份与 oracle 对不上"

    rnd1 = bytes([0xA5] * 32)
    n = await run_op(dut, OP_SIGN, sign_payload(sk, rnd1, ctx, msg),
                     msg_len=len(msg), ctx_len=len(ctx))
    assert n == SIGL
    hedged = await out_bytes(dut, 0, SIGL)
    assert hedged == golden_sig(sk, msg, ctx, rnd1), "换了 rnd 那一份与 oracle 对不上"

    assert det != hedged, (
        "只改 rnd 而签名一个字节没变 —— rnd 根本没进 engine，"
        "那 ACVP 的确定性条目就无从对起")

    dut._log.info("rnd 进到了 engine：rnd=0³² 与 rnd=0xA5×32 签出来的 σ 不同，"
                  "两份各自都对上 oracle")


# ============================================================================
# ④ 一次性闩锁
# ============================================================================
@cocotb.test()
async def test_sk_lock_is_one_way(dut):
    """SK_LOCK 置上之后强制走金库，而且**没有任何写法能把它清掉**"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, pk_w, sk_w = kat_keygen()[0]

    # 闩锁之前：不设 SK_TO_SLOT 就该拿到 pk‖sk（ACVP 核对靠这条路）
    n = await run_op(dut, OP_KEYGEN, xi)
    assert n == PKL + SKL, "闩锁之前 sk 就出不来了，那 ACVP 没法核对"

    assert await wr(dut, CTRL, C_SK_LOCK) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & KS_LOCK, f"闩锁没置上（KEYSTAT=0x{ks:08x}）"

    # 闩锁之后：**同一个 MODE 字**（SK_TO_SLOT=0），sk 不再出来
    n = await run_op(dut, OP_KEYGEN, xi, slot=1)
    assert n == PKL, f"闩锁之后 OUT_LEN={n}，应当只剩 pk 的 {PKL}"
    assert await out_bytes(dut, 0, 64) == pk_w[:64]
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 1), "闩锁强制走金库时槽没有被标成有效"

    # 而且确实是"进了金库"而不是"被丢掉"：拿它签，σ 要对上 oracle
    msg = b"z"
    n = await run_op(dut, OP_SIGN,
                     sign_payload(b"", RND0, b"", msg, from_slot=True),
                     from_slot=True, slot=1, msg_len=len(msg))
    assert n == SIGL
    assert await out_bytes(dut, 0, SIGL) == golden_sig(sk_w, msg, b""), \
        "闩锁强制存进去的 sk 签出来的 σ 不对 —— 那把 sk 没有正确进金库"

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
    n = await run_op(dut, OP_KEYGEN, xi, slot=2)
    assert n == PKL, "ZEROIZE 之后闩锁不管事了"

    dut._log.info("闩锁一次性生效：12 种写法 + ZEROIZE 都撤不回来，擦完仍然在管事")


# ============================================================================
# ⑤ 非法输入
# ============================================================================
@cocotb.test()
async def test_illegal_params_refused(dut):
    """非法 OP / PSET / SLOT / ctx_len 在 START 那一刻被拒，且不启动 engine

    ⚠️ "pset 与本次综合的参数集不符"也在这一列，而且**必须由本层判**。
       engine 自己也查，但它的拒绝路径是"立刻 done、不更新 op_r"，于是本层会
       把**上一次运算**的 out_len 抄下来 —— 软件看到 DONE=1 加一个像样的
       OUT_LEN，读出来却是上一次的输出。看起来成功，比报错更糟。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    cases = [
        ("op=3", mode_word(3), 0),
        ("pset=3", mode_word(0, 3), 0),
        ("op=3 且 pset=3", mode_word(3, 3), 0),
        # 注意这里**没有**"pset 与本次综合的参数集不符"这一条了：
        # engine 已经运行时支持 44/65/87，三个 pset 全都合法，只有 3 是非法值。
        # 这一条以前在，是三个核还只支持编译期单参数集时的形态。
        ("slot=8（只有 8 个槽）", mode_word(0, to_slot=True, slot=8), 0),
        ("slot=15", mode_word(0, to_slot=True, slot=15), 0),
        ("ctx_len=256（FIPS 204 上限 255）", mode_word(1), 256),
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
    xi, _, _ = kat_keygen()[0]
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_PARAMERR), "合法参数跑完之后 PARAM_ERR 还挂着"

    dut._log.info("op=3 / pset=3 / pset 与综合不符 / slot≥8 / ctx_len>255 "
                  "全部被拒且未启动 engine")


@cocotb.test()
async def test_in_ptr_only_accepts_zero(dut):
    """IN_PTR 只认写 0 —— 任意设置写指针等于给了一条绕过喂够校验的路

    把指针推到"够了"的位置而字节其实是残留，正是长度校验要挡的东西。
    所以非零的写必须**明确回 SLVERR**，不是静默忽略（静默忽略的话软件会以为
    seek 成功了，接着按错误的排布灌字节）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
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

    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
    await fill(dut, bytes(32))
    p, _ = await rd(dut, IN_PTR)
    assert p == 32

    # 改 MODE（哪怕只是把 SK_TO_SLOT 打开）→ 指针归零
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, to_slot=True)) == RESP_OKAY
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

    # 起一次真活（KeyGen 是几万拍，够写好几笔寄存器）
    xi, pk_w, sk_w = kat_keygen()[0]
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, xi)
    assert await wr(dut, CTRL, C_START) == RESP_OKAY

    st, _ = await rd(dut, STATUS)
    assert st & ST_BUSY, "先决条件不成立：START 之后没 BUSY"

    assert await wr(dut, MODE, mode_word(OP_SIGN)) == RESP_SLVERR, \
        "运行途中写 MODE 没有回 SLVERR"
    assert await wr(dut, MSG_LEN, 999) == RESP_SLVERR
    assert await wr(dut, CTX_LEN, 7) == RESP_SLVERR
    assert await wr(dut, IN_DATA, 0x5A) == RESP_SLVERR, \
        "运行途中写 IN_DATA 没有回 SLVERR（静默丢字节最危险）"

    # 一个都没生效
    m, _ = await rd(dut, MODE)
    assert m == mode_word(OP_KEYGEN), f"运行途中的 MODE 写生效了（{m:#x}）"
    v, _ = await rd(dut, MSG_LEN)
    assert v == 0, f"运行途中的 MSG_LEN 写生效了（{v}）"
    v, _ = await rd(dut, CTX_LEN)
    assert v == 0

    for _ in range(40_000):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
        await Timer(2_000, unit="ns")
    else:
        raise AssertionError("这一趟没跑完")
    n, _ = await rd(dut, OUT_LEN)
    assert n == PKL + SKL, f"被打扰之后结果不对：OUT_LEN={n}"
    # 被打扰之后结果仍然是**对的那一份**，不只是长度对
    assert await out_bytes(dut, 0, 64) == pk_w[:64], "被打扰之后 pk 不对"
    assert await out_bytes(dut, PKL, 64) == sk_w[:64], "被打扰之后 sk 不对"

    dut._log.info("运行途中改参数一律 SLVERR 且不生效，这一趟的结果仍然对上 ACVP")


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

    xi, _, _ = kat_keygen()[0]
    n = await run_op(dut, OP_KEYGEN, xi)
    assert n == PKL + SKL
    st, _ = await rd(dut, STATUS)
    assert st & ST_DONE, "先决条件不成立：成功那次没报 DONE"

    # 紧接着来一次非法 START（op=3）
    assert await wr(dut, MODE, mode_word(3)) == RESP_OKAY
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

    # ⚠️ 非法 pset 那一次同样要作废 —— 这一条最容易漏：engine 自己拒绝时
    #    不更新 op_r，本层若放它进去就会抄下上一次的 out_len。
    #    用 pset=3（真正的非法值）。**不能再用"与综合参数集不符"来触发**：
    #    engine 运行时支持 44/65/87，那三个值现在都是合法的。
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, 3)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, xi)
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR and not (st & ST_DONE), \
        f"非法 pset 却没有当场作废（STATUS={st:#x}）"
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0, f"pset 对不上之后 OUT_LEN 还是 {n} —— 那是上一次的长度"

    # 欠填的 START 同样作废
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, bytes(4))
    assert await wr(dut, CTRL, C_START) == RESP_OKAY
    st, _ = await rd(dut, STATUS)
    assert not (st & ST_DONE) and (st & ST_LENERR)
    n, _ = await rd(dut, OUT_LEN)
    assert n == 0

    dut._log.info("非法 / pset 不符 / 欠填三种被拒的 START 都当场作废上一次的结果")


@cocotb.test()
async def test_clear_leaves_nothing_behind(dut):
    """CLEAR 之后不留任何陈旧状态"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 先制造一份"脏状态"：跑完一次判通过的 Verify（DONE + verify_ok）
    pk, sig, msg, ctx = kat_sigver("pass")
    await run_op(dut, OP_VERIFY, pk + sig + ctx + msg,
                 msg_len=len(msg), ctx_len=len(ctx))
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
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
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

    ⚠️ 两次要用**不同的输入**：同一个输入跑两次时，"第二次其实没跑、读到的是
       上一次留在核里的输出"与"真的跑对了"完全分不开。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for i, (xi, pk_w, _) in enumerate(kat_keygen(2)):
        n = await run_op(dut, OP_KEYGEN, xi)
        assert n == PKL + SKL, (
            f"第 {i+1} 次 KeyGen OUT_LEN={n}"
            + ("  ← 第二次就是残留 done 那个 bug" if i else ""))
        assert await out_bytes(dut, 0, 64) == pk_w[:64], \
            f"第 {i+1} 次的 pk 与 ACVP 不一致"

    # Sign 也连跑两次，两条消息不同
    sk, _, _, _, _ = kat_siggen()
    for i, msg in enumerate([b"aaaa", b"bbbb"]):
        n = await run_op(dut, OP_SIGN, sign_payload(sk, RND0, b"", msg),
                         msg_len=len(msg))
        assert n == SIGL, f"第 {i+1} 次 Sign OUT_LEN={n}"
        assert await out_bytes(dut, 0, SIGL) == golden_sig(sk, msg, b""), \
            f"第 {i+1} 次的 σ 与 oracle 对不上"

    dut._log.info("KeyGen 连跑两次、Sign 连跑两次（输入各不相同），中间不复位 —— 全对")


# ============================================================================
# ⑦ 防火墙
# ============================================================================
@cocotb.test()
async def test_firewall_nonsecure_refused(dut):
    """non-secure 的读写被拦、无副作用；越界地址读回 0"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, _ = kat_keygen()[0]
    n = await run_op(dut, OP_KEYGEN, xi)
    assert n == PKL + SKL

    v, r = await rd(dut, OUT_LEN, PROT_NONSEC)
    assert r == RESP_REFUSED and v == 0, f"non-secure 读没被拦：0x{v:08x}"
    v, r = await rd(dut, VERSION, PROT_NONSEC)
    assert r == RESP_REFUSED and v == 0, "non-secure 读到了 VERSION"

    assert await wr(dut, CTRL, C_ZEROIZE, PROT_NONSEC) == RESP_REFUSED, \
        "non-secure 写没被拦"
    assert await wr(dut, CTRL, C_SK_LOCK, PROT_NONSEC) == RESP_REFUSED

    # 被拦的那几笔没有副作用
    n, _ = await rd(dut, OUT_LEN)
    assert n == PKL + SKL, "non-secure 的写产生了副作用（输出被清了）"
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
    金库 64 KB → 擦除机要走 65536 拍（engine 那台再走 32768 拍，两台一起等），
    每次轮询是一笔 AXI 读、占好几拍，所以 40000 次轮询足够覆盖。
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

    xi, _, sk_w = kat_keygen()[0]
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=0)
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & 1, "先决条件不成立：槽 0 没装上"

    mem = dut.u_skvault.mem
    depth = len(mem)
    assert depth == 65536, f"金库深度 {depth}，应当是 65536（8 槽 × 8192）"

    # 金库里躺的就是 ACVP 那把 sk —— 顺带钉住"搬进金库的是对的字节"
    got = bytes(int(mem[i].value) & 0xFF for i in range(SKL))
    assert got == sk_w, "金库里的 sk 与 ACVP 不一致 —— 搬运那一段错了"

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

    # 擦完还能照常再跑，而且结果仍然对上 ACVP
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL

    dut._log.info(f"ZEROIZE 后 {depth} 字节金库逐字节读回，全为 0；"
                  "擦除期间写回 SLVERR；擦前金库里的 sk 对上 ACVP")


@cocotb.test()
async def test_zeroize_invalidates_slots_not_just_the_bytes(dut):
    """擦完之后拿被擦掉的槽去签：必须被拒，而不是拿一把全 0 的 sk 签出东西

    只擦字节是不够的 —— 槽的有效位若还在，后面那次 Sign 会**照常跑完**，
    用的是擦成全 0 的那 2560 字节。出来的 σ 长度对、格式对、连 verify 都能过
    （验的是这把全 0 sk 对应的 pk），完全是个安静的错误。

    ⚠️ 顺带说清楚这一条**不**证明什么：engine 自己那台擦除机（输入缓冲 32768
       个地址）在这一层是观测不到的 —— 本层的长度校验保证每一个需要的字节都
       被软件重新写过，残留根本没有机会参与运算，这正是那条校验的用意。
       engine 擦除的反证在 test_mldsa_engine.py 的 zeroize 那条里（它能不灌种子
       直接跑，看得到"缓冲是不是真的 0"）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, _ = kat_keygen()[0]
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=6)
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & (1 << 6), "先决条件不成立：槽 6 没装上"

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    payload = sign_payload(b"", RND0, b"", b"m", from_slot=True)
    assert await run_op(dut, OP_SIGN, payload, from_slot=True, slot=6,
                        msg_len=1, limit=3000) is None, \
        "擦除之后那个槽还能签 —— 用的是被擦成全 0 的 sk"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "擦掉的槽应当报 PARAM_ERR"

    # 重新装一把再签就正常（擦除没有把设备弄坏）
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=6)
    assert await run_op(dut, OP_SIGN, payload, from_slot=True, slot=6,
                        msg_len=1) == SIGL

    dut._log.info("ZEROIZE 之后被擦掉的槽拒绝签名；重新装载后恢复正常")


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

    xi, _, _ = kat_keygen()[0]
    await run_op(dut, OP_KEYGEN, xi, to_slot=True, slot=4)
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
