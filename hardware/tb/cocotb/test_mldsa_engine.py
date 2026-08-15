"""cocotb：mldsa_engine —— 三个整核的**适配层**

============================================================================
【这一层验什么，不验什么】
============================================================================
验的是"**一个字节口 → 三个核各自的原生端口**"这层翻译：
段偏移对不对、ξ/rnd 这两个并行量有没有从缓冲区正确装进寄存器、
输出读口有没有落到正确的核与正确的段、done/busy/out_len 的语义对不对。

**不重复验算法** —— 算法已经由 test_mldsa_{keygen,sign,verify} 的九格矩阵
逐段对 oracle、整体对 ACVP 验过了。但这里的判据仍然拿 **ACVP 的 pk/sk/σ**：
翻译层错一个字节，出来的就不是 KAT 那份，所以 ACVP 同时也是这一层的判据。

⚠️ 用例只经 engine 的对外契约说话（in_we/in_addr/in_data、out_addr/out_data、
   start/done/busy），**从不偷看内部信号** —— 与 mldsa_axi 看到的是同一副面孔。
"""
import os
import sys
from pathlib import Path

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "model"))
from mldsa_oracle import load_kat, _load_records  # noqa: E402

SIGGEN_KAT = Path(__file__).resolve().parents[3] / "vectors" / "mldsa_siggen.kat"
SIGVER_KAT = Path(__file__).resolve().parents[3] / "vectors" / "mldsa_sigver.kat"

ALG = os.environ.get("MLDSA_ALG", "ML-DSA-44")

OP_KEYGEN, OP_SIGN, OP_VERIFY = 0, 1, 2
PSET_OF = {"ML-DSA-44": 0, "ML-DSA-65": 1, "ML-DSA-87": 2}
# FIPS 204 表 2
LEN_OF = {  # alg -> (pk, sk, sig)
    "ML-DSA-44": (1312, 2560, 2420),
    "ML-DSA-65": (1952, 4032, 3309),
    "ML-DSA-87": (2592, 4896, 4627),
}


async def reset(dut):
    dut.rst_n.value = 0
    dut.zeroize.value = 0
    dut.start.value = 0
    dut.op.value = 0
    dut.pset.value = PSET_OF[ALG]
    dut.in_we.value = 0
    dut.in_addr.value = 0
    dut.in_data.value = 0
    dut.msg_len.value = 0
    dut.ctx_len.value = 0
    dut.out_addr.value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)


async def load_bytes(dut, data: bytes, base: int = 0):
    """按字节灌输入缓冲（engine 空闲时才收）"""
    for i, b in enumerate(data):
        dut.in_we.value = 1
        dut.in_addr.value = base + i
        dut.in_data.value = b
        await RisingEdge(dut.clk)
    dut.in_we.value = 0
    await RisingEdge(dut.clk)


async def run_op(dut, op: int, msg_len: int = 0, ctx_len: int = 0, limit=40_000_000,
                 pset=None):   # 注：venv 是 Python 3.9，不能写 int | None
    """发一次运算并等 done。

    ⚠️ pset=None 时**不碰 dut.pset** —— 运行时切换那条用例是自己在外面设的，
    这里若无条件写回 PSET_OF[ALG] 就会把它覆盖掉（踩过一次：44 过、65 报
    out_len 不对，看着像 RTL 没跟着切，其实是测试自己写回去了）。
    """
    dut.op.value = op
    if pset is not None:
        dut.pset.value = pset
    dut.msg_len.value = msg_len
    dut.ctx_len.value = ctx_len
    dut.start.value = 1
    await RisingEdge(dut.clk)
    dut.start.value = 0
    await Timer(1, unit="ns")
    assert int(dut.done.value) == 0, "start 之后 done 应当已被清掉"
    n = 0
    while int(dut.done.value) != 1:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        n += 1
        assert n < limit, f"engine 超时（op={op}）"
    return n


async def read_out(dut, n: int, base: int = 0) -> bytes:
    """输出读口是同步读：摆地址后要等一个上升沿"""
    out = bytearray()
    for i in range(n):
        dut.out_addr.value = base + i
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        out.append(int(dut.out_data.value) & 0xFF)
    return bytes(out)


def kat_keygen():
    recs = load_kat(limit_per_alg=1)
    rec = next((r for r in recs if r.get("alg") == ALG), None)
    assert rec is not None, f"KAT 里没有 {ALG}"
    return (bytes.fromhex(rec["seed"]), bytes.fromhex(rec["pk"]),
            bytes.fromhex(rec["sk"]))


@cocotb.test()
async def test_engine_keygen_acvp(dut):
    """KeyGen 走 engine 的字节口：pk‖sk 逐字节对上 ACVP

    ξ 在 engine 里是从缓冲区头部读出来装进 256 位寄存器的（核要的是并行口）。
    装错一个字节 → 出来的就不是 KAT 那份，所以这条同时钉住了那段翻译。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    seed, pk_w, sk_w = kat_keygen()
    pk_len, sk_len, _ = LEN_OF[ALG]

    await load_bytes(dut, seed, 0)
    cyc = await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])

    assert int(dut.out_len.value) == pk_len + sk_len, \
        f"out_len={int(dut.out_len.value)}，应当是 {pk_len + sk_len}"

    pk_got = await read_out(dut, pk_len, 0)
    sk_got = await read_out(dut, sk_len, pk_len)
    assert pk_got == pk_w, f"{ALG} pk 与 ACVP 不一致"
    assert sk_got == sk_w, f"{ALG} sk 与 ACVP 不一致"
    dut._log.info(f"engine/{ALG} KeyGen：pk {pk_len}B + sk {sk_len}B 逐字节对上 ACVP，{cyc} cycles")


@cocotb.test()
async def test_engine_done_is_level_and_busy(dut):
    """done 是电平、busy 在跑的时候为高 —— mldsa_axi 的轮询就靠这两条"""
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)
    assert int(dut.done.value) == 0, "复位后 done 必须为 0"
    assert int(dut.busy.value) == 0, "复位后 busy 必须为 0"

    seed, _, _ = kat_keygen()
    await load_bytes(dut, seed, 0)
    await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])

    assert int(dut.busy.value) == 0, "done 之后 busy 必须落下"
    for _ in range(50):
        await RisingEdge(dut.clk)
        assert int(dut.done.value) == 1, "done 掉了 —— 它应当保持到下一次 start"
    dut._log.info("engine：done 是电平语义，busy 跟着运行状态")


@cocotb.test()
async def test_engine_rejects_oversize_len(dut):
    """msg 超过核里 u_msg 的 8192、或 ctx 超过 255 时**拒绝启动**

    这条是反证：没有它的话，超长会让高位地址安静回绕 —— 出来的签名长度对、
    格式对，但是错的，而且 KAT 抓不到（KAT 的消息都很短）。
    拒绝路径也必须给 done，否则软件会一直等。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    seed, pk_w, _ = kat_keygen()
    pk_len, _, _ = LEN_OF[ALG]
    await load_bytes(dut, seed, 0)

    for bad_msg, bad_ctx, why in ((8193, 0, "msg 超过 8192"), (0, 256, "ctx 超过 255")):
        dut.op.value = OP_SIGN
        dut.pset.value = PSET_OF[ALG]
        dut.msg_len.value = bad_msg
        dut.ctx_len.value = bad_ctx
        dut.start.value = 1
        await RisingEdge(dut.clk)
        dut.start.value = 0
        for _ in range(200):
            await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        assert int(dut.done.value) == 1, f"{why}：拒绝路径也要给出 done"
        assert int(dut.busy.value) == 0, f"{why}：被拒绝时不该有核在跑"

    # 被拒绝之后再正常跑一次，结果必须仍然正确（拒绝不能留下坏状态）
    await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])
    assert await read_out(dut, pk_len, 0) == pk_w, "被拒绝一次之后再跑，结果就不对了"
    dut._log.info("engine：超长 msg/ctx 被拒绝且不留坏状态")


@cocotb.test()
async def test_engine_runtime_pset_switch(dut):
    """**同一次仿真里先 44 再 65 再 87，各自对上 ACVP**

    这是"运行时选参数集"的判据。分三次编译各跑一个是证明不了的 ——
    那只说明每个参数集单独对，不说明同一份 bitstream 能在运行时切。
    这条用例**一次复位、一份 RTL**，中途只改 pset 端口，
    三个参数集各跑一遍 KeyGen / Sign / Verify，全部对 ACVP 官方向量。

    ⚠️ 顺序是 44 → 65 → 87，每次都换段偏移、换打包位宽、换 γ₁/γ₂、换循环边界。
       若哪一处还留着上一次的配置（例如段偏移没跟着 pset 变），
       出来的字节就不可能与 ACVP 一致。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    for alg in ("ML-DSA-44", "ML-DSA-65", "ML-DSA-87"):
        ps = PSET_OF[alg]
        pk_len, sk_len, sig_len = LEN_OF[alg]
        dut.pset.value = ps

        # ---- KeyGen ----
        recs = load_kat(limit_per_alg=1)
        rec = next((r for r in recs if r.get("alg") == alg), None)
        assert rec is not None, f"KAT 里没有 {alg}"
        seed = bytes.fromhex(rec["seed"])
        await load_bytes(dut, seed, 0)
        await run_op(dut, OP_KEYGEN)      # pset 由循环外面设，这里不许覆盖
        assert int(dut.out_len.value) == pk_len + sk_len, f"{alg} out_len 不对"
        assert await read_out(dut, pk_len, 0) == bytes.fromhex(rec["pk"]), f"{alg} pk 不对"
        got_sk = await read_out(dut, sk_len, pk_len)
        want_sk = bytes.fromhex(rec["sk"])
        if got_sk != want_sk:
            d = next(i for i in range(sk_len) if got_sk[i] != want_sk[i])
            raise AssertionError(
                f"{alg} sk 第一处不同在 offset {d}（pk_len={pk_len}）："
                f"got={got_sk[d]:02x} want={want_sk[d]:02x}；"
                f"got[0:8]={got_sk[:8].hex()} want[0:8]={want_sk[:8].hex()}")

        # ---- Sign ----
        sg = _kat(SIGGEN_KAT, alg, lambda r: r.get("deterministic") == "1")[0]
        msg = bytes.fromhex(sg["msg"]); ctx = bytes.fromhex(sg.get("context", ""))
        await load_bytes(dut, bytes.fromhex(sg["sk"]) + bytes.fromhex(sg["rnd"])
                         + ctx + msg, 0)
        await run_op(dut, OP_SIGN, msg_len=len(msg), ctx_len=len(ctx))
        assert int(dut.out_len.value) == sig_len, f"{alg} sig out_len 不对"
        assert await read_out(dut, sig_len, 0) == bytes.fromhex(sg["sig"]), \
            f"{alg} σ 与 ACVP siggen 不一致"

        # ---- Verify：pass 与 fail 两种判定 ----
        for want in ("pass", "fail"):
            vr = _kat(SIGVER_KAT, alg, lambda r, w=want: r.get("result") == w)[0]
            vmsg = bytes.fromhex(vr["msg"]); vctx = bytes.fromhex(vr.get("context", ""))
            await load_bytes(dut, bytes.fromhex(vr["pk"]) + bytes.fromhex(vr["sig"])
                             + vctx + vmsg, 0)
            await run_op(dut, OP_VERIFY, msg_len=len(vmsg), ctx_len=len(vctx))
            ok = int(dut.verify_ok.value)
            assert ok == (1 if want == "pass" else 0), \
                f"{alg} sigver 判定错了：期望 {want}"

        dut._log.info(f"  {alg}：KeyGen pk/sk + Sign σ + Verify(pass/fail) 全对上 ACVP")

    dut._log.info("engine：同一次仿真里 44 → 65 → 87 三个参数集全部对上 ACVP "
                  "—— 运行时选参数集成立")


@cocotb.test()
async def test_engine_zeroize_wipes_input_buffer(dut):
    """zeroize 反证：擦除后输入缓冲必须真的是 0

    判据不看内部信号 —— 把缓冲灌满非零，擦一次，再用**同一段地址**跑一次
    KeyGen：若缓冲没被真擦掉，ξ 仍是老种子，pk 就还会是老那份。
    所以"擦完再跑得到的 pk ≠ 老 pk"就是"缓冲确实被清了"的外部可观测证据。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    seed, pk_w, _ = kat_keygen()
    pk_len, _, _ = LEN_OF[ALG]

    await load_bytes(dut, seed, 0)
    await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])
    assert await read_out(dut, pk_len, 0) == pk_w

    # 擦除：拉一拍 zeroize，等 wiping 落下
    dut.zeroize.value = 1
    await RisingEdge(dut.clk)
    await Timer(1, unit="ns")
    assert int(dut.wiping.value) == 1, "zeroize 之后 wiping 应当立刻置起"
    dut.zeroize.value = 0
    n = 0
    while int(dut.wiping.value) == 1:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ns")
        n += 1
        assert n < 200_000, "wiping 一直不落"
    dut._log.info(f"engine：擦除用了 {n} 拍（逐地址写 0，32768 个地址）")

    # 不再灌种子，直接跑：ξ 现在应当是全 0，pk 必然不是老那份
    await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])
    pk_after = await read_out(dut, pk_len, 0)
    assert pk_after != pk_w, \
        "擦除后用同一段地址跑出了同一个 pk —— 说明输入缓冲根本没被擦"

    # 而且确实是"全 0 种子"的那份结果：再灌一次全 0 跑，应当与擦除后一致
    await load_bytes(dut, bytes(32), 0)
    await run_op(dut, OP_KEYGEN, pset=PSET_OF[ALG])
    assert await read_out(dut, pk_len, 0) == pk_after, \
        "擦除后的结果与显式灌全 0 的结果不一致 —— 缓冲没被擦干净"
    dut._log.info("engine：zeroize 逐地址擦除得到外部可观测的反证")


def _kat(path, want_alg, pred=None, limit=1):
    recs = _load_records(path)
    assert recs, f"找不到 {path.name}"
    out = [r for r in recs
           if r.get("alg") == want_alg and (pred is None or pred(r))]
    assert out, f"KAT 里没有符合条件的 {want_alg} 记录"
    return out[:limit]


@cocotb.test()
async def test_engine_sign_acvp(dut):
    """Sign 走 engine 的字节口：σ 逐字节对上 ACVP siggen

    这条才是适配层真正的考验 —— Sign 的输入是**四段拼起来的**：
        sk(SK) ‖ rnd(32) ‖ ctx(ctx_len) ‖ msg(msg_len)
    engine 要把它们分别送到核的 sk_wr / ctx_wr / msg_wr 三个写口（各自独立的
    地址空间，都从 0 开始）加上一个 256 位并行的 rnd。段边界算错一个字节，
    签名就完全不同 —— 所以 ACVP 的 σ 同时钉住了这四段的翻译。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    rec = _kat(SIGGEN_KAT, ALG, lambda r: r.get("deterministic") == "1")[0]
    sk = bytes.fromhex(rec["sk"])
    msg = bytes.fromhex(rec["msg"])
    ctx = bytes.fromhex(rec.get("context", ""))
    rnd = bytes.fromhex(rec["rnd"])
    sig_w = bytes.fromhex(rec["sig"])
    _, sk_len, sig_len = LEN_OF[ALG]
    assert len(sk) == sk_len

    # 按约定排布灌进去：sk ‖ rnd ‖ ctx ‖ msg
    await load_bytes(dut, sk + rnd + ctx + msg, 0)
    cyc = await run_op(dut, OP_SIGN, msg_len=len(msg), ctx_len=len(ctx), pset=PSET_OF[ALG])

    assert int(dut.out_len.value) == sig_len, \
        f"out_len={int(dut.out_len.value)}，应当是 {sig_len}"
    got = await read_out(dut, sig_len, 0)
    assert got == sig_w, f"{ALG} σ 与 ACVP siggen 不一致（|msg|={len(msg)} |ctx|={len(ctx)}）"
    dut._log.info(f"engine/{ALG} Sign：σ {sig_len}B 逐字节对上 ACVP，"
                  f"|msg|={len(msg)} |ctx|={len(ctx)}，{cyc} cycles")


@cocotb.test()
async def test_engine_verify_acvp(dut):
    """Verify 走 engine 的字节口：接受与拒绝**两种判定**都对上 ACVP sigver

    只测通过的那一半是不够的：一个恒返回"通过"的实现也能过。
    所以这里各取一条 pass 与一条 fail。
    """
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    await reset(dut)

    pk_len, _, sig_len = LEN_OF[ALG]
    n = 0
    for want in ("pass", "fail"):
        rec = _kat(SIGVER_KAT, ALG, lambda r, w=want: r.get("result") == w)[0]
        pk = bytes.fromhex(rec["pk"])
        sig = bytes.fromhex(rec["sig"])
        msg = bytes.fromhex(rec["msg"])
        ctx = bytes.fromhex(rec.get("context", ""))
        assert len(pk) == pk_len and len(sig) == sig_len

        await load_bytes(dut, pk + sig + ctx + msg, 0)
        await run_op(dut, OP_VERIFY, msg_len=len(msg), ctx_len=len(ctx), pset=PSET_OF[ALG])
        ok = int(dut.verify_ok.value)
        assert ok == (1 if want == "pass" else 0), \
            f"{ALG} sigver 判定错了：期望 {want}，verify_ok={ok}"
        n += 1
    dut._log.info(f"engine/{ALG} Verify：{n} 条（pass + fail）判定都对上 ACVP sigver")
