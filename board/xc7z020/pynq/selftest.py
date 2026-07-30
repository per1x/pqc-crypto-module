#!/usr/bin/env python3
"""上板自检：确认 overlay 里的加速器真的在按寄存器契约干活

四项检查，从"总线通不通"一路走到"结果对不对"，顺序是刻意的：前一项不过，
后一项的失败信息没有意义。

    1. VERSION 读到 0x00010000
       只验证控制面。读不到这个常量说明比特流没加载、地址不对、或者
       .hwh 与 .bit 不配套 —— 此时任何数据面的结论都不必看。
    2. 全零态 Keccak-f[1600]
       验证 200 字节这条数据通路。比对的是**整个 200 字节状态**，不是只看
       首个 lane 的 0xF1258F7940E1DDE7：只对首 lane 的话，后面 24 个 lane
       全错也能过。参考值由 hardware/model/ref_model.py 现算。
    3. 256 点 NTT 正变换
       验证 512 字节这条数据通路，以及"两个 16 位系数打一个 32 位字"的字节序。
    4. 未实现的操作码返回 ERRCODE=3
       验证失败路径。加速器只实现了 7/8/9 三个操作码，其余必须如实报错而不是
       算出点什么来 —— 这一项不过意味着"看起来能用"的操作码里有假货。

参考值的算法本身也要有人验：--refs-only 只跑第 2、3 项的参考值计算，
不碰硬件，可以在开发机上直接执行。Keccak 那份参考值额外用 hashlib 的
SHAKE128 交叉验证过整个置换，NTT 那份用正逆变换的往返关系验证。

用法
    在板上（需要 root，PYNQ 加载比特流要写 /dev/xdevcfg 等设备）：
        sudo python3 selftest.py --bitstream pqc_accel_bd_wrapper.bit
    在开发机上只验参考值：
        python3 selftest.py --refs-only

退出码等于失败项数，0 表示全部通过。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import random
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import pqc_accel  # noqa: E402  同目录模块，须在 sys.path 调整之后导入


# ---- 参考模型 ----


def load_ref_model(extra_dir: str | None = None):
    """导入 hardware/model/ref_model.py

    优先用命令行给的目录，其次是仓库里的 hardware/model，最后是脚本同目录 ——
    板上未必有整个仓库，把 ref_model.py 单独拷到脚本旁边也应该能跑。
    """
    candidates = []
    if extra_dir:
        candidates.append(extra_dir)
    candidates.append(os.path.join(_REPO_ROOT, "hardware", "model"))
    candidates.append(_HERE)
    for d in candidates:
        if os.path.isfile(os.path.join(d, "ref_model.py")):
            if d not in sys.path:
                sys.path.insert(0, d)
            import ref_model
            return ref_model
    raise SystemExit(
        "找不到 ref_model.py。它在仓库的 hardware/model/ 下；板上没有整个仓库时，"
        "把这一个文件拷到 selftest.py 旁边，或者用 --ref-model <目录> 指出位置。\n"
        "已找过：" + "、".join(candidates))


# ---- 结果收集 ----


class Report:
    """逐项打印通过/失败，最后给一行汇总"""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        tag = "[ 通过 ]" if ok else "[ 失败 ]"
        line = f"{tag} {name}"
        if detail:
            line += f" —— {detail}"
        print(line, flush=True)
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        return ok

    def error(self, name: str, exc: BaseException) -> None:
        self.check(name, False, f"{type(exc).__name__}: {exc}")

    def summary(self) -> int:
        print("-" * 64)
        total = self.passed + self.failed
        if self.failed:
            print(f"结果：{total} 项中 {self.failed} 项失败")
        else:
            print(f"结果：{total} 项全部通过")
        return self.failed


# ---- 参考值：不依赖硬件，开发机上可跑 ----


def keccak_reference(ref_model) -> bytes:
    """全零态过一次 Keccak-f[1600]，返回 200 字节结果"""
    lanes = ref_model.keccak_f1600([0] * pqc_accel.KECCAK_LANES)
    return pqc_accel.lanes_to_bytes(lanes)


def check_keccak_reference(ref_model, rep: Report) -> bytes:
    """验证 Keccak 参考值本身可信，返回全零态的置换结果

    两道验证：
      - 全零态置换后的首个 lane 必须是公开常量 0xF1258F7940E1DDE7；
      - 用同一个置换按 FIPS 202 的海绵结构手工算一遍 SHAKE128("")，与 hashlib
        的结果比对。这一道覆盖全部 25 个 lane：SHAKE128 的率是 168 字节，
        一次挤出正好取走置换后状态的前 168 字节，其中 21 个 lane 逐字节参与比较，
        任何一个 lane 错了都会被发现。首 lane 那个常量只能证明 1/25。
    """
    want_first_lane = 0xF1258F7940E1DDE7
    zero_state = keccak_reference(ref_model)
    first_lane = pqc_accel.bytes_to_lanes(zero_state)[0]
    rep.check("参考值：全零态置换的首个 lane 等于公开常量",
              first_lane == want_first_lane,
              f"得到 0x{first_lane:016X}，期望 0x{want_first_lane:016X}")

    # SHAKE128("")：率 168 字节，pad10*1 的域分隔符 0x1F，末位补 0x80
    rate = 168
    block = bytearray(pqc_accel.KECCAK_LEN)
    block[0] ^= 0x1F
    block[rate - 1] ^= 0x80
    permuted = pqc_accel.lanes_to_bytes(
        ref_model.keccak_f1600(pqc_accel.bytes_to_lanes(bytes(block))))
    got = permuted[:rate]
    want = hashlib.shake_128(b"").digest(rate)
    rep.check("参考值：用同一个置换手工算的 SHAKE128(\"\") 与 hashlib 一致",
              got == want,
              f"前 8 字节 {got[:8].hex()} / {want[:8].hex()}")
    return zero_state


def ntt_reference(ref_model, seed: int = 20260731):
    """造一组确定的系数并算出正变换的参考结果

    取值范围 [-1664, 1664] 即 (-q/2, q/2)，与 cocotb 测试里的构造一致：
    参考实现的 NTT 只保证输入落在这个范围内时中间结果不溢出。
    """
    rng = random.Random(seed)
    coeffs = [rng.randrange(-1664, 1665) for _ in range(pqc_accel.NTT_COEFFS)]
    return coeffs, ref_model.ntt(list(coeffs))


def check_ntt_reference(ref_model, rep: Report):
    """验证 NTT 参考值与字节打包本身可信，返回（输入系数，正变换参考结果）

    三道验证：
      - pack/unpack 往返恒等，说明送进硬件的字节串与拿回来的解读是同一套约定；
      - 正变换结果落在 16 位有符号范围内，否则打包时就会被截断；
      - 正逆变换往返满足 invntt(ntt(x)) ≡ x·2^16 (mod q)。参考实现的 invntt
        末尾乘的是 f = mont²/128，所以往返**不是**恒等 —— 这一条既验证了
        ntt() 的实现，也把这个容易被误判成 bug 的约定钉住。
    """
    coeffs, want = ntt_reference(ref_model)

    packed = pqc_accel.pack_coeffs(coeffs)
    rep.check("参考值：系数打包/解包往返恒等",
              len(packed) == pqc_accel.NTT_LEN
              and pqc_accel.unpack_coeffs(packed) == coeffs,
              f"{len(packed)} 字节")

    in_range = all(-0x8000 <= c <= 0x7FFF for c in want)
    rep.check("参考值：正变换结果在 16 位有符号范围内", in_range,
              f"极值 {min(want)} .. {max(want)}")

    q, mont = ref_model.Q, ref_model.MONT
    back = ref_model.invntt(list(want))
    roundtrip = all((a * mont - b) % q == 0 for a, b in zip(coeffs, back))
    rep.check("参考值：正逆变换往返满足 invntt(ntt(x)) ≡ x·2^16 (mod q)",
              roundtrip)
    return coeffs, want


# ---- 硬件检查 ----


def check_version(accel, rep: Report) -> bool:
    """读 VERSION。不过就不必再往下走。"""
    try:
        got = accel.version
    except Exception as exc:
        rep.error("硬件：读 VERSION", exc)
        return False
    ok = got == pqc_accel.VERSION_EXPECTED
    return rep.check(
        "硬件：VERSION 常量", ok,
        f"读到 0x{got:08X}，期望 0x{pqc_accel.VERSION_EXPECTED:08X}"
        + ("" if ok else "；控制面就没通，后面各项的失败信息无意义"))


def check_keccak(accel, want: bytes, rep: Report) -> None:
    """全零态 Keccak-f[1600]，与参考值逐字节比对"""
    try:
        got = accel.keccak_f1600(bytes(pqc_accel.KECCAK_LEN))
    except Exception as exc:
        rep.error("硬件：Keccak-f[1600] 全零态", exc)
        return
    if got == want:
        rep.check("硬件：Keccak-f[1600] 全零态 200 字节全等", True,
                  f"首个 lane 0x{pqc_accel.bytes_to_lanes(got)[0]:016X}")
        return
    diff = [i for i in range(len(want)) if got[i] != want[i]]
    rep.check("硬件：Keccak-f[1600] 全零态 200 字节全等", False,
              f"{len(diff)} 个字节不符，首个差异在偏移 {diff[0]}："
              f"得到 0x{got[diff[0]]:02X}，期望 0x{want[diff[0]]:02X}")


def check_ntt(accel, coeffs, want, rep: Report) -> None:
    """256 点正变换，与参考值逐系数比对"""
    try:
        got = accel.ntt(coeffs)
    except Exception as exc:
        rep.error("硬件：NTT 正变换", exc)
        return
    if got == want:
        rep.check("硬件：NTT 正变换 256 个系数全等", True,
                  f"轮询 STATUS {accel.last_poll_count} 次")
        return
    diff = [i for i in range(len(want)) if got[i] != want[i]]
    rep.check("硬件：NTT 正变换 256 个系数全等", False,
              f"{len(diff)} 个系数不符，首个差异在下标 {diff[0]}："
              f"得到 {got[diff[0]]}，期望 {want[diff[0]]}")


def check_unsupported_mode(accel, rep: Report) -> None:
    """故意发一个没实现的操作码，必须得到 ERR 且 ERRCODE=3

    取 ACCEL_MODE_KEM_KEYGEN（1）：它在软件侧是有效操作码、有软件后端实现，
    但硬件没有。挑一个纯粹的非法值（比如 0xFF）也能触发同一条路径，
    但用真实存在的操作码更贴近实际会发生的误用。
    """
    mode = 1
    payload = bytes(pqc_accel.KECCAK_LEN)
    try:
        accel.run(mode, payload, pqc_accel.KECCAK_LEN)
    except pqc_accel.PqcAccelCommandError as exc:
        rep.check("硬件：未实现的操作码返回 ERRCODE=3",
                  exc.errcode == pqc_accel.ERRCODE_MODE_NOT_IMPLEMENTED,
                  f"MODE={mode} 得到 ERRCODE={exc.errcode}，"
                  f"期望 {pqc_accel.ERRCODE_MODE_NOT_IMPLEMENTED}")
        return
    except Exception as exc:
        rep.error("硬件：未实现的操作码返回 ERRCODE=3", exc)
        return
    rep.check("硬件：未实现的操作码返回 ERRCODE=3", False,
              f"MODE={mode} 竟然成功返回了结果 —— 失败路径没有如实报错")


# ---- 入口 ----


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="XC7Z020 加速器的 PYNQ 上板自检")
    ap.add_argument("--bitstream", default="pqc_accel_bd_wrapper.bit",
                    help="overlay 的 .bit 路径，同目录须有同名 .hwh")
    ap.add_argument("--accel-name", default="pqc_accel_0",
                    help="block design 里加速器实例名")
    ap.add_argument("--dma-name", default="axi_dma_0",
                    help="block design 里 AXI-DMA 实例名")
    ap.add_argument("--timeout", type=float, default=1.0,
                    help="单次等待的时限（秒）")
    ap.add_argument("--ref-model", default=None,
                    help="ref_model.py 所在目录")
    ap.add_argument("--refs-only", action="store_true",
                    help="只跑参考值自检，不碰硬件（开发机上用）")
    args = ap.parse_args(argv)

    ref_model = load_ref_model(args.ref_model)
    rep = Report()

    print("== 参考值自检（不依赖硬件）==")
    keccak_want = check_keccak_reference(ref_model, rep)
    ntt_in, ntt_want = check_ntt_reference(ref_model, rep)

    if args.refs_only:
        print()
        print("只跑了参考值自检；硬件各项需在板上执行 "
              "sudo python3 selftest.py --bitstream <.bit>")
        return rep.summary()

    print()
    print("== 硬件自检（需在板上执行）==")
    try:
        accel = pqc_accel.PqcAccel(args.bitstream,
                                   accel_name=args.accel_name,
                                   dma_name=args.dma_name,
                                   timeout=args.timeout)
    except pqc_accel.PqcAccelError as exc:
        print(f"[ 失败 ] 加载 overlay —— {exc}")
        rep.failed += 1
        return rep.summary()

    try:
        if check_version(accel, rep):
            check_keccak(accel, keccak_want, rep)
            check_ntt(accel, ntt_in, ntt_want, rep)
            check_unsupported_mode(accel, rep)
        else:
            print("VERSION 不对，跳过数据面各项。故障分类见本目录 README。")
    finally:
        accel.close()

    return rep.summary()


if __name__ == "__main__":
    sys.exit(main())
