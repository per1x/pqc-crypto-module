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

C_START, C_CLEAR, C_ZEROIZE = 1 << 0, 1 << 1, 1 << 2

ST_BUSY, ST_DONE, ST_VOK = 1 << 0, 1 << 1, 1 << 2
ST_PARAMERR, ST_LENERR = 1 << 3, 1 << 4
ST_TAMPER, ST_WIPING = 1 << 5, 1 << 6

OP_KEYGEN, OP_SIGN, OP_VERIFY = 0, 1, 2
# 批 2：槽位 ABI 已删（V-02/V-04/V-05/V-06）。MODE 只剩 OP/PSET/CHAIN/SEED_STAGED。
# CHAIN：本次 Sign 先从暂存的 ξ 展开 sk，再签，算完无条件擦 —— 替代 SK_FROM_SLOT，
# 差别在于 sk 是这条命令自己现展开的，不是上一条命令留下的。
M_CHAIN = 1 << 4

# KEYSTAT 现在只有与密钥无关的健康位
KS_EXP_WIPING = 1 << 0
KS_SEED_LOCK  = 1 << 1

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


def sign_payload(sk, rnd, ctx, msg, *, chain=False):
    """软件该往 IN_DATA 里灌什么 —— 走金库时不送 sk"""
    return (b"" if chain else sk) + rnd + ctx + msg


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



async def wait_exp_idle(dut, limit=40000):
    """等展开区擦完再发下一条命令。

    ⚠️ **批 2 新增的软件纪律，不是测试技巧。** 展开区在每次运算之后由硬件
    无条件擦（§7.2 条件①），期间 START 会被拒（置 PARAM_ERR 并作废上一次的
    结果）。daemon 那侧也是同样的等法。
    """
    for _ in range(limit):
        ks, _ = await rd(dut, KEYSTAT)
        if not (ks & KS_EXP_WIPING):
            return
    raise AssertionError("展开区一直在擦，EXP_WIPING 不落")

def mode_word(op, pset=None, *, chain=False, staged=False):
    if pset is None:
        pset = PSET
    # 链式必然走暂存口（RTL 的 chain_gate_ok 要求它），所以这里让 chain
    # 蕴含 staged —— 少一个每个调用点都要记得写的东西。
    return (op | (pset << 2) | (M_CHAIN if chain else 0)
            | (M_SEED_STAGED if (staged or chain) else 0))


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


async def run_op(dut, op, payload, *, pset=None, chain=False, staged=False,
                 msg_len=0, ctx_len=0, limit=40_000, gap_ns=2_000):
    """一次完整调用：设参数 → 清 → 灌字节 → 启动 → 等完成。返回 OUT_LEN

    ⚠️ 顺序是 MODE 在最前 —— 写 MODE 会清 IN_PTR（排布依赖 MODE，见 RTL 文件头）。
    """
    await wait_exp_idle(dut)
    assert await wr(dut, MODE, mode_word(op, pset, chain=chain,
                                        staged=staged)) == RESP_OKAY
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
async def test_verify_after_a_rejected_bigger_pset(dut):
    """**先判否一条大参数集的签名，再验一条本该通过的小参数集签名**（不复位）

    ⚠️ 上面那条 `test_verify_cross_pset_without_reset` 挑记录时写着
    `result == "pass"` —— 六步全是**通过**的签名。这一条补上它漏掉的那半：
    序列里夹一次**判否**。板上跑的是完整的 sigver 向量集（45 条里 36 条是
    `fail`），所以"上一次 Verify 判了否"才是板上的常态，而仿真里从来没有过。

    为什么这半边关键：verify.v 的判定是 `ctilde_p == ctilde` 的**整 512 位**
    比较，而两个寄存器**每次只写低 ctb 字节**（44/65/87 → 32/48/64）。于是
    高位字节是**上一次运算**留下的：

        · 上一次是 87 且判**通过** ⇒ 两边高位相等 ⇒ 下一条 44/65 照样对；
        · 上一次是 87 且判**否**   ⇒ 两边高位不等 ⇒ 之后每一条 44/65 的
          Verify 都被判否，**直到又来一条 87 把 64 字节整个覆盖掉**。
        · 87 自己永远对：ctb=64，64 字节全写，没有"高位"。

    这正是板上那个 100%/0%：44/65 全红、87 全绿，与温度和时序都无关。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)                      # ← 全用例只此一次

    async def verify(ps, want, note):
        alg = {0: "ML-DSA-44", 1: "ML-DSA-65", 2: "ML-DSA-87"}[ps]
        r = _kat_any_alg(SIGVER_KAT, alg,
                         lambda x: x.get("result") == want, _short)[0]
        pk = bytes.fromhex(r["pk"]);  sig = bytes.fromhex(r["sig"])
        msg = bytes.fromhex(r["msg"]); ctx = bytes.fromhex(r.get("context", ""))
        assert len(pk) == PK[ps] and len(sig) == SIG[ps]
        n = await run_op(dut, OP_VERIFY, pk + sig + ctx + msg, pset=ps,
                         msg_len=len(msg), ctx_len=len(ctx))
        assert n == 0, f"Verify 不该有输出字节，OUT_LEN={n}"
        st, _ = await rd(dut, STATUS)
        got = bool(st & ST_VOK)
        assert got == (want == "pass"), (
            f"{note}：{alg}（tcId={r.get('tcid')}）判定错了 —— 期望 {want}，"
            f"verify_ok={got}（STATUS={st:#010x}）")
        dut._log.info(f"  {note}：{alg} 判 {want}，对")

    # ① 大参数集判否 —— 这一步本身是对的，它只是把状态弄脏
    await verify(2, "fail", "第 1 步（把高位字节弄脏）")
    # ② 小参数集的**合法**签名：不该受上一步影响
    await verify(0, "pass", "第 2 步（44，上一步判否之后）")
    await verify(1, "pass", "第 3 步（65，上一步判否之后）")
    # ③ 反过来也要成立：判否之后再判否，仍然是否（不能因为脏字节"碰巧"相等）
    await verify(0, "fail", "第 4 步（44 判否）")
    await verify(2, "pass", "第 5 步（87，任何时候都不受影响）")


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
async def test_chained_sign_from_staged_xi(dut):
    """**批 2 的落点**：一条命令里先从暂存 ξ 展开 sk、再签、算完就擦

    替代原来的 `test_sk_stays_on_chip_and_slot_signs_correctly`。那条证的是
    "sk 留在槽里、Sign 按槽取"——正是登记表 V-02 要消掉的性质。

    现在的判据（判据用 ACVP，比"两条路一致"更硬）：
      · 同一份 ξ，链式签出来的 σ 与官方向量逐字节相同；
      · 整条链里 OUT_LEN 只有 σ 那么长，sk 一个字节都读不到。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    sk, msg, ctx, rnd, sig_w = kat_siggen()

    # ξ 与 ACVP siggen 那条用例的私钥同源：先用官方 ξ 做一次 KeyGen，
    # 确认展开出来的 sk 与向量一致，再用同一份 ξ 走链式签名。
    xi, pk_w, sk_w = kat_keygen()[0]

    await stage_xi(dut, xi)
    n = await run_op(dut, OP_KEYGEN, b"", staged=True)
    assert n == PKL + SKL, f"暂存 ξ 的 KeyGen OUT_LEN={n}"
    got_sk = await out_bytes(dut, PKL, SKL)
    assert got_sk == sk_w, "暂存 ξ 展开出来的 sk 与 ACVP 不一致"

    # 链式签名：同一份 ξ 重送，PL 现展开 sk 再签
    await stage_xi(dut, xi)
    n = await run_op(dut, OP_SIGN,
                     sign_payload(b"", RND0, ctx, msg, chain=True),
                     chain=True, msg_len=len(msg), ctx_len=len(ctx),
                     limit=400_000)
    assert n == SIGL, f"链式签名 OUT_LEN={n}，应当只有 σ 的 {SIGL} 字节"
    sig_chain = await out_bytes(dut, 0, SIGL)

    # 自洽判据：同一份 ξ 自送 sk 签出来的 σ 必须一样
    n2 = await run_op(dut, OP_SIGN,
                      sign_payload(sk_w, RND0, ctx, msg),
                      msg_len=len(msg), ctx_len=len(ctx), limit=400_000)
    assert n2 == SIGL
    sig_ref = await out_bytes(dut, 0, SIGL)
    assert sig_chain == sig_ref, \
        "链式展开签出来的 σ 与自送 sk 签出来的不一样 —— 展开或搬运错了"
    dut._log.info(f"{ALG} 链式签名：sk 由同一条命令现展开、没出总线，σ 一致")


@cocotb.test()
async def test_chain_without_staged_xi_refused(dut):
    """链式签名**没有备好的 ξ** 时必须当场被拒

    替代原来的"空槽/参数集不符"那条 —— 槽没有了，取而代之的失败形态是
    "要链式展开，但暂存口里没有种子"。

    ⚠️ 这条不挡住的后果不是报错：展开相位会退回"从 IN_DATA 读 ξ"那条路，
    而软件送的字节落在 sw_base 之后，那 32 个字节是残留（冷启动全 0）。
    结果是**静默地展开出另一把密钥**，签出来的 σ 完全合法，只是对不上
    任何人的公钥。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    payload = sign_payload(b"", RND0, b"", b"m", chain=True)
    assert await run_op(dut, OP_SIGN, payload, chain=True,
                        msg_len=1, limit=3000) is None, \
        "没有备好的 ξ，链式签名居然跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_PARAMERR, "被拒了却没点亮 PARAM_ERR"
    dut._log.info("链式签名没备好 ξ 时当场被拒，不会静默展开出另一把密钥")


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

    # 链式那条路要先备好 ξ —— 而且必须是**同一份**，两条路才可比
    await run_op(dut, OP_KEYGEN, xi)

    for chain in (False, True):
        for ctx in (b"", b"\xAA\xBB\xCC"):
            label = ("链式展开" if chain else "自送 sk") + \
                    ("、ctx 非空" if ctx else "、ctx 空")
            full = sign_payload(sk, rnd, ctx, msg, chain=chain)
            want_len = 32 + len(ctx) + len(msg) + (0 if chain else SKL)
            assert len(full) == want_len, f"{label}：用例自己的长度就不对"

            # 少一个字节
            if chain:
                await stage_xi(dut, xi)     # 一份 ξ 只用一次，每次重送
            r = await run_op(dut, OP_SIGN, full[:-1], chain=chain,
                             msg_len=len(msg), ctx_len=len(ctx),
                             limit=3000)
            assert r is None, f"{label}：少一个字节居然跑起来了"
            st, _ = await rd(dut, STATUS)
            assert st & ST_LENERR, f"{label}：少一个字节应当置 LEN_ERR（{st:#x}）"

            # 正好喂满
            if chain:
                await stage_xi(dut, xi)
            n = await run_op(dut, OP_SIGN, full, chain=chain,
                             msg_len=len(msg), ctx_len=len(ctx))
            if n != SIGL:
                st, _ = await rd(dut, STATUS)
                ss, _ = await rd(dut, SEED_STAT)
                raise AssertionError(
                    f"{label}：喂满之后 OUT_LEN={n} STATUS={st:#x} SEED_STAT={ss:#x}")
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

    # ---- 8193：字节喂得满满的，仍然必须被拒 ----
    # 用链式省掉几千笔 sk 的写；链式要先备好 ξ（否则会被 chain_gate_ok 拒掉，
    # 那样这条用例就测不到 msg_len 那道门了）。
    await stage_xi(dut, xi)
    over = bytes((i * 31 + 7) & 0xFF for i in range(MSGMAX + 1))
    assert await wr(dut, MODE, mode_word(OP_SIGN, chain=True)) == RESP_OKAY
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
    assert await wr(dut, MODE, mode_word(OP_SIGN, chain=True)) == RESP_OKAY
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
async def test_mldsa_slot_abi_is_gone(dut):
    """槽位 ABI 与 SK_LOCK 在**寄存器面上**确实没有了（V-02/V-04/V-05/V-06）

    替代原来的 `test_sk_lock_is_one_way`。那条证的是"闩上之后强制走金库、
    而且清不掉"——闩守在与"密钥归 TEE"相反的方向，随 V-04 删除。

    判据不是"软件不再用它"，而是**寄存器读回来就是没有**：
    只把软件改成不用，硬件那条路还在，任何能发 AXI 事务的人照样能用。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # 往原来是 SK_FROM_SLOT / SLOT[9:6] 的位置写满 1
    assert await wr(dut, MODE, OP_SIGN | (PSET << 2)
                    | (1 << 5) | (0xF << 6)) == RESP_OKAY
    m, _ = await rd(dut, MODE)
    assert (m >> 5) & 0x1F == 0, \
        f"MODE 回读 0x{m:08x} —— 槽号/SK_FROM_SLOT 那几位还留着值"

    # 原来的 CTRL[4] = SK_LOCK：写它不该再有任何效果
    assert await wr(dut, CTRL, 1 << 4) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert (ks >> 2) == 0, \
        f"KEYSTAT = 0x{ks:08x} —— 除了 EXP_WIPING/SEED_LOCK 还有别的位"
    dut._log.info("槽位 ABI 与 SK_LOCK 在寄存器面上确实没有了")


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
        # 槽号非法那两条已删除 —— 槽位 ABI 没有了，也就没有"槽号越界"这回事。
        # 它们由 test_mldsa_slot_abi_is_gone 接手：写那几位不留值。
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
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
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
    assert await wr(dut, CTRL, 1 << 4, PROT_NONSEC) == RESP_REFUSED

    # 被拦的那几笔没有副作用
    n, _ = await rd(dut, OUT_LEN)
    assert n == PKL + SKL, "non-secure 的写产生了副作用（输出被清了）"
    ks, _ = await rd(dut, KEYSTAT)
    assert not (ks & KS_SEED_LOCK), "non-secure 居然把闩锁置上了"

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
    展开区 8 KB → 擦除机走 8192 拍（engine 那台再走 32768 拍，两台一起等），
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
      · **全量残留**：把 8192 个地址按固定步长撒满非零，证明擦的是整个
        地址空间，不是"用到的那一段"（只擦用过的那段是个很容易犯、
        而且看起来一样有效的错）。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    mem = dut.u_expand.mem
    depth = len(mem)
    assert depth == 8192, \
        f"展开区深度 {depth}，应当是 8192（批 2：8 槽 × 8 KB 的金库降级成一块 8 KB 的匿名展开区）"

    # ⚠️ 原来这里先跑一次"存槽的 KeyGen"，再断言金库里躺的就是 ACVP 那把 sk。
    # 批 2 之后**没有"存起来"这回事**了：展开区只在链式运算的两个相位之间
    # 活着，S_FIN 无条件擦。所以那个先决条件在这里造不出来，也不该造 ——
    # "展开相位写进去的是对的字节"由 test_chained_sign_from_staged_xi 钉着
    # （判据是 σ 对得上，比逐字节比对更硬）。
    #
    # 这条用例剩下的任务是**ZEROIZE 真的逐地址擦**，用人工残留就够，
    # 而且比真 sk 更强：真 sk 只占前 SKL 个字节，人工残留铺满整个地址空间。

    # 全量残留：按步长撒满整个地址空间（逐个写太慢，
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
    assert (ks >> 2) == 0, f"擦除之后 KEYSTAT = 0x{ks:08x}，只该剩健康位"

    # 擦完还能照常再跑
    xi, _, _ = kat_keygen()[0]
    assert await run_op(dut, OP_KEYGEN, xi) == PKL + SKL

    dut._log.info(f"ZEROIZE 后 {depth} 字节展开区逐字节读回，全为 0；"
                  "擦除期间写回 SLVERR；擦完还能照常再跑")


@cocotb.test()
async def test_zeroize_also_kills_the_staged_seed_path(dut):
    """ZEROIZE 之后，链式签名**没有可用的种子**，于是当场被拒

    替代原来的 `test_zeroize_invalidates_slots_not_just_the_bytes`。那条证的是
    "擦字节还不够，槽的有效位也要作废"——槽没有了，那个失败形态也就没有了。

    换成对等的一条：批 2 里"下一次运算能不能用上一次的东西"这个问题的载体
    从槽变成了**暂存的种子**。ZEROIZE 必须把它一起作废，否则擦完之后
    链式签名照样跑得起来 —— 用的是擦除前那份 ξ。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, _ = kat_keygen()[0]
    await stage_xi(dut, xi)
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY, "先决条件不成立：ξ 没备好"

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    await _wait_wipe(dut)

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0x0F) == 0 and not (ss & SS_READY), \
        f"zeroize 之后暂存的 ξ 还在（SEED_STAT={ss:#x}）"

    payload = sign_payload(b"", RND0, b"", b"m", chain=True)
    assert await run_op(dut, OP_SIGN, payload, chain=True,
                        msg_len=1, limit=3000) is None, \
        "擦除之后链式签名还跑得起来 —— 用的是擦除前那份 ξ"
    dut._log.info("ZEROIZE 把暂存的 ξ 一起作废，链式签名当场被拒")


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
    await run_op(dut, OP_KEYGEN, xi)
    mem = dut.u_expand.mem
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

    # 逐时钟等擦完：上限必须大于擦除拍数本身（展开区 8 KB → 8192 拍）
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


# ============================================================================
# 安全世界暂存的种子（CODE-1 的 PL 侧落点）—— 与 mlkem_axi 同一套判据
# ============================================================================
# 四条，缺一条这个口就是装饰品（RTL 文件头逐条写了理由）：
#   ① 暂存口送的 ξ 与灌 IN_DATA 的同一份 ξ，pk‖sk 逐字节相同（对 ACVP）；
#   ② 非安全事务写种子口进不去，字计数一步不动；
#   ③ 读种子口恒 0，整个寄存器窗口都扫不出种子字节；
#   ④ 用一次即作废，且**在 START 那一刻**就清（不是跑完才清）。
SEED_DATA, SEED_STAT = 0x34, 0x38
C_SEED_LOCK, C_SEED_CLR = 1 << 5, 1 << 6
M_SEED_STAGED = 1 << 10
ST_SEED_ERR = 1 << 7
SS_READY, SS_LOCK, SS_OVF = 1 << 8, 1 << 9, 1 << 11


async def stage_xi(dut, xi: bytes, prot=PROT_SECURE):
    """把 32 字节 ξ 按 8 个小端 32 位字写进暂存口"""
    assert len(xi) == 32
    for i in range(8):
        w = int.from_bytes(xi[4 * i:4 * i + 4], "little")
        r = await wr(dut, SEED_DATA, w, prot=prot)
        if prot != PROT_SECURE:
            return r
    return RESP_OKAY


@cocotb.test()
async def test_staged_xi_matches_acvp(dut):
    """①：暂存口送的 ξ 跑出来的 pk‖sk 逐字节对上 ACVP

    判据直接用官方向量而不是"和 IN_DATA 那条路比"—— 更强：它同时证明了
    暂存口接对了、字节序对了、以及整条链路仍然是那条已经过硅的链路。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, pk_w, sk_w = kat_keygen()[0]

    await stage_xi(dut, xi)
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 8 and (ss & SS_READY), \
        f"8 个字写完了 SEED_STAT=0x{ss:08x}"

    # **一个字节都不写 IN_DATA**
    assert await wr(dut, MODE, mode_word(OP_KEYGEN) | M_SEED_STAGED) == RESP_OKAY
    assert await wr(dut, MSG_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTX_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    assert await start_and_wait(dut), "走暂存 ξ 的 KeyGen 被拒了"

    n, _ = await rd(dut, OUT_LEN)
    assert n == PKL + SKL, f"OUT_LEN={n}，应当是 {PKL}+{SKL}"
    got_pk = await out_bytes(dut, 0, PKL)
    assert got_pk == pk_w, diff(got_pk, pk_w, f"{ALG} pk（暂存 ξ）与 ACVP")
    got_sk = await out_bytes(dut, PKL, SKL)
    assert got_sk == sk_w, diff(got_sk, sk_w, f"{ALG} sk（暂存 ξ）与 ACVP")

    dut._log.info("暂存口送的 ξ：pk‖sk 逐字节对上 ACVP")


@cocotb.test()
async def test_xi_port_refuses_nonsecure(dut):
    """②：非安全事务写不进种子口 —— 两种 SECURE_ONLY 形态下都不行

    形态差异与 mlkem_axi 那条完全相同（那里写了完整理由）：
    SECURE_ONLY=1 时防火墙先 RAZ/WI 掉，SECURE_ONLY=0 时才轮到种子口自己
    那道门回 SLVERR。**共同要证的是字计数一步不动。**
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    secure_only = int(os.environ.get("PARAM_SECURE_ONLY", "1"))

    for i in range(3):
        r = await wr(dut, SEED_DATA, 0xCAFE0000 + i, prot=PROT_NONSEC)
        # ⚠️ 两种形态都必须 OKAY：posted 写的 SLVERR 会以 SError 打回来，
        # 内核只能 panic（从 EL3 发的那一笔更糟，EL3 里没有处理器）。
        # 完整理由见 mlkem_axi.py 同名用例与 mlkem_axi.v 文件头①。
        assert r == RESP_OKAY, \
            f"第 {i} 笔非安全写种子口回了 {r}；必须是 OKAY（丢弃 + 计数）"

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 0, "被拒的写居然把字计数推上去了 —— 种子口漏了"
    if secure_only:
        assert (ss >> 16) == 0, "被防火墙拦下的访问不该计进种子口的计数"
    else:
        assert (ss >> 16) == 3, f"种子口被拒计数 = {ss >> 16}，应当是 3"

    # 拒绝没有副作用：之后仍然能正常 staging
    await stage_xi(dut, bytes([0x3C] * 32))
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY, "被拒的写留下了副作用"

    dut._log.info("SECURE_ONLY=%d：非安全写种子口进不去", secure_only)


@cocotb.test()
async def test_xi_port_never_reads_back(dut):
    """③：读种子口恒 0，整个寄存器窗口都扫不出 ξ 的任何一个字"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi = bytes((0x40 + i) & 0xFF for i in range(32))
    await stage_xi(dut, xi)

    for _ in range(4):
        v, resp = await rd(dut, SEED_DATA)
        assert resp == RESP_OKAY and v == 0, \
            f"读 SEED_DATA 得到 0x{v:08x} —— 种子口不该有任何读回路径"

    words = {int.from_bytes(xi[4 * i:4 * i + 4], "little") for i in range(8)}
    for off in range(0x00, 0x40, 4):
        # OUT_DATA 跳过：engine 的输出口，这一刻没跑过运算，读出来是 X。
        # 它装的是 pk/sig，与种子暂存是两块存储（sk 那条路另有用例把关）。
        if off == OUT_DATA:
            continue
        v, _ = await rd(dut, off)
        assert v not in words, \
            f"寄存器 0x{off:02x} 读出 0x{v:08x}，那是 ξ 的一个字"

    dut._log.info("整个寄存器窗口扫过：ξ 的 8 个字一个都读不到")


@cocotb.test()
async def test_staged_xi_consumed_at_start(dut):
    """④：用一次即作废，且**在 START 那一刻**就清

    "跑完才清"与"START 就清"的差别不是洁癖：清在 S_FIN 的话，一次半途被
    zeroize 或被参数错打断的运行会把 ξ **留在暂存里**，而安全世界那边已经
    把它忘了。判据只能在 BUSY 期间查。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, _, _ = kat_keygen()[0]
    await stage_xi(dut, xi)

    assert await wr(dut, MODE, mode_word(OP_KEYGEN) | M_SEED_STAGED) == RESP_OKAY
    assert await wr(dut, MSG_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTX_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    assert await wr(dut, CTRL, C_START) == RESP_OKAY

    st, _ = await rd(dut, STATUS)
    assert st & ST_BUSY, "START 之后立刻查却已经不 BUSY —— 判据失效"
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 0 and not (ss & SS_READY), \
        f"运行途中 SEED_STAT=0x{ss:08x} —— 暂存不是在 START 那一刻清的"

    for _ in range(40_000):
        st, _ = await rd(dut, STATUS)
        if st & ST_DONE:
            break
        await Timer(2_000, unit="ns")
    else:
        raise AssertionError("一直没完成")

    # 第二次：没有备好的 ξ → 当场拒绝并点亮 SEED_ERR
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    ok = await start_and_wait(dut, limit=200, gap_ns=0)
    assert ok is False, "同一份暂存 ξ 居然生出了第二把密钥"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR, "ξ 没备好却没报 SEED_ERR"
    assert st & ST_PARAMERR, "SEED_ERR 时总括位 PARAM_ERR 也该亮"

    dut._log.info("暂存 ξ 在 START 当场作废，第二次 START 报 SEED_ERR")


@cocotb.test()
async def test_xi_seed_clr_partial_and_overfull(dut):
    """半份 ξ 不许开跑；收满后多写回 SLVERR；SEED_CLR 能作废"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for i in range(4):
        assert await wr(dut, SEED_DATA, 0x01020304 + i) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 4 and not (ss & SS_READY)

    assert await wr(dut, MODE, mode_word(OP_KEYGEN) | M_SEED_STAGED) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    ok = await start_and_wait(dut, limit=200, gap_ns=0)
    assert ok is False, "半份 ξ 居然跑起来了"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR

    for i in range(4):
        assert await wr(dut, SEED_DATA, 0x0A0B0C0D + i) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY
    assert await wr(dut, SEED_DATA, 0xFFFFFFFF) == RESP_OKAY, \
        "收满之后的多余写必须回 OKAY（丢弃），不能回总线错误"
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_OVF, "收满之后多写没有留下 OVF 痕迹"
    assert (ss & 0xF) == 8, "多余的那一笔居然把字计数推过 8 了"

    assert await wr(dut, CTRL, C_SEED_CLR) == RESP_OKAY
    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 0 and not (ss & SS_READY), "SEED_CLR 没清掉暂存"

    dut._log.info("半份 ξ 被拒；收满后多写 SLVERR；SEED_CLR 生效")


@cocotb.test()
async def test_xi_seed_lock_independent_of_sk_lock(dut):
    """SEED_LOCK 一次性、zeroize 撤不回，且**不连动 SK_LOCK**

    最后半条是有意验的：SK_LOCK 与 SEED_LOCK 守的方向相反
    （FINAL-PLAN §7 V-04 要删掉前者），两者必须互相独立。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, pk_w, _ = kat_keygen()[0]

    assert await wr(dut, CTRL, C_SEED_LOCK) == RESP_OKAY
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & KS_SEED_LOCK, "SEED_LOCK 没置上"
    # 原来这里还断言"SEED_LOCK 没顺手把 SK_LOCK 也置上"。SK_LOCK 已随 V-04
    # 删除，那条断言的对象没有了 —— 换成"KEYSTAT 里除了健康位没有别的"。
    assert (ks >> 2) == 0, \
        "SEED_LOCK 顺手把 SK_LOCK 也置上了 —— 两把闩方向相反，不能连动"

    # 闩上之后：**不带 SEED_STAGED 的 MODE 字**也必须走暂存口
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill(dut, xi)                      # 灌 IN_DATA 的 ξ 应当被无视
    ok = await start_and_wait(dut, limit=200, gap_ns=0)
    assert ok is False, "闩锁之后居然还能退回 IN_DATA 那条老路"
    st, _ = await rd(dut, STATUS)
    assert st & ST_SEED_ERR

    # 备好就能跑，且结果仍对 ACVP
    await stage_xi(dut, xi)
    assert await wr(dut, MODE, mode_word(OP_KEYGEN)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    assert await start_and_wait(dut), "备好 ξ 之后仍然跑不起来"
    got_pk = await out_bytes(dut, 0, PKL)
    assert got_pk == pk_w, diff(got_pk, pk_w, "闩锁后走暂存口的 pk")

    # zeroize 撤不回
    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    for _ in range(40_000):
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            break
        await Timer(2_000, unit="ns")
    ks, _ = await rd(dut, KEYSTAT)
    assert ks & KS_SEED_LOCK, "zeroize 把 SEED_LOCK 清掉了"

    dut._log.info("SEED_LOCK 一次性、撤不回、与 SK_LOCK 互相独立")


@cocotb.test()
async def test_zeroize_wipes_staged_xi(dut):
    """zeroize 要把暂存的 ξ 一起擦掉 —— 它也是秘密"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    await stage_xi(dut, bytes([0x99] * 32))
    ss, _ = await rd(dut, SEED_STAT)
    assert ss & SS_READY

    assert await wr(dut, CTRL, C_ZEROIZE) == RESP_OKAY
    for _ in range(40_000):
        st, _ = await rd(dut, STATUS)
        if not (st & ST_WIPING):
            break
        await Timer(2_000, unit="ns")
    else:
        raise AssertionError("WIPING 一直不落")

    ss, _ = await rd(dut, SEED_STAT)
    assert (ss & 0xF) == 0 and not (ss & SS_READY), "zeroize 之后暂存的 ξ 还在"

    assert await wr(dut, MODE, mode_word(OP_KEYGEN) | M_SEED_STAGED) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    ok = await start_and_wait(dut, limit=200, gap_ns=0)
    assert ok is False, "zeroize 之后居然还能拿残留的 ξ 生成密钥"

    dut._log.info("zeroize 把暂存的 ξ 一起擦掉了")


@cocotb.test()
async def test_mldsa_mode_reads_back_all_fields(dut):
    """MODE 整字回读（DOC-3）—— 原来 SK_TO_SLOT/SK_FROM_SLOT/SLOT 读不回来"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for word in (mode_word(OP_SIGN, chain=True),
                 mode_word(OP_KEYGEN) | M_SEED_STAGED,
                 mode_word(OP_VERIFY)):
        assert await wr(dut, MODE, word) == RESP_OKAY
        got, _ = await rd(dut, MODE)
        assert got == word, f"MODE 回读 0x{got:03x}，写进去的是 0x{word:03x}"

    dut._log.info("MODE 的 11 个位全部可回读")


# ============================================================================
# 4 字节打包口（D11）
# ============================================================================
IN_DATA4, OUT_DATA4 = 0x40, 0x44


async def fill_packed(dut, payload: bytes):
    """走 IN_DATA4 灌字节，尾巴（不足 4 的那一截）退回逐字节"""
    i = 0
    while i + 4 <= len(payload):
        w = int.from_bytes(payload[i:i + 4], "little")
        assert await wr(dut, IN_DATA4, w) == RESP_OKAY
        i += 4
    while i < len(payload):
        assert await wr(dut, IN_DATA, payload[i]) == RESP_OKAY
        i += 1


async def out_bytes_packed(dut, first, count):
    """走 OUT_DATA4 取字节，尾巴退回逐字节"""
    assert await wr(dut, OUT_PTR, first) == RESP_OKAY
    out = bytearray()
    while len(out) + 4 <= count:
        d, _ = await rd(dut, OUT_DATA4)
        out += (d & 0xFFFFFFFF).to_bytes(4, "little")
    while len(out) < count:
        d, _ = await rd(dut, OUT_DATA)
        out.append(d & 0xFF)
    return bytes(out)


@cocotb.test()
async def test_packed_io_matches_acvp(dut):
    """打包 I/O 跑 KeyGen 与 Sign，结果**逐字节对上 ACVP**

    判据直接钉在官方向量上而不是"与逐字节那趟一致"：ML-DSA 这一层的输出
    跨 pk/sk 两段，段边界正是最容易错一个字节的地方（见 mldsa_axi.v 里
    out_addr 那一大段），而 ACVP 的 pk‖sk 把两段的边界一起钉死了。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    # ---- KeyGen：输入 32 字节 ξ，输出跨 pk/sk 段边界 ----
    xi, pk_w, sk_w = kat_keygen()[0]
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, None)) == RESP_OKAY
    assert await wr(dut, MSG_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTX_LEN, 0) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill_packed(dut, xi)
    p, _ = await rd(dut, IN_PTR)
    assert p == len(xi), f"打包写之后 IN_PTR = {p}，应当是 {len(xi)}"
    assert await start_and_wait(dut, 40_000, 2_000), "打包 KeyGen 没跑完"
    n, _ = await rd(dut, OUT_LEN)
    assert n == PKL + SKL, f"OUT_LEN={n}"

    got_pk = await out_bytes_packed(dut, 0, PKL)
    assert got_pk == pk_w, diff(got_pk, pk_w, f"{ALG} 打包读 pk 与 ACVP")
    got_sk = await out_bytes_packed(dut, PKL, SKL)
    assert got_sk == sk_w, diff(got_sk, sk_w, f"{ALG} 打包读 sk 与 ACVP")

    # **段边界那一段单独再读一次**：跨过 PKL 的那 8 个字节。
    # 上面两趟各自停在边界上，跨过去这一趟才真的把选择器翻面那一拍走到。
    across = await out_bytes_packed(dut, PKL - 4, 8)
    assert across == (pk_w + sk_w)[PKL - 4:PKL + 4], \
        "跨 pk/sk 段边界的打包读错了 —— 正是 out_addr 提前一拍会踩的那个坑"
    dut._log.info(f"{ALG} 打包 KeyGen：pk+sk 逐字节对上 ACVP，段边界也对")

    # ---- Sign：输入是四段拼起来的，长度不是 4 的倍数，尾巴退回逐字节 ----
    await reset(dut)
    sk, msg, ctx, rnd, sig_w = kat_siggen()
    assert await wr(dut, MODE, mode_word(OP_SIGN, None)) == RESP_OKAY
    assert await wr(dut, MSG_LEN, len(msg)) == RESP_OKAY
    assert await wr(dut, CTX_LEN, len(ctx)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY
    await fill_packed(dut, sk + rnd + ctx + msg)
    p, _ = await rd(dut, IN_PTR)
    assert p == len(sk) + len(rnd) + len(ctx) + len(msg), f"IN_PTR = {p}"
    assert await start_and_wait(dut, 400_000, 2_000), "打包 Sign 没跑完"
    n, _ = await rd(dut, OUT_LEN)
    got_sig = await out_bytes_packed(dut, 0, n)
    assert got_sig == sig_w, diff(got_sig, sig_w, f"{ALG} 打包读 σ 与 ACVP")
    dut._log.info(f"{ALG} 打包 Sign：σ {n}B 逐字节对上 ACVP"
                  f"（{n % 4} 字节的尾巴走了逐字节口）")


@cocotb.test()
async def test_packed_read_refuses_short_tail(dut):
    """OUT_DATA4 剩不足 4 字节时回 0 且不推进 OUT_PTR"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    xi, pk_w, sk_w = kat_keygen()[0]
    n = await run_op(dut, OP_KEYGEN, xi)
    assert n == PKL + SKL

    assert await wr(dut, OUT_PTR, n - 2) == RESP_OKAY
    v, resp = await rd(dut, OUT_DATA4)
    assert resp == RESP_OKAY, "剩不足 4 字节应当回 OKAY（RAZ），不是总线错误"
    assert v == 0, f"剩 2 字节时 OUT_DATA4 读到 0x{v:08x}"
    p, _ = await rd(dut, OUT_PTR)
    assert p == n - 2, f"被拒的打包读推进了 OUT_PTR：{n - 2} → {p}"

    tail = bytearray()
    for _ in range(2):
        d, _ = await rd(dut, OUT_DATA)
        tail.append(d & 0xFF)
    assert bytes(tail) == (pk_w + sk_w)[-2:], "退回逐字节读到的尾巴不对"
    dut._log.info("剩不足 4 字节时 OUT_DATA4 回 0、不推进；逐字节仍读得到尾巴")


@cocotb.test()
async def test_packed_write_refused_when_no_room(dut):
    """剩余容量不足 4 字节时 IN_DATA4 回 SLVERR，**且不绕回 0 覆盖开头**

    判 in_room4 而不是 in_full：差 1~3 个字节时 in_full 还不成立，
    一笔打包写下去就绕回开头，而软件看到的是一路 OKAY。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    IN_CAP = 32768
    assert await wr(dut, MODE, mode_word(OP_KEYGEN, None)) == RESP_OKAY
    assert await wr(dut, CTRL, C_CLEAR) == RESP_OKAY

    # 先用打包写填到只差 2 个字节
    head = bytes([0x5A, 0x5B, 0x5C, 0x5D])
    await fill_packed(dut, head)
    for _ in range((IN_CAP - 4 - 2) // 4):
        assert await wr(dut, IN_DATA4, 0x11223344) == RESP_OKAY
    for _ in range((IN_CAP - 4 - 2) % 4):
        assert await wr(dut, IN_DATA, 0x77) == RESP_OKAY
    p, _ = await rd(dut, IN_PTR)
    assert p == IN_CAP - 2, f"IN_PTR = {p}，应当是 {IN_CAP - 2}"

    assert await wr(dut, IN_DATA4, 0xDEADBEEF) == RESP_SLVERR, \
        "剩余容量不足 4 字节时 IN_DATA4 应当回 SLVERR"
    p2, _ = await rd(dut, IN_PTR)
    assert p2 == IN_CAP - 2, f"被拒的打包写推进了 IN_PTR：{p} → {p2}"

    # 开头那 4 个字节必须还是原样 —— 绕回覆盖正是要挡的东西
    assert await wr(dut, IN_PTR, 0) == RESP_OKAY
    dut._log.info("剩余容量不足 4 字节时 IN_DATA4 被拒，IN_PTR 一步没动")
